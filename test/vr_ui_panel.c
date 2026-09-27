// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <munit.h>
#include <math.h>

#include "../android/app/src/main/cpp/vr-ui-panel.h"

#define DEG 0.017453292519943295f
#define EPS 1e-4

// PLE-722: the in-stream menu is 800x448 texels, 50 x 28 degrees at 16 texels per degree.
#define MENU_W 800
#define MENU_H 448

static PleikkariVrVec3 vec3(float x, float y, float z)
{
	PleikkariVrVec3 v = { x, y, z };
	return v;
}

static PleikkariVrQuat yaw_pitch(float yaw_deg, float pitch_deg)
{
	// Head orientation: yaw about +y (left positive), then pitch about +x (up positive).
	PleikkariVrQuat y = { 0.0f, sinf(yaw_deg * DEG / 2), 0.0f, cosf(yaw_deg * DEG / 2) };
	PleikkariVrQuat x = { sinf(pitch_deg * DEG / 2), 0.0f, 0.0f, cosf(pitch_deg * DEG / 2) };
	return pleikkari_vr_quat_multiply(y, x);
}

static MunitResult test_init(const MunitParameter params[], void *user)
{
	(void)params; (void)user;
	PleikkariVrPanel panel;
	pleikkari_vr_panel_init(&panel, MENU_W, MENU_H, 2.0f);
	munit_assert_double_equal(panel.arc_rad, 50.0 * DEG, 5);
	munit_assert_double_equal(panel.height_tan, 28.0 * DEG, 5);
	munit_assert_double_equal(panel.radius_m, 2.0, 5);
	munit_assert_double_equal(panel.orientation.w, 1.0, 5);
	return MUNIT_OK;
}

static MunitResult test_radius(const MunitParameter params[], void *user)
{
	(void)params; (void)user;
	munit_assert_double_equal(pleikkari_vr_panel_radius(3.0f), 2.0, 5);  // the plain screen
	munit_assert_double_equal(pleikkari_vr_panel_radius(12.0f), 2.0, 5);
	munit_assert_double_equal(pleikkari_vr_panel_radius(1.5f), 1.2, 5);  // stays in front of the picture
	munit_assert_double_equal(pleikkari_vr_panel_radius(0.5f), 0.6, 5);  // never nearer than 0.6 m
	return MUNIT_OK;
}

static MunitResult test_centre_ray(const MunitParameter params[], void *user)
{
	(void)params; (void)user;
	PleikkariVrPanel panel;
	pleikkari_vr_panel_init(&panel, MENU_W, MENU_H, 2.0f);
	PleikkariVrPanelHit hit;
	munit_assert_int(pleikkari_vr_panel_hit(&panel, vec3(0, 0, 0), vec3(0, 0, -1), &hit), ==, PLEIKKARI_VR_PANEL_INSIDE);
	munit_assert_double_equal(hit.u, 0.5, 4);
	munit_assert_double_equal(hit.v, 0.5, 4);
	munit_assert_double_equal(hit.distance_m, 2.0, 4);
	munit_assert_double_equal(hit.point.z, -2.0, 4);
	return MUNIT_OK;
}

static MunitResult test_direction_maps_to_texels(const MunitParameter params[], void *user)
{
	(void)params; (void)user;
	PleikkariVrPanel panel;
	pleikkari_vr_panel_init(&panel, MENU_W, MENU_H, 2.0f);
	PleikkariVrPanelHit hit;
	// 10 degrees right is 160 texels right of the middle; up is towards v = 0.
	munit_assert_int(pleikkari_vr_panel_hit(&panel, panel.centre, pleikkari_vr_panel_direction(&panel, 10.0f, 5.0f), &hit),
			==, PLEIKKARI_VR_PANEL_INSIDE);
	munit_assert_double_equal(hit.u * MENU_W, 400.0 + 160.0, 2);
	munit_assert_double_equal(hit.v, 0.5 - tan(5.0 * DEG) / (28.0 * DEG), 4);
	munit_assert_double_equal(hit.distance_m, 2.0 / cos(5.0 * DEG), 4);
	// Left edge, just inside and just beyond.
	munit_assert_int(pleikkari_vr_panel_hit(&panel, panel.centre, pleikkari_vr_panel_direction(&panel, -24.9f, 0.0f), &hit),
			==, PLEIKKARI_VR_PANEL_INSIDE);
	munit_assert_int(pleikkari_vr_panel_hit(&panel, panel.centre, pleikkari_vr_panel_direction(&panel, -25.5f, 0.0f), &hit),
			==, PLEIKKARI_VR_PANEL_OUTSIDE);
	munit_assert_double(hit.u, <, 0.0);
	return MUNIT_OK;
}

