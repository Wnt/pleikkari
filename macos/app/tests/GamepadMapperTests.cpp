// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "GamepadMapper.h"
#include "TestHarness.h"

#include <cmath>
#include <limits>

using namespace plk::app;

PLK_TEST(gamepad_idle_is_the_idle_state)
{
	ChiakiControllerState idle;
	chiaki_controller_state_set_idle(&idle);
	ChiakiControllerState s = mapGamepad(GamepadInput());
	CHECK(chiaki_controller_state_equals(&s, &idle));
}

PLK_TEST(gamepad_buttons_by_position)
{
	struct Case { bool GamepadInput::*field; uint32_t button; };
	const Case cases[] = {
		{ &GamepadInput::a, CHIAKI_CONTROLLER_BUTTON_CROSS },
		{ &GamepadInput::b, CHIAKI_CONTROLLER_BUTTON_MOON },
		{ &GamepadInput::x, CHIAKI_CONTROLLER_BUTTON_BOX },
		{ &GamepadInput::y, CHIAKI_CONTROLLER_BUTTON_PYRAMID },
		{ &GamepadInput::left_shoulder, CHIAKI_CONTROLLER_BUTTON_L1 },
		{ &GamepadInput::right_shoulder, CHIAKI_CONTROLLER_BUTTON_R1 },
		{ &GamepadInput::left_stick_click, CHIAKI_CONTROLLER_BUTTON_L3 },
		{ &GamepadInput::right_stick_click, CHIAKI_CONTROLLER_BUTTON_R3 },
		{ &GamepadInput::dpad_up, CHIAKI_CONTROLLER_BUTTON_DPAD_UP },
		{ &GamepadInput::dpad_down, CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN },
		{ &GamepadInput::dpad_left, CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT },
		{ &GamepadInput::dpad_right, CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT },
		{ &GamepadInput::menu, CHIAKI_CONTROLLER_BUTTON_OPTIONS },
		{ &GamepadInput::options, CHIAKI_CONTROLLER_BUTTON_SHARE },
		{ &GamepadInput::home, CHIAKI_CONTROLLER_BUTTON_PS },
		{ &GamepadInput::touchpad_click, CHIAKI_CONTROLLER_BUTTON_TOUCHPAD },
	};
	for(const Case &c : cases)
	{
		GamepadInput in;
		in.*c.field = true;
		CHECK(mapGamepad(in).buttons == c.button);
	}
	GamepadInput two;
	two.a = true;
	two.right_shoulder = true;
	CHECK(mapGamepad(two).buttons == (CHIAKI_CONTROLLER_BUTTON_CROSS | CHIAKI_CONTROLLER_BUTTON_R1));
}

PLK_TEST(gamepad_sticks_and_triggers)
{
	GamepadInput in;
	in.left_x = 1.0f;
	in.left_y = 1.0f;   // up
	in.right_x = -1.0f;
	in.right_y = -0.5f; // half down
	in.left_trigger = 1.0f;
	in.right_trigger = 0.5f;
	ChiakiControllerState s = mapGamepad(in);
	CHECK(s.left_x == 32767);
	CHECK(s.left_y == -32767); // the console's y points down
	CHECK(s.right_x == -32767);
	CHECK(s.right_y > 16300 && s.right_y < 16450);
	CHECK(s.l2_state == 255);
	CHECK(s.r2_state == 128);

	// Out of range and not-a-number never wrap around.
	in.left_x = 1.7f;
	in.left_y = -3.0f;
	in.right_x = std::numeric_limits<float>::quiet_NaN();
	in.left_trigger = 2.0f;
	in.right_trigger = -1.0f;
	s = mapGamepad(in);
	CHECK(s.left_x == 32767);
	CHECK(s.left_y == 32767);
	CHECK(s.right_x == 0);
	CHECK(s.l2_state == 255);
	CHECK(s.r2_state == 0);
}
