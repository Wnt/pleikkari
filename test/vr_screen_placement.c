// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <munit.h>

#include "../android/app/src/main/cpp/vr-screen-placement.h"

// PLE-702: a worn Go looks within a few degrees of level; one lying lenses-down on the
// table reads about -87 degrees of pitch.
#define WORN_PITCH -4.0f
#define TABLE_PITCH -87.0f

static void frames(PleikkariVrScreenPlacement *placement, int count, int recenters, float pitch, bool full_pose)
{
	for(int i = 0; i < count; ++i)
		munit_assert_int(pleikkari_vr_screen_placement_frame(placement, recenters, pitch, full_pose), ==, PLEIKKARI_VR_SCREEN_KEEP);
}

static MunitResult test_first_frame_and_request(const MunitParameter params[], void *user)
{
	(void)params;
	(void)user;
	PleikkariVrScreenPlacement placement;
	pleikkari_vr_screen_placement_init(&placement);
	munit_assert_int(pleikkari_vr_screen_placement_frame(&placement, 113, WORN_PITCH, false), ==, PLEIKKARI_VR_SCREEN_PLACE_REQUESTED);
	munit_assert_false(placement.provisional);
	frames(&placement, 1000, 113, WORN_PITCH, false);

	// The wearer's recentre (touchpad click) places it once, then it stays.
	pleikkari_vr_screen_placement_request(&placement);
	munit_assert_int(pleikkari_vr_screen_placement_frame(&placement, 113, 20.0f, false), ==, PLEIKKARI_VR_SCREEN_PLACE_REQUESTED);
	frames(&placement, 1000, 113, 20.0f, false);
	return MUNIT_OK;
}

static MunitResult test_runtime_recentre(const MunitParameter params[], void *user)
{
	(void)params;
	(void)user;
	PleikkariVrScreenPlacement placement;
	pleikkari_vr_screen_placement_init(&placement);
	munit_assert_int(pleikkari_vr_screen_placement_frame(&placement, 113, WORN_PITCH, false), ==, PLEIKKARI_VR_SCREEN_PLACE_REQUESTED);
	// VrApi handles "HMT was mounted" in the first submit and re-bases LOCAL space: the
	// count rises between frames 1 and 2 of every session.
	munit_assert_int(pleikkari_vr_screen_placement_frame(&placement, 114, WORN_PITCH, false), ==, PLEIKKARI_VR_SCREEN_PLACE_RUNTIME);
	frames(&placement, 500, 114, WORN_PITCH, false);
	// A long press of the Oculus button, or the Go put back on.
	munit_assert_int(pleikkari_vr_screen_placement_frame(&placement, 115, WORN_PITCH, false), ==, PLEIKKARI_VR_SCREEN_PLACE_RUNTIME);
	frames(&placement, 500, 115, WORN_PITCH, false);

	// A request and a runtime recentre in the same frame are one placement.
	pleikkari_vr_screen_placement_request(&placement);
	munit_assert_int(pleikkari_vr_screen_placement_frame(&placement, 116, WORN_PITCH, false), ==, PLEIKKARI_VR_SCREEN_PLACE_REQUESTED);
	frames(&placement, 10, 116, WORN_PITCH, false);
	return MUNIT_OK;
}