static MunitResult test_offset_origin(const MunitParameter params[], void *user)
{
	(void)params; (void)user;
	// The Go remote sits at the hip, off the cylinder's axis: a ray from there aimed at a point
	// on the panel lands on that point.
	PleikkariVrPanel panel;
	pleikkari_vr_panel_init(&panel, MENU_W, MENU_H, 2.0f);
	PleikkariVrPanelHit target;
	pleikkari_vr_panel_hit(&panel, panel.centre, pleikkari_vr_panel_direction(&panel, -12.0f, 8.0f), &target);
	const PleikkariVrVec3 hand = vec3(0.2f, -0.45f, -0.25f);
	PleikkariVrVec3 aim = vec3(target.point.x - hand.x, target.point.y - hand.y, target.point.z - hand.z);
	PleikkariVrPanelHit hit;
	munit_assert_int(pleikkari_vr_panel_hit(&panel, hand, aim, &hit), ==, PLEIKKARI_VR_PANEL_INSIDE);
	munit_assert_double_equal(hit.u, target.u, 4);
	munit_assert_double_equal(hit.v, target.v, 4);
	// The ray length is scaled into distance_m.
	const float length = sqrtf(aim.x * aim.x + aim.y * aim.y + aim.z * aim.z);
	munit_assert_double_equal(hit.distance_m, length, 4);
	return MUNIT_OK;
}

static MunitResult test_misses(const MunitParameter params[], void *user)
{
	(void)params; (void)user;
	PleikkariVrPanel panel;
	pleikkari_vr_panel_init(&panel, MENU_W, MENU_H, 2.0f);
	PleikkariVrPanelHit hit;
	munit_assert_int(pleikkari_vr_panel_hit(&panel, vec3(0, 0, 0), vec3(0, 1, 0), &hit), ==, PLEIKKARI_VR_PANEL_MISS);
	// Pointing backwards meets the cylinder behind the viewer, far outside the panel's arc.
	munit_assert_int(pleikkari_vr_panel_hit(&panel, vec3(0, 0, 0), vec3(0, 0, 1), &hit), ==, PLEIKKARI_VR_PANEL_OUTSIDE);
	// Far above the panel.
	munit_assert_int(pleikkari_vr_panel_hit(&panel, vec3(0, 0, 0), pleikkari_vr_panel_direction(&panel, 0.0f, 40.0f), &hit),
			==, PLEIKKARI_VR_PANEL_OUTSIDE);
	munit_assert_double(hit.v, <, 0.0);
	return MUNIT_OK;
}

static MunitResult test_place_at_gaze(const MunitParameter params[], void *user)
{
	(void)params; (void)user;
	PleikkariVrPanel panel;
	pleikkari_vr_panel_init(&panel, MENU_W, MENU_H, 2.0f);
	const PleikkariVrVec3 head = vec3(0.1f, 1.6f, -0.05f);
	// Head turned 90 degrees left, level: the panel's middle is along -x, 6 degrees down.
	pleikkari_vr_panel_place_at_gaze(&panel, head, yaw_pitch(90.0f, 0.0f), false);
	PleikkariVrVec3 middle = pleikkari_vr_panel_direction(&panel, 0.0f, 0.0f);
	munit_assert_double_equal(middle.x, -cos(6.0 * DEG), 4);
	munit_assert_double_equal(middle.y, -sin(6.0 * DEG), 4);
	munit_assert_double_equal(middle.z, 0.0, 4);
	PleikkariVrPanelHit hit;
	munit_assert_int(pleikkari_vr_panel_hit(&panel, head, middle, &hit), ==, PLEIKKARI_VR_PANEL_INSIDE);
	munit_assert_double_equal(hit.u, 0.5, 4);
	munit_assert_double_equal(hit.v, 0.5, 4);
	// Looking steeply up clamps the panel to 25 degrees; the Go on a table (-87) to -25.
	pleikkari_vr_panel_place_at_gaze(&panel, head, yaw_pitch(0.0f, 60.0f), false);
	middle = pleikkari_vr_panel_direction(&panel, 0.0f, 0.0f);
	munit_assert_double_equal(middle.y, sin(25.0 * DEG), 4);
	pleikkari_vr_panel_place_at_gaze(&panel, head, yaw_pitch(0.0f, -87.0f), false);
	middle = pleikkari_vr_panel_direction(&panel, 0.0f, 0.0f);
	munit_assert_double_equal(middle.y, -sin(25.0 * DEG), 4);
	// Full pose follows the table pose, so a screencap sees the panel.
	pleikkari_vr_panel_place_at_gaze(&panel, head, yaw_pitch(30.0f, -87.0f), true);
	middle = pleikkari_vr_panel_direction(&panel, 0.0f, 0.0f);
	munit_assert_double_equal(middle.y, -sin(87.0 * DEG), 4);
	munit_assert_double_equal(pleikkari_vr_head_pitch_deg(yaw_pitch(30.0f, -87.0f)), -87.0, 3);
	munit_assert_double_equal(pleikkari_vr_head_yaw_rad(yaw_pitch(30.0f, -40.0f)), 30.0 * DEG, 4);
	return MUNIT_OK;
}

