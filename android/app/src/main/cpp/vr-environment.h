// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// PLE-603: ambient environments around the curved stream screen on the Oculus Go.
//
// Plain OpenGL ES 3.0, no VrApi and no Android dependency, so the same source compiles
// into the APK's native library (arm64, gate) and into a host tool that renders through
// Mesa for off-headset proof (android/vr-environment/host). The caller owns the EGL
// context, the eye framebuffers and the decoder texture; this module only issues draw
// calls into whatever framebuffer is bound.
//
// Coordinate space: metres, y up, right-handed, the tracking-space origin at the
// viewer's eyes with the screen centred straight ahead along -z (the same convention as
// VrApi's tracking space, so PLE-602 passes its ovrTracking2 eye matrices unchanged,
// transposed to column-major).
//
// Video path (hard constraint 4 of the ticket): the video texture is sampled in place,
// exactly as the plain screen samples it. The only extra read is the screen-glow
// downsample, which renders the same texture into an 8x8 target once per new frame. No
// frame is copied, queued or read back to the CPU.
//
// Integration (PLE-602's Cinema::draw):
//   pleikkari_vr_environment_create(&config)            once, on the GL thread
//   per frame, before the eye loop:
//     pleikkari_vr_environment_begin_frame(env, video, target, transform, new_frame)
//   per eye, with the eye framebuffer bound and the viewport set:
//     pleikkari_vr_environment_draw_eye(env, view, projection, video, target, transform, has_video)
//   per frame, after both eyes and the caller's own passes, before glFlush (optional):
//     pleikkari_vr_environment_end_frame(env)
//   pleikkari_vr_environment_destroy(env)               on the same GL thread
// draw_eye clears the framebuffer itself (colour and its own depth, scissored to the
// viewport), so the caller's glClear before it becomes redundant. When the environment is PLAIN the output is the
// caller's own picture: black plus the curved screen.
#ifndef PLEIKKARI_VR_ENVIRONMENT_H
#define PLEIKKARI_VR_ENVIRONMENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	// Black surroundings, curved screen only. Today's picture (PLE-602); the default.
	PLEIKKARI_VR_ENVIRONMENT_PLAIN = 0,
	// A dark, slightly blue dome with a soft floor glow under the screen. No geometry
	// besides the dome; the cheapest room that still gives the eye a horizon.
	PLEIKKARI_VR_ENVIRONMENT_VOID = 1,
	// A dark cinema hall: raked seat rows, side walls with sconces, stage front under
	// the screen, ceiling with a faint centre light. Fully original procedural geometry.
	PLEIKKARI_VR_ENVIRONMENT_CINEMA = 2,
	// A night terrace: dark floor plane, horizon gradient, a field of stars.
	PLEIKKARI_VR_ENVIRONMENT_TERRACE = 3,
	PLEIKKARI_VR_ENVIRONMENT_COUNT = 4
} PleikkariVrEnvironmentKind;

typedef struct {
	PleikkariVrEnvironmentKind environment;
	// Eye to screen centre, along -z. PLE-602's plain screen: 3.0 m.
	float screen_distance_m;
	// Width of the picture measured along its surface (arc length when curved).
	// PLE-602's plain screen: 80 degrees at 3 m = 4.19 m.
	float screen_width_m;
	// Radius of the vertical cylinder the picture lies on; 0 = flat. Skybox and PLE-602
	// both centre the curvature on the viewer (radius == distance).
	float screen_curve_radius_m;
	// Vertical offset of the screen centre from eye height. 0 = centred on the gaze.
	float screen_height_offset_m;
	// 0..1: how strongly the video lights the room (0 = static room, no downsample pass).
	float glow;
	// 0..1: level of the room's own baked lights (sconces, aisle strips, dome tint).
	float room_light;
	// Picture aspect (width / height); the PS5 stream is 16:9.
	float screen_aspect;
} PleikkariVrEnvironmentConfig;

typedef struct {
	// GPU time from begin_frame to the second draw_eye, or to end_frame once the caller uses
	// it (PLE-651), when GL_EXT_disjoint_timer_query exists; 0 otherwise. Read one frame late so it never stalls the pipeline.
	uint64_t gpu_ns;
	uint32_t draw_calls;
	uint32_t triangles;
	uint32_t vertex_bytes;
} PleikkariVrEnvironmentStats;

