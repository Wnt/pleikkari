// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// A controller that is not a DualSense (Xbox, DualShock 4, Switch Pro, any MFi pad), as
// macOS's GameController framework describes it, to the state the console is sent. The
// layout is by position, as the Qt app maps its SDL controllers:
//
//   A (bottom) Cross    B (right) Circle    X (left) Square    Y (top) Triangle
//   LB L1   RB R1   LT L2   RT R2   stick clicks L3 R3   d-pad d-pad
//   Menu (the right one of the two small buttons) Options
//   View (the left one) Create/Share        Xbox/Home button PS
//   Share (Series controllers) or a touchpad click: touchpad click
//
// Such a controller has no motion sensor and no touch surface: motion stays at rest.

#ifndef PLK_APP_GAMEPADMAPPER_H
#define PLK_APP_GAMEPADMAPPER_H

#include <chiaki/controller.h>

namespace plk::app {

/** What the framework reports. Sticks: -1..1, x right, y UP (the framework's sign). Triggers 0..1. */
struct GamepadInput
{
	bool a = false, b = false, x = false, y = false;
	bool left_shoulder = false, right_shoulder = false;
	bool left_stick_click = false, right_stick_click = false;
	bool dpad_up = false, dpad_down = false, dpad_left = false, dpad_right = false;
	bool menu = false;    // Xbox Menu, DualShock Options, Switch +
	bool options = false; // Xbox View, DualShock Share, Switch -
	bool home = false;
	bool touchpad_click = false; // Xbox Share, DualShock touchpad button
	float left_trigger = 0, right_trigger = 0;
	float left_x = 0, left_y = 0, right_x = 0, right_y = 0;
};

ChiakiControllerState mapGamepad(const GamepadInput &in);

} // namespace plk::app

#endif // PLK_APP_GAMEPADMAPPER_H