static MunitResult test_place_relative(const MunitParameter params[], void *user)
{
	(void)params; (void)user;
	PleikkariVrPanel panel;
	pleikkari_vr_panel_init(&panel, 320, 128, 2.0f);
	const PleikkariVrQuat screen = yaw_pitch(0.0f, 0.0f);
	// 30 degrees to the right of the picture's middle and 16 up: its upper-right area.
	pleikkari_vr_panel_place_relative(&panel, vec3(0, 0, 0), screen, 30.0f, 16.0f);
	const PleikkariVrVec3 middle = pleikkari_vr_panel_direction(&panel, 0.0f, 0.0f);
	munit_assert_double_equal(middle.x, sin(30.0 * DEG) * cos(16.0 * DEG), 4);
	munit_assert_double_equal(middle.y, sin(16.0 * DEG), 4);
	munit_assert_double(middle.z, <, 0.0);
	// PLE-761: 1.5x the texels at 1.5x the density is the same panel.
	PleikkariVrPanel dense;
	pleikkari_vr_panel_init_density(&dense, 480, 192, 2.0f, 1.5f * PLEIKKARI_VR_UI_TEXELS_PER_DEGREE);
	munit_assert_double_equal(dense.arc_rad, panel.arc_rad, 5);
	munit_assert_double_equal(dense.height_tan, panel.height_tan, 5);
	return MUNIT_OK;
}

static MunitResult test_matrix(const MunitParameter params[], void *user)
{
	(void)params; (void)user;
	PleikkariVrPanel panel;
	pleikkari_vr_panel_init(&panel, MENU_W, MENU_H, 2.0f);
	panel.centre = vec3(1.0f, 2.0f, 3.0f);
	float m[16];
	pleikkari_vr_panel_matrix(&panel, true, m);
	munit_assert_double_equal(m[0], 2.0, 5);
	munit_assert_double_equal(m[5], 2.0 * 28.0 * DEG / 2.0, 5);
	munit_assert_double_equal(m[10], 2.0, 5);
	munit_assert_double_equal(m[3], 1.0, 5);
	munit_assert_double_equal(m[7], 2.0, 5);
	munit_assert_double_equal(m[11], 3.0, 5);
	munit_assert_double_equal(m[15], 1.0, 5);
	// Rotation part maps -z to the panel's middle.
	pleikkari_vr_panel_place_at_gaze(&panel, vec3(0, 0, 0), yaw_pitch(90.0f, 0.0f), false);
	pleikkari_vr_panel_matrix(&panel, false, m);
	const PleikkariVrVec3 middle = pleikkari_vr_panel_direction(&panel, 0.0f, 0.0f);
	munit_assert_double_equal(-m[2], middle.x, 4);
	munit_assert_double_equal(-m[6], middle.y, 4);
	munit_assert_double_equal(-m[10], middle.z, 4);
	return MUNIT_OK;
}

MunitTest tests_vr_ui_panel[] = {
	{ "/init", test_init, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/radius", test_radius, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/centre_ray", test_centre_ray, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/direction_maps_to_texels", test_direction_maps_to_texels, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/offset_origin", test_offset_origin, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/misses", test_misses, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/place_at_gaze", test_place_at_gaze, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/place_relative", test_place_relative, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/matrix", test_matrix, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
