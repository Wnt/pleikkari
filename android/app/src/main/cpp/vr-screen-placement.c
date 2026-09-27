// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "vr-screen-placement.h"

#include <math.h>

void pleikkari_vr_screen_placement_init(PleikkariVrScreenPlacement *placement)
{
	placement->runtime_recenters = -1;
	placement->requested = true;
	placement->provisional = false;
	placement->level_frames = 0;
}

void pleikkari_vr_screen_placement_request(PleikkariVrScreenPlacement *placement)
{
	placement->requested = true;
}

PleikkariVrScreenPlace pleikkari_vr_screen_placement_frame(PleikkariVrScreenPlacement *placement,
		int runtime_recenters, float head_pitch_deg, bool full_pose)
{
	PleikkariVrScreenPlace place = PLEIKKARI_VR_SCREEN_KEEP;
	if(placement->requested)
		place = PLEIKKARI_VR_SCREEN_PLACE_REQUESTED;
	else if(placement->runtime_recenters >= 0 && runtime_recenters != placement->runtime_recenters)
		place = PLEIKKARI_VR_SCREEN_PLACE_RUNTIME;
	placement->runtime_recenters = runtime_recenters;

	const float tilt = fabsf(head_pitch_deg);
	if(place == PLEIKKARI_VR_SCREEN_KEEP && placement->provisional && !full_pose)
	{
		if(tilt >= PLEIKKARI_VR_SCREEN_LEVEL_PITCH_DEG)
			placement->level_frames = 0;
		else if(++placement->level_frames >= PLEIKKARI_VR_SCREEN_LEVEL_FRAMES)
			place = PLEIKKARI_VR_SCREEN_PLACE_LEVEL;
	}
	if(place == PLEIKKARI_VR_SCREEN_KEEP)
		return place;

	placement->requested = false;
	placement->provisional = !full_pose && tilt > PLEIKKARI_VR_SCREEN_STEEP_PITCH_DEG;
	placement->level_frames = 0;
	return place;
}

const char *pleikkari_vr_screen_place_name(PleikkariVrScreenPlace place)
{
	switch(place)
	{
		case PLEIKKARI_VR_SCREEN_PLACE_REQUESTED: return "requested";
		case PLEIKKARI_VR_SCREEN_PLACE_RUNTIME: return "runtime recentre";
		case PLEIKKARI_VR_SCREEN_PLACE_LEVEL: return "head came level";
		default: return "kept";
	}
}
