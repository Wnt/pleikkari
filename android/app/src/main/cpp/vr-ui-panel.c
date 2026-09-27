// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "vr-ui-panel.h"

#include <math.h>

#define DEG_TO_RAD 0.017453292519943295f

static float clampf(float value, float low, float high)
{
	return value < low ? low : value > high ? high : value;
}

static PleikkariVrQuat axis_angle(float x, float y, float z, float angle_rad)
{
	const float s = sinf(angle_rad * 0.5f);
	PleikkariVrQuat q = { x * s, y * s, z * s, cosf(angle_rad * 0.5f) };
	return q;
}

static PleikkariVrQuat conjugate(PleikkariVrQuat q)
{
	PleikkariVrQuat c = { -q.x, -q.y, -q.z, q.w };
	return c;
}

static PleikkariVrVec3 sub(PleikkariVrVec3 a, PleikkariVrVec3 b)
{
	PleikkariVrVec3 r = { a.x - b.x, a.y - b.y, a.z - b.z };
	return r;
}

PleikkariVrQuat pleikkari_vr_quat_multiply(PleikkariVrQuat a, PleikkariVrQuat b)
{
	PleikkariVrQuat r = {
		a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
		a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
		a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
		a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
	};
	return r;
}

PleikkariVrVec3 pleikkari_vr_quat_rotate(PleikkariVrQuat q, PleikkariVrVec3 v)
{
	// v + 2w (q x v) + 2 q x (q x v)
	const float tx = 2.0f * (q.y * v.z - q.z * v.y);
	const float ty = 2.0f * (q.z * v.x - q.x * v.z);
	const float tz = 2.0f * (q.x * v.y - q.y * v.x);
	PleikkariVrVec3 r = {
		v.x + q.w * tx + (q.y * tz - q.z * ty),
		v.y + q.w * ty + (q.z * tx - q.x * tz),
		v.z + q.w * tz + (q.x * ty - q.y * tx)
	};
	return r;
}

void pleikkari_vr_panel_init(PleikkariVrPanel *panel, int width_texels, int height_texels, float radius_m)
{
	pleikkari_vr_panel_init_density(panel, width_texels, height_texels, radius_m, PLEIKKARI_VR_UI_TEXELS_PER_DEGREE);
}

void pleikkari_vr_panel_init_density(PleikkariVrPanel *panel, int width_texels, int height_texels, float radius_m,
		float texels_per_degree)
{
	panel->centre.x = panel->centre.y = panel->centre.z = 0.0f;
	panel->orientation.x = panel->orientation.y = panel->orientation.z = 0.0f;
	panel->orientation.w = 1.0f;
	panel->radius_m = radius_m;
	panel->arc_rad = (float)width_texels / texels_per_degree * DEG_TO_RAD;
	// One texel is as tall on the surface as it is wide: radius * one texel's angle.
	panel->height_tan = (float)height_texels / texels_per_degree * DEG_TO_RAD;
}

float pleikkari_vr_panel_radius(float screen_distance_m)
{
	float radius = PLEIKKARI_VR_UI_RADIUS_M;
	if(screen_distance_m > 0.0f && radius > screen_distance_m * PLEIKKARI_VR_UI_RADIUS_SCREEN_FRACTION)
		radius = screen_distance_m * PLEIKKARI_VR_UI_RADIUS_SCREEN_FRACTION;
	return radius < PLEIKKARI_VR_UI_RADIUS_MIN_M ? PLEIKKARI_VR_UI_RADIUS_MIN_M : radius;
}

float pleikkari_vr_head_pitch_deg(PleikkariVrQuat q)
{
	return asinf(clampf(2.0f * (q.w * q.x - q.y * q.z), -1.0f, 1.0f)) / DEG_TO_RAD;
}

float pleikkari_vr_head_yaw_rad(PleikkariVrQuat q)
{
	return atan2f(2.0f * (q.w * q.y + q.x * q.z), 1.0f - 2.0f * (q.y * q.y + q.x * q.x));
}

void pleikkari_vr_panel_place_at_gaze(PleikkariVrPanel *panel, PleikkariVrVec3 head_position,
		PleikkariVrQuat head_orientation, bool full_pose)
{
	panel->centre = head_position;
	if(full_pose)
	{
		panel->orientation = head_orientation;
		return;
	}
	const float pitch = clampf(pleikkari_vr_head_pitch_deg(head_orientation) - PLEIKKARI_VR_UI_BELOW_GAZE_DEG,
			-PLEIKKARI_VR_UI_PITCH_LIMIT_DEG, PLEIKKARI_VR_UI_PITCH_LIMIT_DEG);
	panel->orientation = pleikkari_vr_quat_multiply(
			axis_angle(0.0f, 1.0f, 0.0f, pleikkari_vr_head_yaw_rad(head_orientation)),
			axis_angle(1.0f, 0.0f, 0.0f, pitch * DEG_TO_RAD));
}