typedef struct PleikkariVrEnvironment PleikkariVrEnvironment;

// Fills a config with the defaults that reproduce PLE-602's plain screen.
void pleikkari_vr_environment_config_default(PleikkariVrEnvironmentConfig *config);

// Clamps a config into the ranges the geometry supports (see the .cpp for the numbers).
void pleikkari_vr_environment_config_clamp(PleikkariVrEnvironmentConfig *config);

// Creates GL objects on the current context. Returns NULL and logs when a shader fails.
// video_target is GL_TEXTURE_EXTERNAL_OES on Android (the decoder's SurfaceTexture) or
// GL_TEXTURE_2D (host tool, tests); it selects the sampler type compiled into the shaders.
PleikkariVrEnvironment *pleikkari_vr_environment_create(const PleikkariVrEnvironmentConfig *config,
		uint32_t video_target);

// Applies a new configuration; rebuilds the screen and room meshes when their inputs changed.
void pleikkari_vr_environment_set_config(PleikkariVrEnvironment *env,
		const PleikkariVrEnvironmentConfig *config);
void pleikkari_vr_environment_get_config(const PleikkariVrEnvironment *env,
		PleikkariVrEnvironmentConfig *config);

// Once per frame before the eyes: refreshes the 8x8 glow map from the video texture when
// new_frame is set and the environment uses glow. Leaves the caller's draw framebuffer
// binding restored. transform is the 4x4 SurfaceTexture matrix (column-major) or identity.
void pleikkari_vr_environment_begin_frame(PleikkariVrEnvironment *env, uint32_t video_texture,
		const float video_transform[16], int has_video, int new_frame);

// Draws one eye into the currently bound framebuffer at the current viewport: clears,
// draws the room, then the screen (video, or a dark idle panel when has_video is 0).
// view and projection are column-major 4x4 matrices in the space described above.
void pleikkari_vr_environment_draw_eye(PleikkariVrEnvironment *env, const float view[16],
		const float projection[16], uint32_t video_texture, const float video_transform[16],
		int has_video);

// Optional, once per frame after everything the caller draws into the eyes (PLE-651): ends
// the GPU timer there, so gpu_ns covers the whole frame (the caller's overlays, border
// clears and the eye framebuffers' MSAA resolves) instead of stopping after the second
// draw_eye. Once called, draw_eye no longer ends the timer itself.
void pleikkari_vr_environment_end_frame(PleikkariVrEnvironment *env);

// Counters for the last completed frame.
void pleikkari_vr_environment_stats(PleikkariVrEnvironment *env, PleikkariVrEnvironmentStats *stats);

void pleikkari_vr_environment_destroy(PleikkariVrEnvironment *env);

// PLE-650: debug-only sky draw variants for the void and terrace, for interleaved GPU
// A/B in the preview. PLEIKKARI_VR_SKY_DOME is the shipped draw and the default; nothing
// but the debug preview calls this.
enum {
	PLEIKKARI_VR_SKY_DOME = 0,                // 32x12 sphere, culling off (shipped)
	PLEIKKARI_VR_SKY_DOME_CULLED = 1,         // same mesh, back faces culled
	PLEIKKARI_VR_SKY_DOME_COARSE = 2,         // 8x4 sphere, culling off
	PLEIKKARI_VR_SKY_FULLSCREEN = 3,          // one triangle, direction from inverse view-projection
	PLEIKKARI_VR_SKY_NONE = 4,                // dome not drawn
	PLEIKKARI_VR_SKY_DOME_CONSTANT = 5,       // shipped mesh, constant-colour shader
	PLEIKKARI_VR_SKY_FULLSCREEN_CONSTANT = 6, // one triangle, constant colour
	PLEIKKARI_VR_SKY_DOME_VERTEX_GRADIENT = 7, // PLE-665: shipped mesh, gradient per vertex
	PLEIKKARI_VR_SKY_COUNT = 8
};
void pleikkari_vr_environment_debug_set_sky_variant(PleikkariVrEnvironment *env, int variant);

// Name of an environment kind, for logs and tests.
const char *pleikkari_vr_environment_name(PleikkariVrEnvironmentKind kind);

#ifdef __cplusplus
}
#endif

#endif
