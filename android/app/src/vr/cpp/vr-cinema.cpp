// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// PLE-602: original integration code; Oculus headers/binaries are locally supplied.
#include <jni.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <android/trace.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <VrApi.h>
#include <VrApi_Helpers.h>
#include <VrApi_Input.h>
#include "vr-environment.h" // PLE-603: the room around the screen (plain GLES, no VrApi)
#include "vr-screen-placement.h"
#include "vr-frame-pacing.h"
#include "vr-ui-layers.h" // PLE-722: the VR UI toolkit's panels, laser and reticle
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <deque>
#include <memory>
#include <time.h>
#include <vector>

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "GoCinema", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "GoCinema", __VA_ARGS__)
// PLE-715: the debug experiments' per-frame pacing trace, apart from the cinema's own lines.
#define LOGP(...) __android_log_print(ANDROID_LOG_INFO, "GoPacing", __VA_ARGS__)
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

// PLE-746: the input-to-photon probe's mean luma of the video picture, on the GPU. One 1x1 pass
// averages a 16x9 grid of the picture (through the SurfaceTexture's transform, as the screen samples
// it) into a single pixel; the render loop reads it back asynchronously (see LumaProbe).
GLuint lumaProgram() {
    // Named apart from program()'s vs/oes/plain: test/vr_shaders_check.py finds shaders by variable name.
    const char *lumaVs = R"(#version 300 es
void main() {
    vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
})";
    const char *lumaFs = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision highp float;
uniform samplerExternalOES picture;
uniform mat4 textureTransform;
out vec4 color;
void main() {
    float sum = 0.0;
    for(int y = 0; y < 9; ++y) {
        for(int x = 0; x < 16; ++x) {
            vec2 uv = vec2((float(x) + 0.5) / 16.0, (float(y) + 0.5) / 9.0);
            sum += dot(texture(picture, (textureTransform * vec4(uv, 0.0, 1.0)).xy).rgb, vec3(0.299, 0.587, 0.114));
        }
    }
    color = vec4(sum / 144.0, 0.0, 0.0, 1.0);
})";
    GLuint v = shader(GL_VERTEX_SHADER, lumaVs), f = shader(GL_FRAGMENT_SHADER, lumaFs);
    if(!v || !f) { if(v) glDeleteShader(v); if(f) glDeleteShader(f); return 0; }
    GLuint p = glCreateProgram();
    glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p);
    glDeleteShader(v); glDeleteShader(f);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if(!ok) { LOGE("Luma program link failed"); glDeleteProgram(p); return 0; }
    return p;
}

// PLE-746: a zero-length atrace section, written only while atrace records this app.
void traceMark(const char *format, long long a, long long b) {
    if(!ATrace_isEnabled()) return;
    char name[128];
    snprintf(name, sizeof(name), format, a, b);
    ATrace_beginSection(name);
    ATrace_endSection();
}

// VrApi matrices are row-major; GLES requires column-major and transpose=GL_FALSE.
void columnMajor(float out[16], const ovrMatrix4f &matrix) {
    for(int row = 0; row < 4; ++row)
        for(int col = 0; col < 4; ++col) out[col * 4 + row] = matrix.M[row][col];
}

// PLE-698: the clock chiaki and the presenter stamp frames with; VrApi's is System.nanoTime.
int64_t monotonicNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

void matrixUniform(GLint location, const ovrMatrix4f &matrix) {
    float columns[16];
    columnMajor(columns, matrix);
    glUniformMatrix4fv(location, 1, GL_FALSE, columns);
}

/**
 * PLE-746: one latched frame's probe record. The luma pass runs in the frame's draw; its 1x1 result
 * goes into a pixel-pack buffer behind a fence and is read once the fence has signalled, a frame or
 * more later, so the render loop never waits for the GPU. The record then goes back to Kotlin, which
 * reports the latch to CinemaFrameLatency with the luma.
 */
struct LumaRecord {
    int64_t bufferTimestampNs = 0, latchedNs = 0, submittedNs = 0, predictedNs = 0;
    int luma = -1;
};

struct LumaProbe {
    static constexpr int Slots = 8;
    struct Slot { GLuint pbo = 0; GLsync fence = nullptr; LumaRecord record; };
    Slot slots[Slots];
    int head = 0, size = 0; // in-flight slots, oldest first
    GLuint program = 0, target = 0, fbo = 0;
    GLint pictureUniform = -1, transformUniform = -1;
    bool requested = false; // a frame was latched for this draw
    LumaRecord next;
    std::deque<LumaRecord> ready; // read back (or skipped, luma -1), waiting for Kotlin

