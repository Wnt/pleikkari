// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// PLE-602: original integration code; Oculus headers/binaries are locally supplied.
#include <jni.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <VrApi.h>
#include <VrApi_Helpers.h>
#include <VrApi_Input.h>
#include "vr-environment.h" // PLE-603: the room around the screen (plain GLES, no VrApi)
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <memory>
#include <vector>

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "GoCinema", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "GoCinema", __VA_ARGS__)
#define JNI_METHOD(name) Java_fi_madekivi_pleikkari_stream_VrCinemaNative_##name

namespace {
constexpr int Segments = 64;
constexpr float Radius = 3.0f;
constexpr float Arc = 1.4f; // 80 degrees wide, 3 m away; arc length preserves 16:9.
struct Eye {
    ovrTextureSwapChain *chain = nullptr;
    std::vector<GLuint> fbos;
    // PLE-615: the same swapchain images rendered through 4x MSAA (resolved on tile store,
    // GL_EXT_multisampled_render_to_texture, as Skybox does). Empty without the extension.
    std::vector<GLuint> msaaFbos;
    int index = 0;
};

GLuint shader(GLenum type, const char *source) {
    GLuint id = glCreateShader(type);
    glShaderSource(id, 1, &source, nullptr);
    glCompileShader(id);
    GLint ok = 0;
    glGetShaderiv(id, GL_COMPILE_STATUS, &ok);
    if(!ok) {
        char log[2048]; glGetShaderInfoLog(id, sizeof(log), nullptr, log);
        LOGE("Shader compile: %s", log); glDeleteShader(id); return 0;
    }
    return id;
}

GLuint program(bool external) {
    const char *vs = R"(#version 300 es
layout(location=0) in vec3 position;
layout(location=1) in vec2 uv;
uniform mat4 mvp;
uniform mat4 textureTransform;
out highp vec2 texcoord;
void main() {
    gl_Position = mvp * vec4(position, 1.0);
    texcoord = (textureTransform * vec4(uv, 0.0, 1.0)).xy;
})";
    const char *oes = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision mediump float;
uniform samplerExternalOES picture;
in highp vec2 texcoord;
out vec4 color;
void main() { color = vec4(texture(picture, texcoord).rgb, 1.0); }
)";
    const char *plain = R"(#version 300 es
precision mediump float;
uniform sampler2D picture;
in highp vec2 texcoord;
out vec4 color;
void main() { color = vec4(texture(picture, texcoord).rgb, 1.0); }
)";
    GLuint v = shader(GL_VERTEX_SHADER, vs), f = shader(GL_FRAGMENT_SHADER, external ? oes : plain);
    if(!v || !f) { if(v) glDeleteShader(v); if(f) glDeleteShader(f); return 0; }
    GLuint p = glCreateProgram();
    glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p);
    glDeleteShader(v); glDeleteShader(f);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if(!ok) { LOGE("Program link failed"); glDeleteProgram(p); return 0; }
    return p;
}

// VrApi matrices are row-major; GLES requires column-major and transpose=GL_FALSE.
void columnMajor(float out[16], const ovrMatrix4f &matrix) {
    for(int row = 0; row < 4; ++row)
        for(int col = 0; col < 4; ++col) out[col * 4 + row] = matrix.M[row][col];
}

void matrixUniform(GLint location, const ovrMatrix4f &matrix) {
    float columns[16];
    columnMajor(columns, matrix);
    glUniformMatrix4fv(location, 1, GL_FALSE, columns);
}

struct Cinema {
    ovrJava java{};
    bool initialized = false;
    ANativeWindow *window = nullptr;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface pbuffer = EGL_NO_SURFACE;
    ovrMobile *vr = nullptr;
    Eye eyes[2];
    GLuint video = 0, message = 0, videoProgram = 0, messageProgram = 0, vao = 0, vertices = 0;
    int width = 0, height = 0;
    long long frameIndex = 0;
    bool recenterPending = true;
    ovrMatrix4f screen = ovrMatrix4f_CreateIdentity();
    unsigned previousButtons = 0;
    // PLE-603: the environment around the screen; null while the setting is "plain", so
    // the default picture is this file's own path, untouched.
    PleikkariVrEnvironment *environment = nullptr;
    long long environmentStatsFrame = 0;