void pleikkari_vr_panel_place_relative(PleikkariVrPanel *panel, PleikkariVrVec3 anchor_centre,
		PleikkariVrQuat anchor_orientation, float yaw_deg, float pitch_deg)
{
	panel->centre = anchor_centre;
	// Positive yaw about +y turns -z to the left; this function's yaw is to the right.
	const PleikkariVrQuat turn = pleikkari_vr_quat_multiply(
			axis_angle(0.0f, 1.0f, 0.0f, -yaw_deg * DEG_TO_RAD),
			axis_angle(1.0f, 0.0f, 0.0f, pitch_deg * DEG_TO_RAD));
	panel->orientation = pleikkari_vr_quat_multiply(anchor_orientation, turn);
}

PleikkariVrPanelHitResult pleikkari_vr_panel_hit(const PleikkariVrPanel *panel, PleikkariVrVec3 origin,
		PleikkariVrVec3 direction, PleikkariVrPanelHit *hit)
{
	const PleikkariVrQuat inverse = conjugate(panel->orientation);
	const PleikkariVrVec3 o = pleikkari_vr_quat_rotate(inverse, sub(origin, panel->centre));
	const PleikkariVrVec3 d = pleikkari_vr_quat_rotate(inverse, direction);
	const float a = d.x * d.x + d.z * d.z;
	if(a < 1e-8f)
		return PLEIKKARI_VR_PANEL_MISS; // straight up or down, parallel to the axis
	const float b = 2.0f * (o.x * d.x + o.z * d.z);
	const float c = o.x * o.x + o.z * o.z - panel->radius_m * panel->radius_m;
	const float disc = b * b - 4.0f * a * c;
	if(disc < 0.0f)
		return PLEIKKARI_VR_PANEL_MISS;
	const float t = (-b + sqrtf(disc)) / (2.0f * a);
	if(t <= 0.0f)
		return PLEIKKARI_VR_PANEL_MISS;
	const float px = o.x + t * d.x, py = o.y + t * d.y, pz = o.z + t * d.z;
	const float length = sqrtf(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
	hit->u = atan2f(px, -pz) / panel->arc_rad + 0.5f;
	hit->v = 0.5f - py / panel->radius_m / panel->height_tan;
	hit->distance_m = t * length;
	hit->point.x = origin.x + t * direction.x;
	hit->point.y = origin.y + t * direction.y;
	hit->point.z = origin.z + t * direction.z;
	return hit->u >= 0.0f && hit->u <= 1.0f && hit->v >= 0.0f && hit->v <= 1.0f
			? PLEIKKARI_VR_PANEL_INSIDE : PLEIKKARI_VR_PANEL_OUTSIDE;
}

PleikkariVrVec3 pleikkari_vr_panel_direction(const PleikkariVrPanel *panel, float x_deg, float y_deg)
{
	const float theta = x_deg * DEG_TO_RAD, phi = y_deg * DEG_TO_RAD;
	PleikkariVrVec3 local = { sinf(theta) * cosf(phi), sinf(phi), -cosf(theta) * cosf(phi) };
	return pleikkari_vr_quat_rotate(panel->orientation, local);
}

void pleikkari_vr_panel_matrix(const PleikkariVrPanel *panel, bool layer_scale, float out[16])
{
	const PleikkariVrQuat q = panel->orientation;
	const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
	const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
	const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
	const float rotation[3][3] = {
		{ 1.0f - 2.0f * (yy + zz), 2.0f * (xy - wz), 2.0f * (xz + wy) },
		{ 2.0f * (xy + wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz - wx) },
		{ 2.0f * (xz - wy), 2.0f * (yz + wx), 1.0f - 2.0f * (xx + yy) }
	};
	const float scale[3] = {
		layer_scale ? panel->radius_m : 1.0f,
		layer_scale ? panel->radius_m * panel->height_tan * 0.5f : 1.0f,
		layer_scale ? panel->radius_m : 1.0f
	};
	const float translation[3] = { panel->centre.x, panel->centre.y, panel->centre.z };
	for(int row = 0; row < 3; ++row)
	{
		for(int col = 0; col < 3; ++col)
			out[row * 4 + col] = rotation[row][col] * scale[col];
		out[row * 4 + 3] = translation[row];
	}
	out[12] = out[13] = out[14] = 0.0f;
	out[15] = 1.0f;
}
