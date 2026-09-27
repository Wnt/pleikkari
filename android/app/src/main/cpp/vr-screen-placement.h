// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

// PLE-702: when the Go cinema (src/vr/cpp/vr-cinema.cpp) places its screen at the head's
// gaze. Plain C with no VrApi, so chiaki-unit covers it on the host.

#ifndef PLEIKKARI_VR_SCREEN_PLACEMENT_H
#define PLEIKKARI_VR_SCREEN_PLACEMENT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// The screen goes level at the head's yaw. A placement made with the head pitched further
// than this is provisional: the screen is out of view, and a Go lying on a table (about -87
// degrees) has a yaw that says nothing about where its wearer will face.
#define PLEIKKARI_VR_SCREEN_STEEP_PITCH_DEG 60.0f
// A provisional placement is made again once the head has stayed within this of level for
// this many frames in a row (0.5 s at 72 Hz, 0.6 s at 60 Hz).
#define PLEIKKARI_VR_SCREEN_LEVEL_PITCH_DEG 30.0f
#define PLEIKKARI_VR_SCREEN_LEVEL_FRAMES 36

typedef enum pleikkari_vr_screen_place_t
{
	PLEIKKARI_VR_SCREEN_KEEP = 0,        // leave the screen where it is
	PLEIKKARI_VR_SCREEN_PLACE_REQUESTED, // the first frame, or the wearer's recentre
	PLEIKKARI_VR_SCREEN_PLACE_RUNTIME,   // VrApi recentred LOCAL space by itself
	PLEIKKARI_VR_SCREEN_PLACE_LEVEL,     // the head came level after a provisional placement
} PleikkariVrScreenPlace;

typedef struct pleikkari_vr_screen_placement_t
{
	int runtime_recenters; // the last VRAPI_SYS_STATUS_RECENTER_COUNT, -1 before the first frame
	bool requested;
	bool provisional;
	int level_frames;
} PleikkariVrScreenPlacement;

// Starts with a placement requested, for the first frame.
void pleikkari_vr_screen_placement_init(PleikkariVrScreenPlacement *placement);
void pleikkari_vr_screen_placement_request(PleikkariVrScreenPlacement *placement);

// Once per frame, before drawing: whether to place the screen at the head's gaze now.
// full_pose: PLE-675's debug placement along the whole head orientation, which puts the
// screen in view at any pitch, so it is never provisional.
PleikkariVrScreenPlace pleikkari_vr_screen_placement_frame(PleikkariVrScreenPlacement *placement,
		int runtime_recenters, float head_pitch_deg, bool full_pose);

const char *pleikkari_vr_screen_place_name(PleikkariVrScreenPlace place);

#ifdef __cplusplus
}
#endif

#endif
