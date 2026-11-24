// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Output report packing against vectors derived independently: a separate Python
// implementation of SDL3's DS5EffectsState_t layout and
// HIDAPI_DriverPS5_InternalSendJoystickEffect framing, the fork's
// Controller::SendDualSenseEffects / SetDualsenseMic flag logic, SDL's LED and
// player-light handling, the fork's DualSenseBtHaptics::SendReport layout, and
// zlib.crc32 for the CRCs. Bluetooth data[1] = seq << 4 as Linux hid-playstation.

#include "Protocol.h"
#include "test.h"

#include <cstring>
#include <string>

using namespace plk::dualsense;
using namespace plk::dualsense::protocol;

namespace {

const uint8_t kTL[11] = { 0x21, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
const uint8_t kTR[11] = { 0x26, 0xFF, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90 };

std::string Pack(const EffectsState &s, uint32_t include, bool v2, bool release, bool bt, uint8_t seq)
{
	uint8_t out[kBtOutputSize];
	memset(out, 0xEE, sizeof(out));
	size_t n = PackEffectsReport(s, include, v2, release, bt, seq, out);
	CHECK_EQ(n, bt ? kBtOutputSize : kUsbOutputSize);
	return t::ToHex(out, n);
}

} // namespace

TEST(output_usb_rumble_v2)
{
	EffectsState s;
	s.active = kGroupRumble;
	s.rumble_left = 0x80;
	s.rumble_right = 0x40;
	CHECK_EQ(Pack(s, s.active, true, false, false, 0),
		std::string("020240408000000000000000000000000000000000000000000000000000000000000000000000040000000000000000"));
}

TEST(output_usb_rumble_legacy_halves_values)
{
	EffectsState s;
	s.active = kGroupRumble;
	s.rumble_left = 0x81;
	s.rumble_right = 0x41;
	CHECK_EQ(Pack(s, s.active, false, false, false, 0),
		std::string("020340204000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"));
}

TEST(output_bt_rumble_triggers_intensity)
{
	EffectsState s;
	s.active = kGroupRumble | kGroupTriggers;
	s.rumble_left = 0xFF;
	s.rumble_right = 0x01;
	memcpy(s.trigger_left, kTL, 11);
	memcpy(s.trigger_right, kTR, 11);
	s.motor_power = 0x62;
	CHECK_EQ(Pack(s, s.active, true, false, true, 5),
		std::string("3150100e4001ff00000000000026ff102030405060708090210102030405060708090a0000000062000400000000000000000000000000000000000000000000000000000000000000004c3e49bb"));
}

TEST(output_bt_leds_and_mic)
{
	EffectsState s;
	s.active = kGroupLightbar | kGroupPlayerLeds | kGroupMicLed | kGroupPowerSave;
	s.lightbar[0] = 0x12;
	s.lightbar[1] = 0x34;
	s.lightbar[2] = 0x56;
	s.player_leds = PlayerLedsForIndex(2);
	s.mic_led = 1;
	s.power_save = 0x08;
	s.motor_power = 0x99; // not sent: no rumble or trigger group in the report
	CHECK_EQ(Pack(s, s.active, true, false, true, 15),
		std::string("31f0100017000000000000010800000000000000000000000000000000000000000000000000000000000000000035123456000000000000000000000000000000000000000000000000356a87d6"));
}

TEST(output_bt_release_leds_holds_led_values_back)
{
	EffectsState s;
	s.active = kGroupTriggers | kGroupLightbar | kGroupPlayerLeds;
	memcpy(s.trigger_left, kTL, 11);
	memcpy(s.trigger_right, kTR, 11);
	s.lightbar[0] = 0xAA;
	s.player_leds = 0x3F;
	CHECK_EQ(Pack(s, s.active & ~kGroupLeds, true, true, true, 0),
		std::string("3100100c48000000000000000026ff102030405060708090210102030405060708090a00000000000000000000000000000000000000000000000000000000000000000000000000000079cc42fa"));
}

TEST(output_bt_effects_off)
{
	EffectsState s = EffectsOff(0x00);
	CHECK_EQ(Pack(s, s.active, true, false, true, 3),
		std::string("3130100e400000000000000000050000000000000000000005000000000000000000000000000000000400000000000000000000000000000000000000000000000000000000000000005f2f4837"));
}

TEST(output_usb_everything_legacy)
{
	EffectsState s;
	s.active = kGroupRumble | kGroupTriggers | kGroupLightbar | kGroupPlayerLeds | kGroupMicLed | kGroupPowerSave;
	s.rumble_left = 0x10;
	s.rumble_right = 0x20;
	memcpy(s.trigger_left, kTL, 11);
	memcpy(s.trigger_right, kTR, 11);
	s.motor_power = 0xF0;
	s.lightbar[0] = 1;
	s.lightbar[1] = 2;
	s.lightbar[2] = 3;
	s.player_leds = PlayerLedsForIndex(0);
	s.mic_led = 2;
	s.power_save = 0x00;
	CHECK_EQ(Pack(s, s.active, false, false, false, 0),
		std::string("020f57100800000000020026ff102030405060708090210102030405060708090a00000000f000000000000024010203"));
}

TEST(output_include_mask_and_inactive_groups)
{
	EffectsState s;
	s.rumble_left = 0x55; // not active: must not appear
	s.lightbar[0] = 0x77;
	s.active = kGroupLightbar;
	uint8_t block[kEffectsBlockSize];
	FillEffectsBlock(s, ~0u, true, false, block);
	CHECK_EQ(block[0], 0);
	CHECK_EQ(block[1], 0x04);
	CHECK_EQ(block[3], 0);
	CHECK_EQ(block[44], 0x77);
	FillEffectsBlock(s, ~kGroupLeds, true, false, block);
	CHECK_EQ(block[1], 0);
	CHECK_EQ(block[44], 0);
}

TEST(output_bt_seq_nibble_and_crc)
{
	EffectsState s;
	s.active = kGroupRumble;
	uint8_t out[kBtOutputSize];
	for(int seq = 0; seq < 20; seq++)
	{
		PackEffectsReport(s, s.active, true, false, true, (uint8_t)seq, out);
		CHECK_EQ(out[1], (uint8_t)((seq & 0xF) << 4));
		CHECK_EQ(out[2], 0x10);
		CHECK(BtCrcValid(kCrcSeedOutput, out, kBtOutputSize));
	}
}

TEST(output_player_leds_table)
{
	CHECK_EQ(PlayerLedsForIndex(-1), 0x00);
	CHECK_EQ(PlayerLedsForIndex(0), 0x24);
	CHECK_EQ(PlayerLedsForIndex(4), 0x3F);
	CHECK_EQ(PlayerLedsForIndex(7), 0x24);
}

TEST(output_rumble_v2_rule)
{
	CHECK(UseRumbleV2(kProductDualSense, 0x0224));
	CHECK(UseRumbleV2(kProductDualSense, 0x0630));
	CHECK(!UseRumbleV2(kProductDualSense, 0x0223));
	CHECK(UseRumbleV2(kProductDualSense, 0)); // unknown: assume current firmware, as SDL
	CHECK(UseRumbleV2(kProductDualSenseEdge, 0x0100));
}

TEST(output_haptics_report_double_block)
{
	uint8_t samples[128];
	for(int i = 0; i < 128; i++)
		samples[i] = (uint8_t)(i * 7);
	uint8_t out[kBtHapticsSize];
	PackHapticsReport(3, 6, 48, true, samples, sizeof(samples), out);
	CHECK_EQ(t::ToHex(out, sizeof(out)),
		std::string("3230910466303006d24000070e151c232a31383f464d545b626970777e858c939aa1a8afb6bdc4cbd2d9e0e7eef5fc030a11181f262d343b424950575e656c737a81888f969da4abb2b9c0c7ced5dce3eaf1f8ff060d141b222930373e454c535a61686f767d848b9299a0a7aeb5bcc3cad1d8dfe6edf4fb020910171e252c333a41484f565d646b7279dc5d321d"));
}

TEST(output_haptics_report_single_block)
{
	uint8_t samples[64];
	for(int i = 0; i < 64; i++)
		samples[i] = (uint8_t)(0x80 + i);
	uint8_t out[kBtHapticsSize];
	PackHapticsReport(15, 255, 16, false, samples, sizeof(samples), out);
	CHECK_EQ(t::ToHex(out, sizeof(out)),
		std::string("32f09104661010ff9240808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9fa0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000073235061"));
}
