// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

// PLE-722: the geometry of the Go VR UI's panels (docs/design/vr-ui.md §10.3, §10.6). A panel
// is a section of a vertical cylinder centred on the viewer's eye, drawn by the VrApi
// compositor as a cylinder layer (src/vr/cpp/vr-cinema.cpp). This file places panels, maps a
// controller ray onto one, and builds the layer's model matrix. Plain C with no VrApi, so
// chiaki-unit covers it on the host.
//
// Space: VrApi's tracking space, metres, y up, right-handed, -z straight ahead. A panel's own
// frame has its middle along -z; u runs left to right and v top to bottom, both 0..1 over the
// panel's texture.

#ifndef PLEIKKARI_VR_UI_PANEL_H
#define PLEIKKARI_VR_UI_PANEL_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Texel density of every panel texture, in both directions (§10.3): at or a little above the
// Go's display density, so the compositor minifies slightly and never magnifies.
#define PLEIKKARI_VR_UI_TEXELS_PER_DEGREE 16.0f
// Default panel distance, and its limits against the picture behind it (§10.3).
#define PLEIKKARI_VR_UI_RADIUS_M 2.0f
#define PLEIKKARI_VR_UI_RADIUS_MIN_M 0.6f
#define PLEIKKARI_VR_UI_RADIUS_SCREEN_FRACTION 0.8f
// A summoned panel opens this far below the gaze, its pitch clamped to +-PITCH_LIMIT.
#define PLEIKKARI_VR_UI_BELOW_GAZE_DEG 6.0f
#define PLEIKKARI_VR_UI_PITCH_LIMIT_DEG 25.0f

typedef struct pleikkari_vr_vec3_t { float x, y, z; } PleikkariVrVec3;
typedef struct pleikkari_vr_quat_t { float x, y, z, w; } PleikkariVrQuat;

typedef struct pleikkari_vr_panel_t
{
	PleikkariVrVec3 centre;      // on the cylinder's axis: the eye when the panel was placed
	PleikkariVrQuat orientation; // panel frame to tracking space
	float radius_m;
	float arc_rad;               // horizontal angle the texture covers
	float height_tan;            // texture height over radius (square texels on the surface)
} PleikkariVrPanel;

typedef struct pleikkari_vr_panel_hit_t
{
	float u, v;             // texture fractions; outside 0..1 when the ray misses the panel
	float distance_m;       // along the ray, from its origin
	PleikkariVrVec3 point;  // tracking space
} PleikkariVrPanelHit;

typedef enum pleikkari_vr_panel_hit_result_t
{
	PLEIKKARI_VR_PANEL_MISS = -1,   // the ray never reaches the cylinder in front of it
	PLEIKKARI_VR_PANEL_OUTSIDE = 0, // it reaches the cylinder beside the panel (u, v still set)
	PLEIKKARI_VR_PANEL_INSIDE = 1,
} PleikkariVrPanelHitResult;

// A panel of width x height texels at PLEIKKARI_VR_UI_TEXELS_PER_DEGREE, straight ahead of the
// origin, radius_m away.
void pleikkari_vr_panel_init(PleikkariVrPanel *panel, int width_texels, int height_texels, float radius_m);

// The panel radius for a picture screen_distance_m away: RADIUS_M, but always in front of the
// picture and never nearer than RADIUS_MIN_M.
float pleikkari_vr_panel_radius(float screen_distance_m);

// Head pitch in degrees (positive up) and yaw in radians (positive to the left) from an
// orientation, as the cinema places its screen.
float pleikkari_vr_head_pitch_deg(PleikkariVrQuat head);
float pleikkari_vr_head_yaw_rad(PleikkariVrQuat head);

// Summons the panel: centred BELOW_GAZE_DEG under the gaze at the head's yaw, pitch clamped to
// +-PITCH_LIMIT_DEG, facing the eye. full_pose (PLE-675's debug screencaps of a Go on a table)
// places it straight along the whole head orientation instead.
void pleikkari_vr_panel_place_at_gaze(PleikkariVrPanel *panel, PleikkariVrVec3 head_position,
		PleikkariVrQuat head_orientation, bool full_pose);

// Places the panel yaw_deg right and pitch_deg up of an anchor frame (the picture's).
void pleikkari_vr_panel_place_relative(PleikkariVrPanel *panel, PleikkariVrVec3 anchor_centre,
		PleikkariVrQuat anchor_orientation, float yaw_deg, float pitch_deg);

// Where a ray meets the panel's cylinder, on the far side from the axis (the viewer is inside).
PleikkariVrPanelHitResult pleikkari_vr_panel_hit(const PleikkariVrPanel *panel, PleikkariVrVec3 origin,
		PleikkariVrVec3 direction, PleikkariVrPanelHit *hit);

// A ray from the panel's centre x_deg right and y_deg up of its middle: the debug pointer.
PleikkariVrVec3 pleikkari_vr_panel_direction(const PleikkariVrPanel *panel, float x_deg, float y_deg);

// Row-major 4x4 (ovrMatrix4f's layout): translation(centre) * rotation(orientation), then with
// layer_scale the cylinder layer's scale (radius, radius * height_tan / 2, radius), which
// VrApi's cylinder mapping needs for square texels (the SDK's VrCompositor sample).
void pleikkari_vr_panel_matrix(const PleikkariVrPanel *panel, bool layer_scale, float out[16]);

PleikkariVrVec3 pleikkari_vr_quat_rotate(PleikkariVrQuat q, PleikkariVrVec3 v);
PleikkariVrQuat pleikkari_vr_quat_multiply(PleikkariVrQuat a, PleikkariVrQuat b);

#ifdef __cplusplus
}
#endif

#endif