static MunitResult test_table_then_worn(const MunitParameter params[], void *user)
{
	(void)params;
	(void)user;
	// The 07:16 report: a stream started on the table, then someone looked into the Go.
	PleikkariVrScreenPlacement placement;
	pleikkari_vr_screen_placement_init(&placement);
	munit_assert_int(pleikkari_vr_screen_placement_frame(&placement, 113, TABLE_PITCH, false), ==, PLEIKKARI_VR_SCREEN_PLACE_REQUESTED);
	munit_assert_true(placement.provisional);
	// The runtime recentre on mount still happens on the table; still provisional.
	munit_assert_int(pleikkari_vr_screen_placement_frame(&placement, 114, TABLE_PITCH, false), ==, PLEIKKARI_VR_SCREEN_PLACE_RUNTIME);
	munit_assert_true(placement.provisional);
	// Minutes on the table: the screen stays put.
	frames(&placement, 72 * 60, 114, TABLE_PITCH, false);

	// Picked up, and swung through level on the way: not long enough to count.
	frames(&placement, PLEIKKARI_VR_SCREEN_LEVEL_FRAMES - 1, 114, -10.0f, false);
	frames(&placement, 5, 114, -45.0f, false);
	// Worn: placed again after 0.5 s of a level head, then kept.
	frames(&placement, PLEIKKARI_VR_SCREEN_LEVEL_FRAMES - 1, 114, WORN_PITCH, false);
	munit_assert_int(pleikkari_vr_screen_placement_frame(&placement, 114, WORN_PITCH, false), ==, PLEIKKARI_VR_SCREEN_PLACE_LEVEL);
	munit_assert_false(placement.provisional);
	frames(&placement, 1000, 114, WORN_PITCH, false);
	// Looking at the floor later does not move a placement that was made level.
	frames(&placement, 1000, 114, -80.0f, false);
	frames(&placement, 1000, 114, WORN_PITCH, false);
	return MUNIT_OK;
}

static MunitResult test_steep_request(const MunitParameter params[], void *user)
{
	(void)params;
	(void)user;
	PleikkariVrScreenPlacement placement;
	pleikkari_vr_screen_placement_init(&placement);
	munit_assert_int(pleikkari_vr_screen_placement_frame(&placement, 113, WORN_PITCH, false), ==, PLEIKKARI_VR_SCREEN_PLACE_REQUESTED);
	// A recentre pressed while looking up at 70 degrees is provisional too...
	pleikkari_vr_screen_placement_request(&placement);
	munit_assert_int(pleikkari_vr_screen_placement_frame(&placement, 113, 70.0f, false), ==, PLEIKKARI_VR_SCREEN_PLACE_REQUESTED);
	munit_assert_true(placement.provisional);
	// ...but 60 degrees is not steep, and between 30 and 60 nothing counts.
	pleikkari_vr_screen_placement_request(&placement);
	munit_assert_int(pleikkari_vr_screen_placement_frame(&placement, 113, 60.0f, false), ==, PLEIKKARI_VR_SCREEN_PLACE_REQUESTED);
	munit_assert_false(placement.provisional);
	pleikkari_vr_screen_placement_request(&placement);
	munit_assert_int(pleikkari_vr_screen_placement_frame(&placement, 113, -61.0f, false), ==, PLEIKKARI_VR_SCREEN_PLACE_REQUESTED);
	frames(&placement, 1000, 113, 30.0f, false);
	frames(&placement, PLEIKKARI_VR_SCREEN_LEVEL_FRAMES - 1, 113, -29.0f, false);
	munit_assert_int(pleikkari_vr_screen_placement_frame(&placement, 113, -29.0f, false), ==, PLEIKKARI_VR_SCREEN_PLACE_LEVEL);
	return MUNIT_OK;
}

static MunitResult test_full_pose_never_provisional(const MunitParameter params[], void *user)
{
	(void)params;
	(void)user;
	// PLE-675's debug placement puts the screen along the table pose, in view: it must stay
	// there for a screencap, not jump when nobody moves the Go.
	PleikkariVrScreenPlacement placement;
	pleikkari_vr_screen_placement_init(&placement);
	munit_assert_int(pleikkari_vr_screen_placement_frame(&placement, 113, TABLE_PITCH, true), ==, PLEIKKARI_VR_SCREEN_PLACE_REQUESTED);
	munit_assert_false(placement.provisional);
	munit_assert_int(pleikkari_vr_screen_placement_frame(&placement, 114, TABLE_PITCH, true), ==, PLEIKKARI_VR_SCREEN_PLACE_RUNTIME);
	frames(&placement, 1000, 114, WORN_PITCH, true);
	munit_assert_string_equal(pleikkari_vr_screen_place_name(PLEIKKARI_VR_SCREEN_PLACE_LEVEL), "head came level");
	return MUNIT_OK;
}

MunitTest tests_vr_screen_placement[] = {
	{ "/first_frame_and_request", test_first_frame_and_request, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/runtime_recentre", test_runtime_recentre, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/table_then_worn", test_table_then_worn, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/steep_request", test_steep_request, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/full_pose_never_provisional", test_full_pose_never_provisional, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
