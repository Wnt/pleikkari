// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// PLE-722: the native half of the Go VR UI toolkit (docs/design/vr-ui.md §10.2, §10.6).
//
// Each panel is a VrApi cylinder layer whose texture is an Android Surface the Kotlin toolkit
// draws into with Canvas on its own thread (vrapi_CreateAndroidSurfaceSwapChain): the compositor
// latches it, so this GL thread uploads and draws nothing of the panel itself. Panels are
// underlays: submitted before the eye buffer's projection layer, which then blends over them
// with ONE / ONE_MINUS_SRC_ALPHA. Into the eye buffer this class draws, after the scene:
//   1. each open panel's footprint, which scales the scene by one minus the panel's alpha and
//      zeroes the eye buffer's alpha there, so the panel below shows through exactly;
//   2. the laser and reticle from the Go remote's pose at the frame's own predicted display
//      time, with the depth test off, so they sit over the panel in the same frame.
// With no panel open nothing here draws or submits: one layer and ONE / ZERO, as before.
#ifndef PLEIKKARI_VR_UI_LAYERS_H
#define PLEIKKARI_VR_UI_LAYERS_H

#include <jni.h>
#include <GLES3/gl3.h>
#include <VrApi.h>
#include <VrApi_Helpers.h>
#include <VrApi_Input.h>
#include <cstdint>
#include <vector>
#include "vr-ui-panel.h"

namespace pleikkari {

constexpr int VrUiMaxPanels = 2;

// StreamVrActivity's VrUiFrame, handed over once a frame.
struct VrUiControl {
    int visible = 0;          // bit i: panel i is open
    int flags = 0;            // VrUiFlag*
    float pointerX = 0, pointerY = 0; // debug pointer, degrees right and up of panel 0's middle
    float radius = PLEIKKARI_VR_UI_RADIUS_M;
    float anchorX = 0, anchorY = 0;   // a picture-anchored panel's middle, degrees right and up
};
enum : int {
    VrUiFlagInteractive = 1,  // the pointer is over something that reacts: the larger reticle
    VrUiFlagPlace = 2,        // place the gaze panels again (the Recentre item)
    VrUiFlagDebugPointer = 4, // debug builds: a synthetic ray from pointerX/pointerY
};
enum : int { VrUiAnchorGaze = 0, VrUiAnchorPicture = 1 };

// PLE-761: debug-build switches for PLE-735's layer measurement (`debug.pleikkari.vr_ui_layers`,
// StreamVrActivity). The defaults are PLE-722's shipped panels.
struct VrUiDebug {
    bool quad = false;            // a flat projection-layer quad instead of the cylinder
    bool overlay = false;         // panels over the eye buffer: no footprint, eye buffer ONE / ZERO
    float texelScale = 1.0f;      // texture texels per PLEIKKARI_VR_UI_TEXELS_PER_DEGREE (1.5 = 24/degree)
    bool filterExpensive = false; // VRAPI_FRAME_LAYER_FLAG_FILTER_EXPENSIVE on the panel layers
    int maxPanels = 2;            // panel layers submitted at most (1: the menu only)
    bool reticleLayer = false;    // PLE-776: laser and reticle in their own layer, submitted last
};

// The Go remote, read once a frame by the cinema's input().
struct VrUiRemote {
    bool present = false;
    ovrDeviceID device = 0;
    uint32_t buttons = 0;
    bool touching = false;
    float touchX = 0.5f, touchY = 0.5f; // 0..1 over the touchpad
};

// VrUiFrame's output slots.
enum : int {
    VrUiOutPanel = 0, VrUiOutU, VrUiOutV, VrUiOutButtons, VrUiOutTouching, VrUiOutTouchX, VrUiOutTouchY,
    VrUiOutRemote, VrUiOutCount
};
enum : int { VrUiButtonTrigger = 1, VrUiButtonTouchpad = 2 };

class VrUiLayers {
public:
    bool init();
    // Before createPanel.
    void setDebug(const VrUiDebug &value) { debug = value; }
    // On the GL thread with its context current, before VrApi shuts down.
    void destroy();
    // A panel of width x height texels. inset and corner (texels) describe the rounded
    // rectangle the toolkit fills, inside a transparent border, so the footprint matches it.
    // Returns the Surface the toolkit draws into, or null.
    jobject createPanel(JNIEnv *env, int index, int width, int height, float inset, float corner, int anchor);

    // Once a frame before the eyes. screenPlaced: the cinema placed its picture this frame.
    void beginFrame(const VrUiControl &control, const ovrTracking2 &head, double displayTime,
                    const VrUiRemote &remote, const ovrTracking *remotePose,
                    bool screenPlaced, PleikkariVrVec3 screenCentre, PleikkariVrQuat screenOrientation, bool fullPose);
    // Whether any panel layer goes out this frame (the projection layer then blends over it,
    // unless the overlay switch puts the panels on top).
    bool active() const { return layerCount > 0; }
    bool overlay() const { return debug.overlay; }
    void drawEye(const ovrMatrix4f &view, const ovrMatrix4f &projection);
    // The frame's panel layers, back to front, to submit before the projection layer (after it
    // with the overlay switch).
    int layers(const ovrTracking2 &head, ovrLayer_Union2 *out) const;
    // PLE-776: with the reticle-layer switch, draws this frame's laser and reticle into a
    // transparent width x height eye-sized swapchain and fills out, to submit after every other
    // layer. False (and nothing drawn) without the switch or without a pointer this frame.
    bool reticleLayer(const ovrTracking2 &head, int width, int height, ovrLayerProjection2 *out);
    void output(float out[VrUiOutCount]) const;

private:
    struct Panel {
        ovrTextureSwapChain *chain = nullptr;
        int width = 0, height = 0;
        float inset = 0, corner = 0;
        int anchor = VrUiAnchorGaze;
        PleikkariVrPanel shape{};
        bool open = false;
        float opacity = 0;
    };
    Panel panels[VrUiMaxPanels];
    VrUiDebug debug{};
    // PLE-776: the reticle layer's eye swapchains, made on first use.
    struct ReticleEye {
        ovrTextureSwapChain *chain = nullptr;
        std::vector<GLuint> fbos;
        int index = 0;
    };
    ReticleEye reticleEyes[VRAPI_FRAME_LAYER_EYE_MAX];
    int reticleWidth = 0, reticleHeight = 0;
    void drawPointer(const ovrMatrix4f &view, const ovrMatrix4f &projection);
    void destroyReticle();
    GLuint program = 0, vao = 0, stripVertices = 0, quadVertices = 0;
    GLint mvpLocation = -1, modeLocation = -1, aLocation = -1, bLocation = -1, cLocation = -1,
          colorLocation = -1, rectLocation = -1, opacityLocation = -1;
    double lastDisplayTime = 0;
    int layerCount = 0;
    // The frame's pointer.
    bool pointer = false;
    PleikkariVrVec3 rayStart{}, rayEnd{};
    bool hit = false;
    int hitPanel = -1;
    PleikkariVrPanelHit hitAt{};
    float reticleDegrees = 0;
    PleikkariVrVec3 eye{};
    VrUiRemote remote{};
};

} // namespace pleikkari

#endif