    ~Cinema() {
        // Called on the same Java render thread, with its JNIEnv and EGL context still alive.
        if(environment) pleikkari_vr_environment_destroy(environment);
        if(vr) vrapi_LeaveVrMode(vr);
        for(auto &eye : eyes) {
            if(!eye.fbos.empty()) glDeleteFramebuffers(static_cast<GLsizei>(eye.fbos.size()), eye.fbos.data());
            if(!eye.msaaFbos.empty()) glDeleteFramebuffers(static_cast<GLsizei>(eye.msaaFbos.size()), eye.msaaFbos.data());
            if(eye.chain) vrapi_DestroyTextureSwapChain(eye.chain);
        }
        if(video) glDeleteTextures(1, &video);
        if(message) glDeleteTextures(1, &message);
        if(videoProgram) glDeleteProgram(videoProgram);
        if(messageProgram) glDeleteProgram(messageProgram);
        if(vertices) glDeleteBuffers(1, &vertices);
        if(vao) glDeleteVertexArrays(1, &vao);
        if(display != EGL_NO_DISPLAY) {
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if(pbuffer != EGL_NO_SURFACE) eglDestroySurface(display, pbuffer);
            if(context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
            eglTerminate(display);
        }
        if(window) ANativeWindow_release(window);
        if(initialized) vrapi_Shutdown();
        if(java.ActivityObject) java.Env->DeleteGlobalRef(java.ActivityObject);
    }

    // environmentSamples: PLE-615's 4x unless the debug preview asks otherwise (PLE-653).
    bool init(JNIEnv *env, jobject activity, jobject surface, float refreshHz, GLsizei environmentSamples) {
        env->GetJavaVM(&java.Vm);
        java.Env = env;
        java.ActivityObject = env->NewGlobalRef(activity);
        ovrInitParms initParms = vrapi_DefaultInitParms(&java);
        if(vrapi_Initialize(&initParms) != VRAPI_INITIALIZE_SUCCESS) { LOGE("vrapi_Initialize failed"); return false; }
        initialized = true;
        display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if(display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr)) return false;
        // Do not use eglChooseConfig: Android's force-MSAA preference can rewrite its result.
        EGLint count = 0;
        if(!eglGetConfigs(display, nullptr, 0, &count)) return false;
        std::vector<EGLConfig> configs(count);
        if(!eglGetConfigs(display, configs.data(), count, &count)) return false;
        EGLConfig config = nullptr;
        for(auto candidate : configs) {
            auto attr = [&](EGLint key) { EGLint value = 0; eglGetConfigAttrib(display, candidate, key, &value); return value; };
            if((attr(EGL_RENDERABLE_TYPE) & EGL_OPENGL_ES3_BIT_KHR) &&
               (attr(EGL_SURFACE_TYPE) & (EGL_WINDOW_BIT | EGL_PBUFFER_BIT)) == (EGL_WINDOW_BIT | EGL_PBUFFER_BIT) &&
               attr(EGL_RED_SIZE) == 8 && attr(EGL_GREEN_SIZE) == 8 && attr(EGL_BLUE_SIZE) == 8 &&
               attr(EGL_ALPHA_SIZE) == 8 && attr(EGL_DEPTH_SIZE) == 0 && attr(EGL_STENCIL_SIZE) == 0 && attr(EGL_SAMPLES) == 0) {
                config = candidate; break;
            }
        }
        if(!config) { LOGE("No VrApi-compatible EGL config"); return false; }
        const EGLint contextAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttrs);
        const EGLint bufferAttrs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        pbuffer = eglCreatePbufferSurface(display, config, bufferAttrs);
        if(context == EGL_NO_CONTEXT || pbuffer == EGL_NO_SURFACE || !eglMakeCurrent(display, pbuffer, pbuffer, context)) return false;
        window = ANativeWindow_fromSurface(env, surface);
        if(!window) return false;
        ovrModeParms mode = vrapi_DefaultModeParms(&java);
        mode.Flags |= VRAPI_MODE_FLAG_NATIVE_WINDOW;
        mode.Display = reinterpret_cast<size_t>(display);
        mode.WindowSurface = reinterpret_cast<size_t>(window);
        mode.ShareContext = reinterpret_cast<size_t>(context);
        vr = vrapi_EnterVrMode(&mode);
        if(!vr) { LOGE("vrapi_EnterVrMode failed"); return false; }
        if(vrapi_SetDisplayRefreshRate(vr, refreshHz) < 0) { LOGE("Runtime refused %.0f Hz", refreshHz); return false; }
        vrapi_SetClockLevels(vr, 2, 2);
        width = vrapi_GetSystemPropertyInt(&java, VRAPI_SYS_PROP_SUGGESTED_EYE_TEXTURE_WIDTH);
        height = vrapi_GetSystemPropertyInt(&java, VRAPI_SYS_PROP_SUGGESTED_EYE_TEXTURE_HEIGHT);
        if(width <= 0 || height <= 0) return false;
        // PLE-615: MSAA only for the environments; the plain screen keeps its single-sample FBOs.
        auto attachMultisample = reinterpret_cast<PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC>(
            eglGetProcAddress("glFramebufferTexture2DMultisampleEXT"));
        const char *extensions = reinterpret_cast<const char *>(glGetString(GL_EXTENSIONS));
        GLint maxSamples = 0;
        if(extensions && strstr(extensions, "GL_EXT_multisampled_render_to_texture")) glGetIntegerv(GL_MAX_SAMPLES_EXT, &maxSamples);
        const GLsizei samples = std::min<GLsizei>(environmentSamples, maxSamples);
        if(!attachMultisample || samples < 2) { attachMultisample = nullptr; LOGI("No multisampled render-to-texture; environments render without MSAA"); }
        for(auto &eye : eyes) {
            eye.chain = vrapi_CreateTextureSwapChain3(VRAPI_TEXTURE_TYPE_2D, GL_RGBA8, width, height, 1, 3);
            if(!eye.chain) return false;
            eye.fbos.resize(vrapi_GetTextureSwapChainLength(eye.chain));
            if(eye.fbos.empty()) return false;
            glGenFramebuffers(static_cast<GLsizei>(eye.fbos.size()), eye.fbos.data());
            for(size_t i = 0; i < eye.fbos.size(); ++i) {
                GLuint tex = vrapi_GetTextureSwapChainHandle(eye.chain, static_cast<int>(i));
                glBindTexture(GL_TEXTURE_2D, tex);
                textureParams(GL_TEXTURE_2D);
                glBindFramebuffer(GL_FRAMEBUFFER, eye.fbos[i]);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
                if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return false;
            }
            if(attachMultisample) {
                eye.msaaFbos.resize(eye.fbos.size());
                glGenFramebuffers(static_cast<GLsizei>(eye.msaaFbos.size()), eye.msaaFbos.data());
                for(size_t i = 0; i < eye.msaaFbos.size(); ++i) {
                    glBindFramebuffer(GL_FRAMEBUFFER, eye.msaaFbos[i]);
                    attachMultisample(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                        vrapi_GetTextureSwapChainHandle(eye.chain, static_cast<int>(i)), 0, samples);
                    if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                        LOGE("Multisampled eye framebuffer incomplete; environments render without MSAA");
                        glDeleteFramebuffers(static_cast<GLsizei>(eye.msaaFbos.size()), eye.msaaFbos.data());
                        eye.msaaFbos.clear();
                        break;
                    }
                }
            }
        }
        while(glGetError() != GL_NO_ERROR) {} // a refused MSAA attachment is not an init failure
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        videoProgram = program(true); messageProgram = program(false);
        if(!videoProgram || !messageProgram) return false;
        glGenTextures(1, &video); glBindTexture(GL_TEXTURE_EXTERNAL_OES, video); textureParams(GL_TEXTURE_EXTERNAL_OES);
        glGenTextures(1, &message); glBindTexture(GL_TEXTURE_2D, message); textureParams(GL_TEXTURE_2D);
        std::vector<float> mesh;
        for(int x = 0; x <= Segments; ++x) {
            const float u = static_cast<float>(x) / Segments;
            const float angle = (u - 0.5f) * Arc;
            for(int y = 0; y < 2; ++y) {
                mesh.insert(mesh.end(), {Radius * std::sin(angle),
                    (y == 0 ? -0.5f : 0.5f) * Radius * Arc * 9.0f / 16.0f,
                    -Radius * std::cos(angle), u, static_cast<float>(y)});
            }
        }
        glGenVertexArrays(1, &vao); glBindVertexArray(vao);
        glGenBuffers(1, &vertices); glBindBuffer(GL_ARRAY_BUFFER, vertices);
        glBufferData(GL_ARRAY_BUFFER, mesh.size() * sizeof(float), mesh.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), nullptr);
        glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), reinterpret_cast<void *>(3 * sizeof(float)));
        glBindVertexArray(0);
        LOGI("VrApi cinema entered; requested %.0f Hz, eye %dx%d, curved screen 3 m / 80 degrees, environment MSAA %dx",
            refreshHz, width, height, eyes[0].msaaFbos.empty() || eyes[1].msaaFbos.empty() ? 1 : static_cast<int>(samples));
        return glGetError() == GL_NO_ERROR;
    }

    // PLE-603: apply the environment setting. PLAIN drops the renderer so nothing of it
    // runs per frame; anything else builds or reconfigures it on this GL thread. A room
    // that fails to build logs and falls back to the plain screen rather than failing VR.
    void setEnvironment(const PleikkariVrEnvironmentConfig &requested) {
        PleikkariVrEnvironmentConfig config = requested;
        pleikkari_vr_environment_config_clamp(&config);
        if(config.environment == PLEIKKARI_VR_ENVIRONMENT_PLAIN) {
            if(environment) { pleikkari_vr_environment_destroy(environment); environment = nullptr; }
            LOGI("Environment plain: black plus the 3 m / 80 degree screen");
            return;
        }
        if(environment) pleikkari_vr_environment_set_config(environment, &config);
        else environment = pleikkari_vr_environment_create(&config, GL_TEXTURE_EXTERNAL_OES);
        if(!environment) { LOGE("Environment %s failed to build; keeping the plain screen", pleikkari_vr_environment_name(config.environment)); return; }
        LOGI("Environment %s: screen %.2f m away, %.2f m wide, curve radius %.2f m, %.2f m above eyes, glow %.2f, room light %.2f",
            pleikkari_vr_environment_name(config.environment), config.screen_distance_m, config.screen_width_m,
            config.screen_curve_radius_m, config.screen_height_offset_m, config.glow, config.room_light);
    }

    static void textureParams(GLenum target) {
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    int input() {
        unsigned buttons = 0;
        float horizontal = 0.5f;
        for(unsigned i = 0; ; ++i) {
            ovrInputCapabilityHeader header{};
            if(vrapi_EnumerateInputDevices(vr, i, &header) < 0) break;
            if(header.Type != ovrControllerType_TrackedRemote) continue; // Never steal the Bluetooth pad.
            ovrInputStateTrackedRemote state{};
            state.Header.ControllerType = ovrControllerType_TrackedRemote;
            if(vrapi_GetCurrentInputState(vr, header.DeviceID, &state.Header) < 0) continue;
            buttons |= state.Buttons;
            ovrInputTrackedRemoteCapabilities caps{};
            caps.Header = header;
            if(vrapi_GetInputDeviceCapabilities(vr, &caps.Header) >= 0 && caps.TrackpadMaxX > 0)
                horizontal = state.TrackpadPosition.x / static_cast<float>(caps.TrackpadMaxX);
        }
        const unsigned pressed = buttons & ~previousButtons;
        previousButtons = buttons;
        int action = (pressed & ovrButton_Back) ? 1 : 0;
        if(pressed & ovrButton_Enter) {
            action |= 2;
            if(horizontal >= 1.0f / 3.0f && horizontal < 2.0f / 3.0f) action |= 4;
            if(horizontal >= 2.0f / 3.0f) action |= 8;
        }
        return action;
    }

    int draw(const float *textureTransform, bool showVideo, bool menu, bool newFrame) {
        ++frameIndex;
        double time = vrapi_GetPredictedDisplayTime(vr, frameIndex);
        ovrTracking2 tracking = vrapi_GetPredictedTracking2(vr, time);
        if(recenterPending) {
            // Move the screen to the current horizontal gaze without resetting the tracking space.
            const auto &q = tracking.HeadPose.Pose.Orientation;
            float yaw = std::atan2(2.0f * (q.w*q.y + q.x*q.z), 1.0f - 2.0f * (q.y*q.y + q.x*q.x));
            screen = ovrMatrix4f_CreateRotation(0, yaw, 0);
            const auto &p = tracking.HeadPose.Pose.Position;
            screen.M[0][3] = p.x; screen.M[1][3] = p.y; screen.M[2][3] = p.z;
            recenterPending = false;
        }
        auto layer = vrapi_DefaultLayerProjection2();
        layer.HeadPose = tracking.HeadPose;
        layer.Header.Flags |= VRAPI_FRAME_LAYER_FLAG_CHROMATIC_ABERRATION_CORRECTION;
        // Android video/Canvas samples already contain display-encoded RGB values.
        layer.Header.Flags |= VRAPI_FRAME_LAYER_FLAG_INHIBIT_SRGB_FRAMEBUFFER;
        GLuint p = showVideo ? videoProgram : messageProgram;
        glUseProgram(p);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(showVideo ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D, showVideo ? video : message);
        glUniform1i(glGetUniformLocation(p, "picture"), 0);
        if(showVideo) glUniformMatrix4fv(glGetUniformLocation(p, "textureTransform"), 1, GL_FALSE, textureTransform);
        else {
            // Bitmap rows start at the top; the mesh UV origin is at the bottom.
            auto flip = ovrMatrix4f_CreateIdentity(); flip.M[1][1] = -1; flip.M[1][3] = 1;
            matrixUniform(glGetUniformLocation(p, "textureTransform"), flip);
        }
        glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_BLEND); glDisable(GL_SCISSOR_TEST);
        glBindVertexArray(vao);
        // PLE-603: refresh the room's 8x8 glow map from the video once per new frame (GPU only).
        if(environment) pleikkari_vr_environment_begin_frame(environment, video, textureTransform, showVideo, newFrame);
        for(int eyeIndex = 0; eyeIndex < 2; ++eyeIndex) {
            Eye &eye = eyes[eyeIndex];
            const bool msaa = environment && !eye.msaaFbos.empty();
            glBindFramebuffer(GL_FRAMEBUFFER, msaa ? eye.msaaFbos[eye.index] : eye.fbos[eye.index]);
            glViewport(0, 0, width, height);
            const auto &view = tracking.Eye[eyeIndex].ViewMatrix;
            const auto &projection = tracking.Eye[eyeIndex].ProjectionMatrix;
            if(environment) {
                // The room and its screen sit in the recentred screen space, so their view is
                // view * screen, exactly the strip's model-view. draw_eye clears the eye and
                // draws the room and the picture; this pass then only adds the message strip.
                float roomView[16], eyeProjection[16];
                columnMajor(roomView, ovrMatrix4f_Multiply(&view, &screen));
                columnMajor(eyeProjection, projection);
                pleikkari_vr_environment_draw_eye(environment, roomView, eyeProjection, video, textureTransform, showVideo);
                glUseProgram(p);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(showVideo ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D, showVideo ? video : message);
                glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_BLEND); glDisable(GL_SCISSOR_TEST);
                glBindVertexArray(vao);
            } else {
                glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
            }
            // Menu follows the head so Back always makes it reachable even when looking away.
            auto model = menu ? ovrMatrix4f_CreateFromQuaternion(&tracking.HeadPose.Pose.Orientation) : screen;
            if(menu) {
                const auto &position = tracking.HeadPose.Pose.Position;
                model.M[0][3] = position.x; model.M[1][3] = position.y; model.M[2][3] = position.z;
            }
            auto mv = ovrMatrix4f_Multiply(&view, &model);
            auto mvp = ovrMatrix4f_Multiply(&projection, &mv);
            matrixUniform(glGetUniformLocation(p, "mvp"), mvp);
            if(!environment || !showVideo) glDrawArrays(GL_TRIANGLE_STRIP, 0, (Segments + 1) * 2);
            // Keep an opaque black border for timewarp's out-of-range sampling.
            glEnable(GL_SCISSOR_TEST);
            glScissor(0, 0, width, 1); glClear(GL_COLOR_BUFFER_BIT);
            glScissor(0, height - 1, width, 1); glClear(GL_COLOR_BUFFER_BIT);
            glScissor(0, 0, 1, height); glClear(GL_COLOR_BUFFER_BIT);
            glScissor(width - 1, 0, 1, height); glClear(GL_COLOR_BUFFER_BIT);
            glDisable(GL_SCISSOR_TEST);
            if(environment) {
                // The room's depth never needs to leave the tile (and, with MSAA, never resolves).
                const GLenum depth = GL_DEPTH_ATTACHMENT;
                glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &depth);
            }
            layer.Textures[eyeIndex].ColorSwapChain = eye.chain;
            layer.Textures[eyeIndex].SwapChainIndex = eye.index;
            layer.Textures[eyeIndex].TexCoordsFromTanAngles = ovrMatrix4f_TanAngleMatrixFromProjection(&projection);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindVertexArray(0);
        if(environment && frameIndex - environmentStatsFrame >= 720) {
            // Every 10 s at 72 Hz: the room's GPU cost (GL_EXT_disjoint_timer_query, one frame late).
            PleikkariVrEnvironmentStats stats{};
            pleikkari_vr_environment_stats(environment, &stats);
            LOGI("Environment frame: gpu %.3f ms, %u draws, %u triangles", stats.gpu_ns / 1e6, stats.draw_calls, stats.triangles);
            environmentStatsFrame = frameIndex;
        }
        glFlush();
        const ovrLayerHeader2 *layers[] = {&layer.Header};
        ovrSubmitFrameDescription2 frame{};
        frame.SwapInterval = 1;
        frame.FrameIndex = frameIndex;
        frame.DisplayTime = time;
        frame.LayerCount = 1;
        frame.Layers = layers;
        const int result = vrapi_SubmitFrame2(vr, &frame);
        for(auto &eye : eyes) eye.index = (eye.index + 1) % eye.fbos.size();
        return result;
    }
};
Cinema *cinema(jlong handle) { return reinterpret_cast<Cinema *>(handle); }
} // namespace

