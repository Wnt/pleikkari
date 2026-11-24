// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// ControllerHub without hardware: the encodings and the event paths with no controller
// attached. (The hub never starts a Manager here: that would open the real DualSense.)

#include "Controller.h"
#include "TestHarness.h"

#include <cstring>

using namespace plk::app;

PLK_TEST(controller_motor_power_encoding)
{
	// The Qt app's table: trigger reduction high nibble, rumble reduction low nibble.
	CHECK(encodeMotorPower(Strong, Strong) == 0x00);
	CHECK(encodeMotorPower(Off, Strong) == 0xF0);
	CHECK(encodeMotorPower(Strong, Off) == 0x0F);
	CHECK(encodeMotorPower(Weak, Weak) == 0x93);
	CHECK(encodeMotorPower(Medium, Medium) == 0x62);
	CHECK(encodeMotorPower(Weak, Medium) == 0x92);
	CHECK(encodeMotorPower(Off, Off) == 0xFF);
}

PLK_TEST(controller_events_without_devices)
{
	ControllerHub hub;
	CHECK(hub.deviceCount() == 0);
	hub.onRumble(10, 20);
	ChiakiTriggerEffectsEvent e;
	memset(&e, 0, sizeof(e));
	e.type_left = 0x21;
	e.type_right = 0x26;
	e.mask = 0x02;
	hub.onTriggerEffects(e);
	hub.onTriggerIntensity(Off);
	hub.onTriggerEffects(e);
	hub.onHapticIntensity(Off);
	hub.onRumble(1, 2);
	const uint8_t rgb[3] = { 1, 2, 3 };
	hub.onLedColor(rgb);
	hub.onPlayerIndex(1);
	hub.onMotionReset();
	int16_t pcm[60] = {};
	hub.onHapticsFrame(reinterpret_cast<const uint8_t *>(pcm), sizeof(pcm));
	hub.onHapticsFrame(reinterpret_cast<const uint8_t *>(pcm), 3); // odd size: ignored
	hub.setSessionActive(true);
	hub.setSessionActive(false);
	const std::string line = hub.statsLine();
	CHECK(line.find("devices=0") != std::string::npos);
	CHECK(line.find("rumble=2 (dropped 1") != std::string::npos);
	CHECK(line.find("triggers=2 (dropped 1)") != std::string::npos);
	CHECK(line.find("haptics_frames=1") != std::string::npos);
	hub.stop(); // never started: nothing to do
}
