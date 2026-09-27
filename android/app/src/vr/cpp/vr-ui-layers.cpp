// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// PLE-722: see vr-ui-layers.h.
#include "vr-ui-layers.h"
#include "vr-ui-shaders.h"

#include <android/log.h>
#include <android/native_window_jni.h>
#include <VrApi_Input.h>
#include <algorithm>
#include <cmath>

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "GoVrUi", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "GoVrUi", __VA_ARGS__)

namespace pleikkari {
namespace {

constexpr int StripSegments = 48;
constexpr double FadeSeconds = 0.15;      // §10.4: open and close
constexpr double ReticleTweenSeconds = 0.1;
constexpr float ReticleIdleDegrees = 0.6f, ReticleInteractiveDegrees = 0.8f;
constexpr float LaserWidthM = 0.004f, LaserAlpha = 0.6f;
constexpr float LaserFraction = 0.25f, LaserNoHitM = 0.4f, LaserStartM = 0.04f;
// The debug pointer has no remote; its laser starts where a right hand would hold one.
constexpr PleikkariVrVec3 DebugHand = {0.15f, -0.35f, -0.25f};

GLuint compile(GLenum type, const char *source) {
    GLuint id = glCreateShader(type);
    glShaderSource(id, 1, &source, nullptr);
    glCompileShader(id);
    GLint ok = 0;
    glGetShaderiv(id, GL_COMPILE_STATUS, &ok);
    if(!ok) {
        char log[1024]; glGetShaderInfoLog(id, sizeof(log), nullptr, log);
        LOGE("Shader compile: %s", log); glDeleteShader(id); return 0;
    }
    return id;
}

ovrMatrix4f toOvr(const float m[16]) {
    ovrMatrix4f out;
    for(int row = 0; row < 4; ++row)
        for(int col = 0; col < 4; ++col) out.M[row][col] = m[row * 4 + col];
    return out;
}

void uniformMatrix(GLint location, const ovrMatrix4f &matrix) {
    float columns[16];
    for(int row = 0; row < 4; ++row)
        for(int col = 0; col < 4; ++col) columns[col * 4 + row] = matrix.M[row][col];
    glUniformMatrix4fv(location, 1, GL_FALSE, columns);
}

PleikkariVrVec3 add(PleikkariVrVec3 a, PleikkariVrVec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
PleikkariVrVec3 sub(PleikkariVrVec3 a, PleikkariVrVec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
PleikkariVrVec3 scale(PleikkariVrVec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float length(PleikkariVrVec3 a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
PleikkariVrVec3 normalize(PleikkariVrVec3 a) { const float l = length(a); return l > 1e-6f ? scale(a, 1.0f / l) : a; }
PleikkariVrVec3 cross(PleikkariVrVec3 a, PleikkariVrVec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
PleikkariVrVec3 vec(const ovrVector3f &v) { return {v.x, v.y, v.z}; }
PleikkariVrQuat quat(const ovrQuatf &q) { return {q.x, q.y, q.z, q.w}; }

} // namespace

bool VrUiLayers::init() {
    GLuint v = compile(GL_VERTEX_SHADER, VrUiVertexShader), f = compile(GL_FRAGMENT_SHADER, VrUiFragmentShader);
    if(!v || !f) { if(v) glDeleteShader(v); if(f) glDeleteShader(f); return false; }
    program = glCreateProgram();
    glAttachShader(program, v); glAttachShader(program, f); glLinkProgram(program);
    glDeleteShader(v); glDeleteShader(f);
    GLint ok = 0; glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if(!ok) {
        char log[1024]; glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        LOGE("UI program link failed: %s", log);
        return false;
    }
    mvpLocation = glGetUniformLocation(program, "mvp");
    modeLocation = glGetUniformLocation(program, "mode");
    aLocation = glGetUniformLocation(program, "a");
    bLocation = glGetUniformLocation(program, "b");
    cLocation = glGetUniformLocation(program, "c");
    colorLocation = glGetUniformLocation(program, "color");
    rectLocation = glGetUniformLocation(program, "rect");
    opacityLocation = glGetUniformLocation(program, "opacity");
    float strip[(StripSegments + 1) * 4];
    for(int i = 0; i <= StripSegments; ++i) {
        const float u = static_cast<float>(i) / StripSegments;
        strip[i * 4 + 0] = u; strip[i * 4 + 1] = 0.0f;
        strip[i * 4 + 2] = u; strip[i * 4 + 3] = 1.0f;
    }
    const float quad[] = {0, 0, 1, 0, 0, 1, 1, 1};
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &stripVertices);
    glBindBuffer(GL_ARRAY_BUFFER, stripVertices);
    glBufferData(GL_ARRAY_BUFFER, sizeof(strip), strip, GL_STATIC_DRAW);
    glGenBuffers(1, &quadVertices);
    glBindBuffer(GL_ARRAY_BUFFER, quadVertices);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return glGetError() == GL_NO_ERROR;
}

void VrUiLayers::destroy() {
    for(auto &panel : panels) {
        if(panel.chain) vrapi_DestroyTextureSwapChain(panel.chain);
        panel.chain = nullptr;
    }
    if(program) glDeleteProgram(program);
    if(stripVertices) glDeleteBuffers(1, &stripVertices);
    if(quadVertices) glDeleteBuffers(1, &quadVertices);
    if(vao) glDeleteVertexArrays(1, &vao);
    program = vao = stripVertices = quadVertices = 0;
}

jobject VrUiLayers::createPanel(JNIEnv *env, int index, int width, int height, float inset, float corner, int anchor) {
    if(index < 0 || index >= VrUiMaxPanels || width <= 0 || height <= 0) return nullptr;
    Panel &panel = panels[index];
    if(panel.chain) vrapi_DestroyTextureSwapChain(panel.chain);
    panel = Panel{};
    // PLE-761: the texel-density switch scales the texture, keeping its angular size; the toolkit
    // scales its Canvas to match.
    width = static_cast<int>(std::lround(width * debug.texelScale));
    height = static_cast<int>(std::lround(height * debug.texelScale));
    inset *= debug.texelScale; corner *= debug.texelScale;
    const float texelsPerDegree = PLEIKKARI_VR_UI_TEXELS_PER_DEGREE * debug.texelScale;
    panel.chain = vrapi_CreateAndroidSurfaceSwapChain(width, height);
    if(!panel.chain) { LOGE("Panel %d: no Android surface swapchain", index); return nullptr; }
    panel.width = width; panel.height = height; panel.inset = inset; panel.corner = corner; panel.anchor = anchor;
    pleikkari_vr_panel_init_density(&panel.shape, width, height, PLEIKKARI_VR_UI_RADIUS_M, texelsPerDegree);
    jobject surface = vrapi_GetTextureSwapChainAndroidSurface(panel.chain);
    if(ANativeWindow *window = surface ? ANativeWindow_fromSurface(env, surface) : nullptr) {
        // Canvas draws premultiplied RGBA; the panel needs its alpha for the rounded corners.
        ANativeWindow_setBuffersGeometry(window, width, height, WINDOW_FORMAT_RGBA_8888);
        ANativeWindow_release(window);
    }
    LOGI("Panel %d: %dx%d texels, %.1f x %.1f degrees, %s-anchored %s %s layer%s, at most %d panels", index,
        width, height, width / texelsPerDegree, height / texelsPerDegree,
        anchor == VrUiAnchorGaze ? "gaze" : "picture", debug.overlay ? "overlay" : "underlay",
        debug.quad ? "quad" : "cylinder", debug.filterExpensive ? ", expensive filter" : "", debug.maxPanels);
    return surface ? env->NewLocalRef(surface) : nullptr;
}

void VrUiLayers::beginFrame(const VrUiControl &control, const ovrTracking2 &head, double displayTime,
                            const VrUiRemote &input, const ovrTracking *remotePose,
                            bool screenPlaced, PleikkariVrVec3 screenCentre, PleikkariVrQuat screenOrientation, bool fullPose) {
    const double dt = lastDisplayTime > 0 ? std::clamp(displayTime - lastDisplayTime, 0.0, 0.1) : 0.0;
    lastDisplayTime = displayTime;
    remote = input;
    eye = vec(head.HeadPose.Pose.Position);
    const PleikkariVrQuat headOrientation = quat(head.HeadPose.Pose.Orientation);
    layerCount = 0;
    for(int i = 0; i < VrUiMaxPanels; ++i) {
        Panel &panel = panels[i];
        if(!panel.chain) continue;
        const bool wanted = (control.visible & (1 << i)) != 0 && i < debug.maxPanels;
        panel.shape.radius_m = control.radius;
        if(panel.anchor == VrUiAnchorPicture) {
            pleikkari_vr_panel_place_relative(&panel.shape, screenCentre, screenOrientation, control.anchorX, control.anchorY);
        } else if(wanted && ((!panel.open && panel.opacity <= 0.0f) || (control.flags & VrUiFlagPlace) || screenPlaced)) {
            // Summoned at the gaze, then world-locked; again on every recentre (§10.3).
            pleikkari_vr_panel_place_at_gaze(&panel.shape, eye, headOrientation, fullPose);
            LOGI("Panel %d placed: head yaw %.0f, pitch %.0f degrees, radius %.2f m%s", i,
                pleikkari_vr_head_yaw_rad(headOrientation) * 57.2958f, pleikkari_vr_head_pitch_deg(headOrientation),
                panel.shape.radius_m, fullPose ? ", full pose" : "");
        }
        panel.open = wanted;
        const float step = FadeSeconds > 0 ? static_cast<float>(dt / FadeSeconds) : 1.0f;
        panel.opacity = std::clamp(panel.opacity + (wanted ? step : -step), 0.0f, 1.0f);
        if(panel.opacity > 0.0f) ++layerCount;
    }
    // The pointer: only while a gaze panel is open.
    pointer = false;
    hit = false;
    hitPanel = -1;
    const Panel &menu = panels[0];
    if(!menu.chain || !menu.open) return;
    PleikkariVrVec3 origin{}, direction{}, laserFrom{};
    if(control.flags & VrUiFlagDebugPointer) {
        origin = menu.shape.centre;
        direction = pleikkari_vr_panel_direction(&menu.shape, control.pointerX, control.pointerY);
        laserFrom = add(menu.shape.centre, pleikkari_vr_quat_rotate(menu.shape.orientation, DebugHand));
    } else if(remote.present && remotePose) {
        origin = vec(remotePose->HeadPose.Pose.Position);
        direction = pleikkari_vr_quat_rotate(quat(remotePose->HeadPose.Pose.Orientation), {0.0f, 0.0f, -1.0f});
        laserFrom = add(origin, scale(direction, LaserStartM));
    } else {
        return;
    }
    pointer = true;
    float nearest = 1e9f;
    for(int i = 0; i < VrUiMaxPanels; ++i) {
        const Panel &panel = panels[i];
        if(!panel.chain || !panel.open || panel.anchor != VrUiAnchorGaze) continue;
        PleikkariVrPanelHit at{};
        const PleikkariVrPanelHitResult result = pleikkari_vr_panel_hit(&panel.shape, origin, direction, &at);
        if(result == PLEIKKARI_VR_PANEL_MISS || at.distance_m >= nearest) continue;
        nearest = at.distance_m;
        hitPanel = i;
        hitAt = at;
        hit = result == PLEIKKARI_VR_PANEL_INSIDE;
    }
    rayStart = laserFrom;
    rayEnd = hit ? add(laserFrom, scale(sub(hitAt.point, laserFrom), LaserFraction))
                 : add(laserFrom, scale(normalize(direction), LaserNoHitM));
    const float target = control.flags & VrUiFlagInteractive ? ReticleInteractiveDegrees : ReticleIdleDegrees;
    if(reticleDegrees <= 0.0f) reticleDegrees = target;
    reticleDegrees += (target - reticleDegrees) * static_cast<float>(std::min(1.0, dt / ReticleTweenSeconds));
}

void VrUiLayers::drawEye(const ovrMatrix4f &view, const ovrMatrix4f &projection) {
    if(layerCount == 0 && !pointer) return;
    glUseProgram(program);
    glBindVertexArray(vao);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glEnableVertexAttribArray(0);
    // 1. Footprints: colour times one minus the panel's alpha; eye-buffer alpha 0.
    glBlendFuncSeparate(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ZERO);
    // PLE-761: a flat footprint under a quad; none under an overlay, which covers the eye buffer.
    glUniform1i(modeLocation, debug.quad ? 3 : 0);
    glBindBuffer(GL_ARRAY_BUFFER, stripVertices);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    for(const Panel &panel : panels) {
        if(debug.overlay || !panel.chain || panel.opacity <= 0.0f) continue;
        float model[16];
        pleikkari_vr_panel_matrix(&panel.shape, false, model);
        const ovrMatrix4f modelMatrix = toOvr(model);
        const ovrMatrix4f modelView = ovrMatrix4f_Multiply(&view, &modelMatrix);
        uniformMatrix(mvpLocation, ovrMatrix4f_Multiply(&projection, &modelView));
        glUniform3f(aLocation, panel.shape.arc_rad, panel.shape.height_tan, panel.shape.radius_m);
        glUniform4f(rectLocation, static_cast<float>(panel.width), static_cast<float>(panel.height), panel.inset, panel.corner);
        glUniform1f(opacityLocation, panel.opacity);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, (StripSegments + 1) * 2);
    }
    // 2. Laser and reticle over everything, straight alpha.
    if(pointer) {
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        uniformMatrix(mvpLocation, ovrMatrix4f_Multiply(&projection, &view));
        glBindBuffer(GL_ARRAY_BUFFER, quadVertices);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        const PleikkariVrVec3 across = scale(normalize(cross(sub(rayEnd, rayStart), sub(eye, rayStart))), LaserWidthM);
        glUniform1i(modeLocation, 1);
        glUniform3f(aLocation, rayStart.x, rayStart.y, rayStart.z);
        glUniform3f(bLocation, rayEnd.x, rayEnd.y, rayEnd.z);
        glUniform3f(cLocation, across.x, across.y, across.z);
        glUniform4f(colorLocation, 1.0f, 1.0f, 1.0f, LaserAlpha);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        if(hit) {
            const PleikkariVrVec3 toEye = sub(eye, hitAt.point);
            const float half = length(toEye) * std::tan(reticleDegrees * 0.5f * 0.0174533f);
            const PleikkariVrVec3 facing = normalize(toEye);
            const PleikkariVrVec3 right = scale(normalize(cross({0.0f, 1.0f, 0.0f}, facing)), half);
            const PleikkariVrVec3 up = scale(normalize(cross(facing, right)), half);
            glUniform1i(modeLocation, 2);
            glUniform3f(aLocation, hitAt.point.x, hitAt.point.y, hitAt.point.z);
            glUniform3f(bLocation, right.x, right.y, right.z);
            glUniform3f(cLocation, up.x, up.y, up.z);
            glUniform4f(colorLocation, 0.93f, 0.92f, 0.94f, 1.0f);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisable(GL_BLEND);
    glBindVertexArray(0);
}

int VrUiLayers::layers(const ovrTracking2 &head, ovrLayer_Union2 *out) const {
    int count = 0;
    // Back to front: the picture-anchored readout under the summoned menu.
    for(int i = VrUiMaxPanels - 1; i >= 0; --i) {
        const Panel &panel = panels[i];
        if(!panel.chain || panel.opacity <= 0.0f) continue;
        const uint32_t headerFlags = VRAPI_FRAME_LAYER_FLAG_CHROMATIC_ABERRATION_CORRECTION |
                                     VRAPI_FRAME_LAYER_FLAG_INHIBIT_SRGB_FRAMEBUFFER |
                                     (debug.filterExpensive ? VRAPI_FRAME_LAYER_FLAG_FILTER_EXPENSIVE : 0);
        // Canvas pixels are premultiplied; a fade scales all four channels (§10.2).
        const ovrVector4f colorScale = {panel.opacity, panel.opacity, panel.opacity, panel.opacity};
        if(debug.quad) {
            // PLE-761: a flat quad at the cylinder's radius, as wide at its middle as the arc.
            ovrLayerProjection2 quad = vrapi_DefaultLayerProjection2();
            quad.Header.Flags |= headerFlags;
            quad.Header.ColorScale = colorScale;
            quad.Header.SrcBlend = VRAPI_FRAME_LAYER_BLEND_ONE;
            quad.Header.DstBlend = VRAPI_FRAME_LAYER_BLEND_ONE_MINUS_SRC_ALPHA;
            quad.HeadPose = head.HeadPose;
            float model[16];
            pleikkari_vr_panel_matrix(&panel.shape, false, model);
            const float r = panel.shape.radius_m;
            const ovrMatrix4f place = toOvr(model);
            const ovrMatrix4f push = ovrMatrix4f_CreateTranslation(0.0f, 0.0f, -r);
            const ovrMatrix4f size = ovrMatrix4f_CreateScale(r * std::tan(panel.shape.arc_rad * 0.5f),
                                                             r * panel.shape.height_tan * 0.5f, 1.0f);
            const ovrMatrix4f pushed = ovrMatrix4f_Multiply(&place, &push);
            const ovrMatrix4f modelMatrix = ovrMatrix4f_Multiply(&pushed, &size);
            for(int eye = 0; eye < VRAPI_FRAME_LAYER_EYE_MAX; ++eye) {
                const ovrMatrix4f modelView = ovrMatrix4f_Multiply(&head.Eye[eye].ViewMatrix, &modelMatrix);
                quad.Textures[eye].TexCoordsFromTanAngles = ovrMatrix4f_TanAngleMatrixFromUnitSquare(&modelView);
                quad.Textures[eye].ColorSwapChain = panel.chain;
                quad.Textures[eye].SwapChainIndex = 0;
            }
            out[count++].Projection = quad;
            continue;
        }
        ovrLayerCylinder2 layer = vrapi_DefaultLayerCylinder2();
        layer.Header.Flags |= headerFlags;
        layer.Header.ColorScale = colorScale;
        layer.Header.SrcBlend = VRAPI_FRAME_LAYER_BLEND_ONE;
        layer.Header.DstBlend = VRAPI_FRAME_LAYER_BLEND_ONE_MINUS_SRC_ALPHA;
        layer.HeadPose = head.HeadPose;
        float model[16];
        pleikkari_vr_panel_matrix(&panel.shape, true, model);
        const ovrMatrix4f modelMatrix = toOvr(model);
        // VrApi's cylinder maps 180 degrees around to 0..1; the texture covers arc_rad of it.
        const float circScale = static_cast<float>(M_PI) / panel.shape.arc_rad;
        for(int eye = 0; eye < VRAPI_FRAME_LAYER_EYE_MAX; ++eye) {
            const ovrMatrix4f modelView = ovrMatrix4f_Multiply(&head.Eye[eye].ViewMatrix, &modelMatrix);
            layer.Textures[eye].TexCoordsFromTanAngles = ovrMatrix4f_Inverse(&modelView);
            layer.Textures[eye].ColorSwapChain = panel.chain;
            layer.Textures[eye].SwapChainIndex = 0;
            layer.Textures[eye].TextureMatrix.M[0][0] = circScale;
            layer.Textures[eye].TextureMatrix.M[0][2] = 0.5f - 0.5f * circScale;
            layer.Textures[eye].TextureMatrix.M[1][1] = 0.5f;
            layer.Textures[eye].TextureMatrix.M[1][2] = 0.25f;
        }
        out[count++].Cylinder = layer;
    }
    return count;
}

void VrUiLayers::output(float out[VrUiOutCount]) const {
    out[VrUiOutPanel] = hitPanel >= 0 ? static_cast<float>(hitPanel) : -1.0f;
    out[VrUiOutU] = hitPanel >= 0 ? hitAt.u : -1.0f;
    out[VrUiOutV] = hitPanel >= 0 ? hitAt.v : -1.0f;
    out[VrUiOutButtons] = static_cast<float>((remote.buttons & ovrButton_A ? VrUiButtonTrigger : 0) |
                                             (remote.buttons & ovrButton_Enter ? VrUiButtonTouchpad : 0));
    out[VrUiOutTouching] = remote.touching ? 1.0f : 0.0f;
    out[VrUiOutTouchX] = remote.touchX;
    out[VrUiOutTouchY] = remote.touchY;
    out[VrUiOutRemote] = remote.present ? 1.0f : 0.0f;
}

} // namespace pleikkari
