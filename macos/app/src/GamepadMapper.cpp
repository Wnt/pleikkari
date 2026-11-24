// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "GamepadMapper.h"

#include <algorithm>
#include <cmath>

namespace plk::app {

namespace {

int16_t axis(float v)
{
	if(!(v == v)) // NaN
		return 0;
	v = std::clamp(v, -1.0f, 1.0f);
	return (int16_t)std::lround(v * 32767.0f);
}

uint8_t trigger(float v)
{
	if(!(v == v))
		return 0;
	return (uint8_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f);
}

} // namespace

ChiakiControllerState mapGamepad(const GamepadInput &in)
{
	ChiakiControllerState s;
	chiaki_controller_state_set_idle(&s);
	uint32_t b = 0;
	if(in.a) b |= CHIAKI_CONTROLLER_BUTTON_CROSS;
	if(in.b) b |= CHIAKI_CONTROLLER_BUTTON_MOON;
	if(in.x) b |= CHIAKI_CONTROLLER_BUTTON_BOX;
	if(in.y) b |= CHIAKI_CONTROLLER_BUTTON_PYRAMID;
	if(in.dpad_left) b |= CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT;
	if(in.dpad_right) b |= CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT;
	if(in.dpad_up) b |= CHIAKI_CONTROLLER_BUTTON_DPAD_UP;
	if(in.dpad_down) b |= CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN;
	if(in.left_shoulder) b |= CHIAKI_CONTROLLER_BUTTON_L1;
	if(in.right_shoulder) b |= CHIAKI_CONTROLLER_BUTTON_R1;
	if(in.left_stick_click) b |= CHIAKI_CONTROLLER_BUTTON_L3;
	if(in.right_stick_click) b |= CHIAKI_CONTROLLER_BUTTON_R3;
	if(in.menu) b |= CHIAKI_CONTROLLER_BUTTON_OPTIONS;
	if(in.options) b |= CHIAKI_CONTROLLER_BUTTON_SHARE;
	if(in.home) b |= CHIAKI_CONTROLLER_BUTTON_PS;
	if(in.touchpad_click) b |= CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;
	s.buttons = b;
	s.l2_state = trigger(in.left_trigger);
	s.r2_state = trigger(in.right_trigger);
	// The console's y axis points down.
	s.left_x = axis(in.left_x);
	s.left_y = axis(-in.left_y);
	s.right_x = axis(in.right_x);
	s.right_y = axis(-in.right_y);
	return s;
}

} // namespace plk::app