extern "C" JNIEXPORT jlong JNICALL JNI_METHOD(create)(JNIEnv *env, jobject, jobject activity, jobject surface, jfloat refreshHz, jint environmentSamples) {
    std::unique_ptr<Cinema> state(new Cinema());
    if(!state->init(env, activity, surface, refreshHz, environmentSamples)) { LOGE("Cinema initialization failed; EGL error 0x%x", eglGetError()); return 0; }
    return reinterpret_cast<jlong>(state.release());
}
extern "C" JNIEXPORT jint JNICALL JNI_METHOD(videoTexture)(JNIEnv *, jobject, jlong h) { return cinema(h)->video; }
extern "C" JNIEXPORT jint JNICALL JNI_METHOD(messageTexture)(JNIEnv *, jobject, jlong h) { return cinema(h)->message; }
extern "C" JNIEXPORT jint JNICALL JNI_METHOD(input)(JNIEnv *, jobject, jlong h) { return cinema(h)->input(); }
extern "C" JNIEXPORT void JNICALL JNI_METHOD(recentre)(JNIEnv *, jobject, jlong h) { cinema(h)->recenterPending = true; }
extern "C" JNIEXPORT jint JNICALL JNI_METHOD(draw)(JNIEnv *env, jobject, jlong h, jfloatArray transform, jboolean video, jboolean menu, jboolean newFrame) {
    float matrix[16];
    env->GetFloatArrayRegion(transform, 0, 16, matrix);
    if(env->ExceptionCheck()) return -1;
    return cinema(h)->draw(matrix, video, menu, newFrame);
}
// PLE-603: Preferences.vrEnvironmentConfig().toNative(), in metres and unit fractions.
extern "C" JNIEXPORT void JNICALL JNI_METHOD(setEnvironment)(JNIEnv *, jobject, jlong h, jint environment, jfloat distance,
        jfloat widthM, jfloat radius, jfloat heightOffset, jfloat glow, jfloat roomLight) {
    PleikkariVrEnvironmentConfig config;
    pleikkari_vr_environment_config_default(&config);
    config.environment = static_cast<PleikkariVrEnvironmentKind>(environment);
    config.screen_distance_m = distance;
    config.screen_width_m = widthM;
    config.screen_curve_radius_m = radius;
    config.screen_height_offset_m = heightOffset;
    config.glow = glow;
    config.room_light = roomLight;
    cinema(h)->setEnvironment(config);
}
extern "C" JNIEXPORT void JNICALL JNI_METHOD(destroy)(JNIEnv *, jobject, jlong h) { delete cinema(h); }