    bool init() {
        program = lumaProgram();
        if(!program) return false;
        pictureUniform = glGetUniformLocation(program, "picture");
        transformUniform = glGetUniformLocation(program, "textureTransform");
        glGenTextures(1, &target);
        glBindTexture(GL_TEXTURE_2D, target);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 1, 1);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
        const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        for(auto &slot : slots) {
            glGenBuffers(1, &slot.pbo);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, slot.pbo);
            glBufferData(GL_PIXEL_PACK_BUFFER, 4, nullptr, GL_STREAM_READ);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        return complete && glGetError() == GL_NO_ERROR;
    }

    void destroy() {
        for(auto &slot : slots) {
            if(slot.fence) glDeleteSync(slot.fence);
            if(slot.pbo) glDeleteBuffers(1, &slot.pbo);
        }
        if(fbo) glDeleteFramebuffers(1, &fbo);
        if(target) glDeleteTextures(1, &target);
        if(program) glDeleteProgram(program);
    }

    /** In the draw, after the eye passes: measure the latched picture into the next free slot. */
    void measure(GLuint video, const float *textureTransform, int64_t submittedNs, int64_t predictedNs) {
        next.submittedNs = submittedNs;
        next.predictedNs = predictedNs;
        requested = false;
        if(size == Slots) { next.luma = -1; ready.push_back(next); return; } // the GPU is 8 frames behind: skip
        Slot &slot = slots[(head + size) % Slots];
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, 1, 1);
        glUseProgram(program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, video);
        glUniform1i(pictureUniform, 0);
        glUniformMatrix4fv(transformUniform, 1, GL_FALSE, textureTransform);
        glDisable(GL_SCISSOR_TEST);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, slot.pbo);
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        slot.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        slot.record = next;
        ++size;
    }

    /** Collect every slot whose fence has signalled, oldest first; never waits. */
    void poll() {
        while(size > 0) {
            Slot &slot = slots[head];
            const GLenum state = glClientWaitSync(slot.fence, 0, 0);
            if(state == GL_TIMEOUT_EXPIRED) break;
            LumaRecord record = slot.record;
            if(state != GL_WAIT_FAILED) {
                glBindBuffer(GL_PIXEL_PACK_BUFFER, slot.pbo);
                auto *pixel = static_cast<const uint8_t *>(glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, 4, GL_MAP_READ_BIT));
                if(pixel) { record.luma = pixel[0]; glUnmapBuffer(GL_PIXEL_PACK_BUFFER); }
                glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            }
            glDeleteSync(slot.fence);
            slot.fence = nullptr;
            ready.push_back(record);
            head = (head + 1) % Slots;
            --size;
        }
    }
};

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
    // PLE-702: when the screen goes to the head's gaze. Besides the first frame and the
    // wearer's recentre: after every recentre the runtime makes by itself (the mount event in
    // the first frame's submit, a long press of the Oculus button), because the screen sits in
    // the LOCAL space it re-bases; and once the head comes level after a placement made looking
    // steeply up or down, such as a Go started on the table and then picked up.
    PleikkariVrScreenPlacement placement;
    // PLE-675: debug builds only. Recentre along the full head orientation (pitch and roll
    // too), so a Go lying on a table still has the screen in view for a display-0 screencap.
    bool fullPoseRecentre = false;
    ovrMatrix4f screen = ovrMatrix4f_CreateIdentity();
    unsigned previousButtons = 0;
    // PLE-603: the environment around the screen; null while the setting is "plain", so
    // the default picture is this file's own path, untouched.
    PleikkariVrEnvironment *environment = nullptr;
    long long environmentStatsFrame = 0;
    // PLE-652: GPU clock level while a room is drawn; 2 (the PLE-623 baseline) unless the A/B setting raises it.
    int roomGpuLevel = 2;
    int skyVariant = PLEIKKARI_VR_SKY_DOME; // PLE-666: debug sky draw, kept across environment rebuilds
    bool depthAttachOnce = false; // PLE-808: A/B, kept across environment rebuilds
    // PLE-698: the last vrapi_SubmitFrame2 call (CLOCK_MONOTONIC ns) and its predicted display time.
    int64_t submitNs = 0;
    int64_t predictedDisplayNs = 0;
    // PLE-715: when each frame starts (after the loop's own sleep, if any) and what VrApi's
    // scheduler did with it; the per-second "Frame pacing" line with the stats log on.
    PleikkariVrPacing pacing;
    // The mode asked for; the late start is for the plain cinema only (see applyPacingMode).
    PleikkariVrPacingMode pacingRequested = PLEIKKARI_VR_PACING_VRAPI;
    // PLE-801: the latch on the frame signal asked for; the plain cinema only too.
    bool latchRequested = false;
    bool pacingLog = false;
    int64_t frameStartNs = 0;
    int64_t frameSleptNs = 0;
    int64_t pacingWindowStartNs = 0;
    // PLE-722: the VR UI's panels and pointer; idle (no layer, no draw) until a panel opens.
    pleikkari::VrUiLayers ui;
    bool uiReady = false;
    pleikkari::VrUiRemote remote;
    // Where the picture was last placed, for panels anchored to it.
    PleikkariVrVec3 screenCentre{0.0f, 0.0f, 0.0f};
    PleikkariVrQuat screenOrientation{0.0f, 0.0f, 0.0f, 1.0f};
    // PLE-746: the input-to-photon probe; null unless stream_go_vr_latency_probe started it.
    std::unique_ptr<LumaProbe> probe;

    Cinema() {
        pleikkari_vr_screen_placement_init(&placement);
        PleikkariVrPacingConfig config;
        pleikkari_vr_pacing_config_default(&config, 72.0f);
        pleikkari_vr_pacing_init(&pacing, &config);
    }

    ~Cinema() {
        // Called on the same Java render thread, with its JNIEnv and EGL context still alive.
        if(initialized) ui.destroy();
        if(probe) probe->destroy();
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
        setPacing(PLEIKKARI_VR_PACING_VRAPI, false, false, nullptr, refreshHz);
        applyClockLevels();
        {
            // PLE-698: per-frame latency mixes both clocks; log how far apart they are (expected ~0).
            const int64_t before = monotonicNs();
            const double vrapiSeconds = vrapi_GetTimeInSeconds();
            const int64_t after = monotonicNs();
            LOGI("VrApi clock minus CLOCK_MONOTONIC: %.3f ms", (vrapiSeconds * 1e9 - static_cast<double>(before + after) / 2) / 1e6);
        }
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
        // PLE-722: a UI that fails to build leaves the cinema as it was (the strip menu still works).
        uiReady = ui.init();
        if(!uiReady) LOGE("VR UI program failed; panels disabled");
        // docs/design/vr-ui.md §11: the eye field of view settles the eye buffer's texel density.
        LOGI("Suggested eye FOV %.1f x %.1f degrees",
            vrapi_GetSystemPropertyFloat(&java, VRAPI_SYS_PROP_SUGGESTED_EYE_FOV_DEGREES_X),
            vrapi_GetSystemPropertyFloat(&java, VRAPI_SYS_PROP_SUGGESTED_EYE_FOV_DEGREES_Y));
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

    // PLE-755: the first frame of a session pays the driver's first-use work (program binaries,
    // texture and swap-chain image allocation, tile setup) and takes 25-34 ms; the late submit
    // followed by a quick one is what puts VrApi's scheduler a refresh ahead (PLE-715). Draw
    // every swap-chain image of both eyes once with both screen programs, and wait for the GPU,
    // before the first submit. Nothing drawn here is submitted: the first frame clears it.
    void warmUp() {
        const int64_t start = monotonicNs();
        const auto identity = ovrMatrix4f_CreateIdentity();
        glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_BLEND); glDisable(GL_SCISSOR_TEST);
        glBindVertexArray(vao);
        glActiveTexture(GL_TEXTURE0);
        for(auto &eye : eyes) {
            for(GLuint fbo : eye.fbos) {
                glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                glViewport(0, 0, width, height);
                glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
                for(GLuint p : {videoProgram, messageProgram}) {
                    const bool isVideo = p == videoProgram;
                    glUseProgram(p);
                    glBindTexture(isVideo ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D, isVideo ? video : message);
                    glUniform1i(glGetUniformLocation(p, "picture"), 0);
                    matrixUniform(glGetUniformLocation(p, "textureTransform"), identity);
                    matrixUniform(glGetUniformLocation(p, "mvp"), identity);
                    glDrawArrays(GL_TRIANGLE_STRIP, 0, (Segments + 1) * 2);
                }
                glClear(GL_COLOR_BUFFER_BIT);
            }
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindVertexArray(0);
        glUseProgram(0);
        glFinish();
        for(GLenum error = glGetError(); error != GL_NO_ERROR; error = glGetError()) LOGE("GL error 0x%x in the warm-up draw", error);
        LOGI("Warm-up draw: %zu + %zu eye images, %.2f ms", eyes[0].fbos.size(), eyes[1].fbos.size(), (monotonicNs() - start) / 1e6);
    }

    // PLE-603: apply the environment setting. PLAIN drops the renderer so nothing of it
    // runs per frame; anything else builds or reconfigures it on this GL thread. A room
    // that fails to build logs and falls back to the plain screen rather than failing VR.
    // PLE-652: CPU stays at 2; the GPU takes roomGpuLevel only while a room is active.
    void applyClockLevels() {
        const int gpu = environment ? roomGpuLevel : 2;
        const ovrResult result = vrapi_SetClockLevels(vr, 2, gpu);
        LOGI("Clock levels CPU 2 / GPU %d (%s)", gpu, result == ovrSuccess ? "accepted" : "refused");
    }

    void setEnvironment(const PleikkariVrEnvironmentConfig &requested) {
        setEnvironmentRenderer(requested);
        applyClockLevels();
        applyPacingMode();
    }

    void setEnvironmentRenderer(const PleikkariVrEnvironmentConfig &requested) {
        PleikkariVrEnvironmentConfig config = requested;
        pleikkari_vr_environment_config_clamp(&config);
        if(config.environment == PLEIKKARI_VR_ENVIRONMENT_PLAIN) {
            if(environment) { pleikkari_vr_environment_destroy(environment); environment = nullptr; }
            LOGI("Environment plain: black plus the 3 m / 80 degree screen");
            return;
        }
        // The room checks glGetError after building; an error left by an earlier call is not its own.
        for(GLenum stale = glGetError(); stale != GL_NO_ERROR; stale = glGetError())
            LOGE("GL error 0x%x pending before the environment change", stale);
        if(environment) pleikkari_vr_environment_set_config(environment, &config);
        else environment = pleikkari_vr_environment_create(&config, GL_TEXTURE_EXTERNAL_OES);
        if(!environment) { LOGE("Environment %s failed to build; keeping the plain screen", pleikkari_vr_environment_name(config.environment)); return; }
        if(skyVariant != PLEIKKARI_VR_SKY_DOME) pleikkari_vr_environment_debug_set_sky_variant(environment, skyVariant);
        if(depthAttachOnce) pleikkari_vr_environment_debug_set_depth_attach_once(environment, true);
        LOGI("Environment %s: screen %.2f m away, %.2f m wide, curve radius %.2f m, %.2f m above eyes, glow %.2f, room light %.2f",
            pleikkari_vr_environment_name(config.environment), config.screen_distance_m, config.screen_width_m,
            config.screen_curve_radius_m, config.screen_height_offset_m, config.glow, config.room_light);
    }

    // PLE-715: mode and the stats log from the settings; spec is a debug build's experiments.
    // PLE-801: latchOnSignal from its setting (or the spec's "signal"); returns whether it is asked for.
    bool setPacing(PleikkariVrPacingMode mode, bool latchOnSignal, bool log, const char *spec, float refreshHz) {
        PleikkariVrPacingConfig config;
        pleikkari_vr_pacing_config_default(&config, refreshHz);
        config.mode = mode;
        config.latch_on_signal = latchOnSignal;
        const int read = pleikkari_vr_pacing_config_parse(&config, spec);
        pleikkari_vr_pacing_init(&pacing, &config);
        pacingRequested = config.mode;
        latchRequested = config.latch_on_signal;
        pacingLog = log || config.trace;
        pacingWindowStartNs = 0;
        if(read || mode != PLEIKKARI_VR_PACING_VRAPI || config.latch_on_signal)
            LOGI("Frame pacing: %s, budget %.1f ms, period %.3f ms%s%s%s", pleikkari_vr_pacing_mode_name(config.mode),
                config.budget_ns / 1e6, config.period_ns / 1e6,
                !config.latch_on_signal ? "" : config.mode == PLEIKKARI_VR_PACING_LATE
                    ? "; latch on the frame signal (inert: the late start already latches at its deadline)" : "; latch on the frame signal",
                read ? "; debug experiments: " : "", read ? spec : "");
        applyPacingMode();
        return config.latch_on_signal;
    }

    // PLE-715: a room's eye frame takes about 8 ms of GPU (VrApi App=, PLE-623), which the late
    // start's budget does not leave, and the rooms were never measured with it: they keep VrApi's
    // release, today's loop. PLE-801: the latch on the frame signal draws within the same budget.
    void applyPacingMode() {
        const PleikkariVrPacingMode mode = pacingRequested == PLEIKKARI_VR_PACING_LATE && environment
            ? PLEIKKARI_VR_PACING_VRAPI : pacingRequested;
        const bool latch = latchRequested && !environment;
        if(pacing.config.mode == mode && pacing.config.latch_on_signal == latch) return;
        const bool modeChanged = pacing.config.mode != mode;
        pacing.config.mode = mode;
        pacing.config.latch_on_signal = latch;
        if(modeChanged)
            LOGI("Frame pacing: %s%s", pleikkari_vr_pacing_mode_name(mode),
                mode != pacingRequested ? " (late start is for the plain cinema; a room is drawn)" : "");
        if(latchRequested)
            LOGI("Frame pacing: latch on the frame signal %s", latch ? "on" : "off (it is for the plain cinema; a room is drawn)");
    }

    // PLE-801: with no new video frame, until when the loop may wait for the decoder's frame signal; 0 latches now.
    int64_t latchDeadline() const { return pleikkari_vr_pacing_latch_deadline_ns(&pacing, monotonicNs()); }

    // PLE-801: the wait is the loop's own sleep; the frame starts at the latch that follows it.
    void latchWaited(int64_t waitStartNs, bool signalled) {
        const int64_t now = monotonicNs();
        pleikkari_vr_pacing_latch_waited(&pacing, now - waitStartNs, signalled);
        frameSleptNs += now - waitStartNs;
        frameStartNs = now;
    }

    // PLE-715: at the top of the loop, before input and the video latch.
    void pace() {
        const int64_t now = monotonicNs();
        const int64_t wake = pleikkari_vr_pacing_wake_ns(&pacing, now);
        if(wake > now) {
            timespec until{static_cast<time_t>(wake / 1000000000LL), static_cast<long>(wake % 1000000000LL)};
            while(clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, nullptr) == EINTR) {}
        }
        frameStartNs = monotonicNs();
        frameSleptNs = frameStartNs - now;
    }

    void recordPacing(int64_t returnedNs) {
        const int64_t start = frameStartNs ? frameStartNs : submitNs;
        pleikkari_vr_pacing_frame(&pacing, start, frameSleptNs, submitNs, returnedNs, predictedDisplayNs);
        if(pacing.config.trace)
            LOGP("F %llu s=%lld z=%lld u=%lld r=%lld p=%lld", static_cast<unsigned long long>(pacing.frames),
                static_cast<long long>(start / 1000), static_cast<long long>(frameSleptNs / 1000),
                static_cast<long long>((submitNs - start) / 1000), static_cast<long long>((returnedNs - submitNs) / 1000),
                static_cast<long long>((predictedDisplayNs - returnedNs) / 1000));
        frameStartNs = 0;
        frameSleptNs = 0;
        if(!pacingLog) return;
        if(!pacingWindowStartNs) pacingWindowStartNs = returnedNs;
        if(returnedNs - pacingWindowStartNs < 1000000000LL) return;
        PleikkariVrPacingWindow w{};
        pleikkari_vr_pacing_take_window(&pacing, &w);
        const double n = w.frames ? w.frames : 1;
        LOGI("Frame pacing (%s): %u frames, %u throttled, %u late | slept mean/max %.2f/%.2f ms | start to submit mean/max %.2f/%.2f ms"
            " | submit wait min/max %.2f/%.2f ms | submit to predicted min/mean/max %.2f/%.2f/%.2f ms"
            " | return to predicted min/max %.2f/%.2f ms | start to predicted mean %.2f ms | leads 0/1/2+ %u/%u/%u, %u drained, period %.3f ms"
            " | VrApi early %d stale %d per s",
            pleikkari_vr_pacing_mode_name(pacing.config.mode), w.frames, w.throttled, w.late,
            w.slept_sum_ns / n / 1e6, w.slept_max_ns / 1e6, w.work_sum_ns / n / 1e6, w.work_max_ns / 1e6,
            w.wait_min_ns / 1e6, w.wait_max_ns / 1e6, w.ahead_min_ns / 1e6, w.ahead_sum_ns / n / 1e6, w.ahead_max_ns / 1e6,
            w.lead_min_ns / 1e6, w.lead_max_ns / 1e6, w.start_to_photon_sum_ns / n / 1e6,
            w.leads[0], w.leads[1], w.leads[2], w.drains, w.period_ns / 1e6,
            vrapi_GetSystemStatusInt(&java, VRAPI_SYS_STATUS_EARLY_FRAMES_PER_SECOND),
            vrapi_GetSystemStatusInt(&java, VRAPI_SYS_STATUS_STALE_FRAMES_PER_SECOND));
        // PLE-801: only with the latch on the frame signal, so today's line stays as it was.
        if(pacing.config.latch_on_signal)
            LOGI("Frame pacing latch on signal: %u of %u frames waited, %u woken by a frame, %u by the deadline"
                " | wait mean/max %.2f/%.2f ms | %u waited and came late (next held)",
                w.latch_waits, w.frames, w.latch_signalled, w.latch_waits - w.latch_signalled,
                w.latch_waits ? w.latch_wait_sum_ns / static_cast<double>(w.latch_waits) / 1e6 : 0.0,
                w.latch_wait_max_ns / 1e6, w.latch_late);
        pacingWindowStartNs = returnedNs;
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
        remote = pleikkari::VrUiRemote{};
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
            const bool hasCaps = vrapi_GetInputDeviceCapabilities(vr, &caps.Header) >= 0;
            if(hasCaps && caps.TrackpadMaxX > 0)
                horizontal = state.TrackpadPosition.x / static_cast<float>(caps.TrackpadMaxX);
            if(!remote.present) {
                // PLE-722: the first remote points; its pose is read in draw() for the frame's own time.
                remote.present = true;
                remote.device = header.DeviceID;
                remote.buttons = state.Buttons;
                remote.touching = state.TrackpadStatus != 0;
                if(hasCaps && caps.TrackpadMaxX > 0 && caps.TrackpadMaxY > 0) {
                    remote.touchX = std::clamp(state.TrackpadPosition.x / static_cast<float>(caps.TrackpadMaxX), 0.0f, 1.0f);
                    remote.touchY = std::clamp(state.TrackpadPosition.y / static_cast<float>(caps.TrackpadMaxY), 0.0f, 1.0f);
                }
            }
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

    // uiControl: PLE-722's panels (null with the strip menu); uiOut receives the pointer's hit.
    int draw(const float *textureTransform, bool showVideo, bool menu, bool newFrame,
             const pleikkari::VrUiControl *uiControl, float *uiOut) {
        ++frameIndex;
        double time = vrapi_GetPredictedDisplayTime(vr, frameIndex);
        ovrTracking2 tracking = vrapi_GetPredictedTracking2(vr, time);
        // PLE-702: about 1 us a read on the Go. The count rises between frames 1 and 2 of every
        // session, so the first placement alone could leave the screen out of view.
        const int recenters = vrapi_GetSystemStatusInt(&java, VRAPI_SYS_STATUS_RECENTER_COUNT);
        const auto &q = tracking.HeadPose.Pose.Orientation;
        // A pitch near +-90 is a Go lying on a table: its view is the room's floor or sky,
        // near-black in the void, and its yaw says nothing about where a wearer will face.
        const float pitch = std::asin(std::clamp(2.0f * (q.w*q.x - q.y*q.z), -1.0f, 1.0f)) * 57.2958f;
        const PleikkariVrScreenPlace place = pleikkari_vr_screen_placement_frame(&placement, recenters, pitch, fullPoseRecentre);
        if(place != PLEIKKARI_VR_SCREEN_KEEP) {
            // Move the screen to the current horizontal gaze without resetting the tracking space.
            float yaw = std::atan2(2.0f * (q.w*q.y + q.x*q.z), 1.0f - 2.0f * (q.y*q.y + q.x*q.x));
            screen = fullPoseRecentre ? ovrMatrix4f_CreateFromQuaternion(&q) : ovrMatrix4f_CreateRotation(0, yaw, 0);
            const auto &p = tracking.HeadPose.Pose.Position;
            screen.M[0][3] = p.x; screen.M[1][3] = p.y; screen.M[2][3] = p.z;
            screenCentre = {p.x, p.y, p.z};
            screenOrientation = fullPoseRecentre ? PleikkariVrQuat{q.x, q.y, q.z, q.w}
                : PleikkariVrQuat{0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
            LOGI("Screen placed at frame %lld (%s): head yaw %.0f, pitch %.0f degrees, runtime recentres %d%s%s",
                frameIndex, pleikkari_vr_screen_place_name(place), yaw * 57.2958f, pitch, recenters,
                fullPoseRecentre ? ", full pose" : "",
                placement.provisional ? "; provisional, placed again once the head is level" : "");
        }
        const bool uiFrame = uiReady && uiControl;
        if(uiFrame) {
            ovrTracking remotePose{};
            const bool posed = remote.present &&
                vrapi_GetInputTrackingState(vr, remote.device, time, &remotePose) == ovrSuccess;
            ui.beginFrame(*uiControl, tracking, time, remote, posed ? &remotePose : nullptr,
                place != PLEIKKARI_VR_SCREEN_KEEP, screenCentre, screenOrientation, fullPoseRecentre);
        }
        const bool panels = uiFrame && ui.active();
        auto layer = vrapi_DefaultLayerProjection2();
        if(panels && !ui.overlay()) {
            // PLE-722: the panels lie under the eye buffer, which lets them through where its alpha is 0.
            layer.Header.SrcBlend = VRAPI_FRAME_LAYER_BLEND_ONE;
            layer.Header.DstBlend = VRAPI_FRAME_LAYER_BLEND_ONE_MINUS_SRC_ALPHA;
        }
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
            if(uiFrame) {
                ui.drawEye(view, projection);
                // The plain screen binds its program and picture once, before the eye loop: put them
                // back, or the next eye draws its picture with the UI's program (PLE-722, seen on the Go).
                glUseProgram(p);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(showVideo ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D, showVideo ? video : message);
                glBindVertexArray(vao);
            }
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
        // PLE-651: stop the room's GPU timer after the strip, border clears and both eyes' resolves.
        if(environment) pleikkari_vr_environment_end_frame(environment);
        if(environment && frameIndex - environmentStatsFrame >= 720) {
            // Every 10 s at 72 Hz: the whole eye frame's GPU cost (GL_EXT_disjoint_timer_query, a few frames late).
            PleikkariVrEnvironmentStats stats{};
            pleikkari_vr_environment_stats(environment, &stats);
            LOGI("Environment frame: gpu %.3f ms, %u draws, %u triangles", stats.gpu_ns / 1e6, stats.draw_calls, stats.triangles);
            environmentStatsFrame = frameIndex;
        }
        // PLE-776: the reticle-layer switch draws the laser and reticle into their own layer, last.
        ovrLayerProjection2 reticle;
        const bool reticleOn = uiFrame && ui.reticleLayer(tracking, width, height, &reticle);
        // PLE-746: after the eyes, so the probe's pass never delays the picture on the GPU.
        const int64_t predictedNs = std::llround(time * 1e9);
        if(probe && probe->requested)
            probe->measure(video, textureTransform, showVideo ? monotonicNs() : 0, showVideo ? predictedNs : 0);
        glFlush();
        ovrLayer_Union2 panelLayers[pleikkari::VrUiMaxPanels];
        const int panelCount = panels ? ui.layers(tracking, panelLayers) : 0;
        const ovrLayerHeader2 *layers[pleikkari::VrUiMaxPanels + 2];
        // PLE-761: the overlay switch submits the panels after the eye buffer instead of before it.
        const bool overlay = panels && ui.overlay();
        layers[overlay ? 0 : panelCount] = &layer.Header;
        for(int i = 0; i < panelCount; ++i) layers[(overlay ? 1 : 0) + i] = &panelLayers[i].Header;
        if(reticleOn) layers[panelCount + 1] = &reticle.Header;
        if(uiFrame && uiOut) ui.output(uiOut);
        ovrSubmitFrameDescription2 frame{};
        frame.SwapInterval = 1;
        frame.FrameIndex = frameIndex;
        frame.DisplayTime = time;
        frame.LayerCount = static_cast<uint32_t>(panelCount + 1 + (reticleOn ? 1 : 0));
        frame.Layers = layers;
        submitNs = monotonicNs();
        predictedDisplayNs = predictedNs;
        const bool traced = probe && ATrace_isEnabled();
        if(traced) {
            traceMark("PLE746 cinema submit frame=%lld predicted_ns=%lld", frameIndex, predictedDisplayNs);
            ATrace_beginSection("PLE746 vrapi_SubmitFrame2");
        }
        const int result = vrapi_SubmitFrame2(vr, &frame);
        const int64_t returnedNs = monotonicNs();
        if(traced) ATrace_endSection();
        recordPacing(returnedNs);
        for(auto &eye : eyes) eye.index = (eye.index + 1) % eye.fbos.size();
        return result;
    }

    // PLE-746: a frame was latched for the next draw; the probe measures its luma in that draw.
    void probeFrame(int64_t bufferTimestampNs, int64_t latchedNs) {
        if(!probe) return;
        probe->requested = true;
        probe->next = LumaRecord{};
        probe->next.bufferTimestampNs = bufferTimestampNs;
        probe->next.latchedNs = latchedNs;
        traceMark("PLE746 cinema latched ts=%lld at_ns=%lld", bufferTimestampNs, latchedNs);
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
// PLE-715: the top of every render loop iteration; sleeps when the pacing mode asks.
extern "C" JNIEXPORT void JNICALL JNI_METHOD(pace)(JNIEnv *, jobject, jlong h) { cinema(h)->pace(); }
// PLE-715: mode 0 is VrApi's own release (the default), 1 the late start; spec a debug build's experiments.
// PLE-801: latchOnSignal is the latch on the frame signal; returns it, or the spec's "signal".
extern "C" JNIEXPORT jboolean JNICALL JNI_METHOD(setPacing)(JNIEnv *env, jobject, jlong h, jint mode, jboolean latchOnSignal,
        jboolean log, jstring spec, jfloat refreshHz) {
    const char *chars = spec ? env->GetStringUTFChars(spec, nullptr) : nullptr;
    const bool latch = cinema(h)->setPacing(mode == PLEIKKARI_VR_PACING_LATE ? PLEIKKARI_VR_PACING_LATE : PLEIKKARI_VR_PACING_VRAPI,
        latchOnSignal, log, chars, refreshHz);
    if(chars) env->ReleaseStringUTFChars(spec, chars);
    return latch;
}
// PLE-801: CLOCK_MONOTONIC ns to wait for the frame signal until before the latch, or 0 to latch now.
extern "C" JNIEXPORT jlong JNICALL JNI_METHOD(latchDeadline)(JNIEnv *, jobject, jlong h) { return cinema(h)->latchDeadline(); }
extern "C" JNIEXPORT void JNICALL JNI_METHOD(latchWaited)(JNIEnv *, jobject, jlong h, jlong waitStartNs, jboolean signalled) {
    cinema(h)->latchWaited(waitStartNs, signalled);
}
extern "C" JNIEXPORT void JNICALL JNI_METHOD(warmUp)(JNIEnv *, jobject, jlong h) { cinema(h)->warmUp(); }
extern "C" JNIEXPORT void JNICALL JNI_METHOD(recentre)(JNIEnv *, jobject, jlong h) { pleikkari_vr_screen_placement_request(&cinema(h)->placement); }
extern "C" JNIEXPORT void JNICALL JNI_METHOD(setFullPoseRecentre)(JNIEnv *, jobject, jlong h, jboolean enabled) {
    cinema(h)->fullPoseRecentre = enabled;
    pleikkari_vr_screen_placement_request(&cinema(h)->placement);
}
// uiControl: null for the strip menu, else StreamVrActivity's VrUiFrame.control; uiOut its output.
extern "C" JNIEXPORT jint JNICALL JNI_METHOD(draw)(JNIEnv *env, jobject, jlong h, jfloatArray transform, jboolean video,
        jboolean menu, jboolean newFrame, jfloatArray uiControl, jfloatArray uiOut) {
    float matrix[16];
    env->GetFloatArrayRegion(transform, 0, 16, matrix);
    if(env->ExceptionCheck()) return -1;
    if(!uiControl) return cinema(h)->draw(matrix, video, menu, newFrame, nullptr, nullptr);
    float in[7];
    env->GetFloatArrayRegion(uiControl, 0, 7, in);
    if(env->ExceptionCheck()) return -1;
    pleikkari::VrUiControl control;
    control.visible = static_cast<int>(in[0]);
    control.flags = static_cast<int>(in[1]);
    control.pointerX = in[2];
    control.pointerY = in[3];
    control.radius = in[4];
    control.anchorX = in[5];
    control.anchorY = in[6];
    float out[pleikkari::VrUiOutCount];
    const int result = cinema(h)->draw(matrix, video, menu, newFrame, &control, out);
    if(uiOut) env->SetFloatArrayRegion(uiOut, 0, pleikkari::VrUiOutCount, out);
    return result;
}
// PLE-722: a toolkit panel's Surface (Canvas draws into it; the compositor latches it), or null.
extern "C" JNIEXPORT jobject JNICALL JNI_METHOD(createPanel)(JNIEnv *env, jobject, jlong h, jint index, jint width, jint height,
        jfloat inset, jfloat corner, jint anchor) {
    Cinema *c = cinema(h);
    return c->uiReady ? c->ui.createPanel(env, index, width, height, inset, corner, anchor) : nullptr;
}
// PLE-698: {submit call, predicted display time} of the last draw, CLOCK_MONOTONIC ns.
// PLE-761: debug builds only, before createPanel; see vr-ui-layers.h's VrUiDebug.
extern "C" JNIEXPORT void JNICALL JNI_METHOD(debugSetUiLayers)(JNIEnv *, jobject, jlong h, jboolean quad, jboolean overlay,
        jfloat texelScale, jboolean filterExpensive, jint maxPanels, jboolean reticleLayer) {
    pleikkari::VrUiDebug debug;
    debug.quad = quad;
    debug.overlay = overlay;
    debug.texelScale = std::clamp(static_cast<float>(texelScale), 0.5f, 2.0f);
    debug.filterExpensive = filterExpensive;
    debug.maxPanels = std::clamp(static_cast<int>(maxPanels), 1, pleikkari::VrUiMaxPanels);
    debug.reticleLayer = reticleLayer;
    cinema(h)->ui.setDebug(debug);
}
extern "C" JNIEXPORT void JNICALL JNI_METHOD(submitTiming)(JNIEnv *env, jobject, jlong h, jlongArray out) {
    const jlong timing[] = {cinema(h)->submitNs, cinema(h)->predictedDisplayNs};
    env->SetLongArrayRegion(out, 0, 2, timing);
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
// PLE-652: call before setEnvironment; it applies on the next environment change.
extern "C" JNIEXPORT void JNICALL JNI_METHOD(setRoomGpuLevel)(JNIEnv *, jobject, jlong h, jint level) {
    cinema(h)->roomGpuLevel = std::clamp(static_cast<int>(level), 0, 4);
}
// PLE-666: debug preview only; call before setEnvironment (PLE-650 variants, 0 is shipped).
extern "C" JNIEXPORT void JNICALL JNI_METHOD(debugSetSkyVariant)(JNIEnv *, jobject, jlong h, jint variant) {
    cinema(h)->skyVariant = variant;
}
// PLE-808: debug A/B; call before setEnvironment.
extern "C" JNIEXPORT void JNICALL JNI_METHOD(debugSetDepthAttachOnce)(JNIEnv *, jobject, jlong h, jboolean enabled) {
    cinema(h)->depthAttachOnce = enabled;
}
// PLE-746: start the input-to-photon probe's luma pass; false when the GPU objects cannot be made.
extern "C" JNIEXPORT jboolean JNICALL JNI_METHOD(probeEnable)(JNIEnv *, jobject, jlong h) {
    auto *c = cinema(h);
    if(c->probe) return true;
    std::unique_ptr<LumaProbe> probe(new LumaProbe());
    if(!probe->init()) { probe->destroy(); LOGE("Latency probe: luma pass unavailable"); return false; }
    c->probe = std::move(probe);
    LOGI("Latency probe: GPU luma of every latched frame (16x9 taps, 1x1 pass, async PBO readback)");
    return true;
}
extern "C" JNIEXPORT void JNICALL JNI_METHOD(probeFrame)(JNIEnv *, jobject, jlong h, jlong bufferTimestampNs, jlong latchedNs) {
    cinema(h)->probeFrame(bufferTimestampNs, latchedNs);
}
// PLE-746: up to out.length / 5 records {buffer timestamp, latched, submitted, predicted photon, luma};
// returns how many were written. Submitted and predicted are 0 when the draw did not show the video.
extern "C" JNIEXPORT jint JNICALL JNI_METHOD(probeTake)(JNIEnv *env, jobject, jlong h, jlongArray out) {
    auto &probe = cinema(h)->probe;
    if(!probe) return 0;
    probe->poll();
    const jsize capacity = env->GetArrayLength(out) / 5;
    jint count = 0;
    while(count < capacity && !probe->ready.empty()) {
        const LumaRecord &r = probe->ready.front();
        const jlong row[] = {r.bufferTimestampNs, r.latchedNs, r.submittedNs, r.predictedNs, r.luma};
        env->SetLongArrayRegion(out, count * 5, 5, row);
        probe->ready.pop_front();
        ++count;
    }
    return count;
}
extern "C" JNIEXPORT void JNICALL JNI_METHOD(destroy)(JNIEnv *, jobject, jlong h) { delete cinema(h); }
