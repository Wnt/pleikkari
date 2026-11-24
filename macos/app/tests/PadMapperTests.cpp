// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "PadMapper.h"
#include "TestHarness.h"

#include <cmath>

using namespace plk;
using namespace plk::app;

namespace {

dualsense::InputState restingReport(uint64_t sensor_us)
{
	dualsense::InputState in;
	in.sensor_time_us = sensor_us;
	in.motion_valid = true;
	in.accel_x = 0.0f;
	in.accel_y = 1.0f;
	in.accel_z = 0.0f;
	return in;
}

bool near(float a, float b, float eps = 1e-3f)
{
	return std::fabs(a - b) < eps;
}

} // namespace

PLK_TEST(pad_buttons_sticks_triggers)
{
	PadMapper m;
	dualsense::InputState in;
	in.buttons = dualsense::kButtonCross | dualsense::kButtonPS | dualsense::kButtonTouchpad;
	in.extra_buttons = dualsense::kExtraMute | dualsense::kExtraEdgePaddleLeft;
	in.l2_state = 17;
	in.r2_state = 255;
	in.left_x = -32768;
	in.left_y = 32767;
	in.right_x = 1234;
	in.right_y = -4321;
	m.apply(in);
	const ChiakiControllerState &s = m.state();
	CHECK(s.buttons == (CHIAKI_CONTROLLER_BUTTON_CROSS | CHIAKI_CONTROLLER_BUTTON_PS | CHIAKI_CONTROLLER_BUTTON_TOUCHPAD));
	CHECK(s.l2_state == 17 && s.r2_state == 255);
	CHECK(s.left_x == -32768 && s.left_y == 32767);
	CHECK(s.right_x == 1234 && s.right_y == -4321);
	// The mapper's button bits are the lib's.
	CHECK((uint32_t)dualsense::kButtonMoon == (uint32_t)CHIAKI_CONTROLLER_BUTTON_MOON);
	CHECK((uint32_t)dualsense::kButtonDpadDown == (uint32_t)CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN);
	CHECK((uint32_t)dualsense::kButtonShare == (uint32_t)CHIAKI_CONTROLLER_BUTTON_SHARE);

	m.reset();
	CHECK(m.state().buttons == 0 && m.state().l2_state == 0 && m.state().left_x == 0);
}

PLK_TEST(pad_touch_lifecycle)
{
	PadMapper m;
	dualsense::InputState in;
	in.touches[0] = { true, 5, 100, 200 };
	m.apply(in);
	const int8_t id0 = m.state().touches[0].id;
	CHECK(id0 >= 0);
	CHECK(m.state().touches[0].x == 100 && m.state().touches[0].y == 200);

	// Same contact moves: same lib id.
	in.touches[0] = { true, 5, 1919, 1079 };
	m.apply(in);
	CHECK(m.state().touches[0].id == id0);
	CHECK(m.state().touches[0].x == 1919 && m.state().touches[0].y == 1079);

	// A second finger.
	in.touches[1] = { true, 6, 50, 60 };
	m.apply(in);
	CHECK(m.state().touches[1].id >= 0 && m.state().touches[1].id != id0);

	// Out-of-range coordinates are clamped to the PS5's touchpad.
	in.touches[1] = { true, 6, 5000, 5000 };
	m.apply(in);
	CHECK(m.state().touches[1].x == 1919 && m.state().touches[1].y == 1079);

	// A new contact id in slot 0 without a lift in between: a new touch.
	in.touches[0] = { true, 9, 10, 10 };
	m.apply(in);
	bool found_new = false;
	for(const auto &t : m.state().touches)
		if(t.id >= 0 && t.id != id0 && t.x == 10 && t.y == 10)
			found_new = true;
	CHECK(found_new);

	// Both lift.
	in.touches[0].active = false;
	in.touches[1].active = false;
	m.apply(in);
	CHECK(m.state().touches[0].id < 0 && m.state().touches[1].id < 0);
}

PLK_TEST(pad_motion_uses_sensor_clock)
{
	PadMapper m;
	// Reports arriving in one Bluetooth burst still carry their own sensor times.
	m.apply(restingReport(1000000));
	CHECK(m.lastMotionStepUs() == 0);
	m.apply(restingReport(1004000));
	CHECK(m.lastMotionStepUs() == 4000);
	m.apply(restingReport(1007900));
	CHECK(m.lastMotionStepUs() == 3900);
	// The 32-bit wrap of the tracker's timestamp.
	PadMapper w;
	w.apply(restingReport(0xFFFFF000ull));
	w.apply(restingReport(0x100000F00ull));
	CHECK(w.lastMotionStepUs() == 0x1F00);

	// A rotation about one axis turns the orientation by about rate * time.
	PadMapper r;
	dualsense::InputState in = restingReport(0);
	r.apply(in);
	in.gyro_y = 1.0f; // rad/s
	for(int i = 1; i <= 250; i++) // 1 s at 4 ms per report
	{
		in.sensor_time_us = (uint64_t)i * 4000;
		r.apply(in);
	}
	const ChiakiControllerState &s = r.state();
	CHECK(near(s.gyro_y, 1.0f));
	const float norm = std::sqrt(s.orient_x * s.orient_x + s.orient_y * s.orient_y + s.orient_z * s.orient_z
			+ s.orient_w * s.orient_w);
	CHECK(near(norm, 1.0f, 1e-2f));
	CHECK_MSG(std::fabs(s.orient_w) < 0.99f, "orientation did not move: w=%f", s.orient_w);
}

PLK_TEST(pad_motion_reset_zeroes_accel)
{
	PadMapper m;
	dualsense::InputState in = restingReport(0);
	in.accel_x = 0.5f;
	in.accel_y = 0.8f;
	in.accel_z = 0.3f;
	m.apply(in);
	in.sensor_time_us = 4000;
	m.apply(in);
	CHECK(near(m.state().accel_x, 0.5f));
	m.resetMotion();
	// The held pose now reads as level: accel (0, 1, 0).
	CHECK(near(m.state().accel_x, 0.0f));
	CHECK(near(m.state().accel_y, 1.0f));
	CHECK(near(m.state().accel_z, 0.0f));
	in.sensor_time_us = 8000;
	m.apply(in);
	CHECK(near(m.state().accel_x, 0.0f) && near(m.state().accel_y, 1.0f) && near(m.state().accel_z, 0.0f));
}

PLK_TEST(pad_simple_report_has_no_motion)
{
	PadMapper m;
	dualsense::InputState in;
	in.mode = dualsense::ReportMode::BluetoothSimple;
	in.motion_valid = false;
	in.buttons = dualsense::kButtonBox;
	m.apply(in);
	CHECK(m.state().buttons == CHIAKI_CONTROLLER_BUTTON_BOX);
	CHECK(near(m.state().accel_y, 1.0f) && near(m.state().orient_w, 1.0f));
}
