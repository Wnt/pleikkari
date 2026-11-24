// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// DualSense HID report layouts as pure functions: no IOKit, no threads, no clocks.
// Everything here is unit tested without hardware (tests/).
//
// Sources: SDL3 src/joystick/hidapi/SDL_hidapi_ps5.c (layouts, flag bits,
// calibration), Linux drivers/hid/hid-playstation.c (sequence nibble, feature CRC
// seed, firmware info offsets), and the Qt GUI's Controller::SendDualSenseEffects()
// (gui/src/controllermanager.cpp) for what this fork sends.

#pragma once

#include "DualSenseHID.h"

#include <cstddef>
#include <cstdint>

namespace plk::dualsense::protocol {

constexpr uint16_t kVendorSony = 0x054C;
constexpr uint16_t kProductDualSense = 0x0CE6;
constexpr uint16_t kProductDualSenseEdge = 0x0DF2;

constexpr uint8_t kReportIdUsbInput = 0x01;      // also the simple Bluetooth input report
constexpr uint8_t kReportIdBtInput = 0x31;
constexpr uint8_t kReportIdUsbOutput = 0x02;
constexpr uint8_t kReportIdBtOutput = 0x31;
constexpr uint8_t kReportIdBtHaptics = 0x32;
constexpr uint8_t kFeatureIdCalibration = 0x05;
constexpr uint8_t kFeatureIdFirmwareInfo = 0x20;
// Feature 0x09 (pairing info) carries the controller's Bluetooth MAC address. This
// driver never reads it.

constexpr size_t kUsbInputSize = 64;
constexpr size_t kBtInputSize = 78;
constexpr size_t kBtSimpleInputSize = 10;
constexpr size_t kUsbOutputSize = 48;            // what SDL sends: id + 47-byte effects block
constexpr size_t kBtOutputSize = 78;             // id, seq/tag, tag, 47-byte block, pad, CRC
constexpr size_t kEffectsBlockSize = 47;
constexpr size_t kBtHapticsSize = 142;
constexpr size_t kCalibrationSize = 41;
constexpr size_t kFirmwareInfoSize = 64;

// Bluetooth reports carry a CRC32 (zlib polynomial) over the HIDP transaction header
// byte followed by the report: 0xA1 for input (DATA|Input), 0xA2 for output
// (DATA|Output), 0xA3 for GET_REPORT feature replies. Stored little endian in the
// last four bytes.
constexpr uint8_t kCrcSeedInput = 0xA1;
constexpr uint8_t kCrcSeedOutput = 0xA2;
constexpr uint8_t kCrcSeedFeature = 0xA3;

// zlib-compatible: Crc32(b, Crc32(a)) == Crc32(a || b), Crc32("123456789") == 0xCBF43926.
uint32_t Crc32(const uint8_t *data, size_t size, uint32_t crc = 0);
uint32_t BtCrc32(uint8_t seed, const uint8_t *data, size_t size);
bool BtCrcValid(uint8_t seed, const uint8_t *report, size_t size);
void BtCrcStore(uint8_t seed, uint8_t *report, size_t size);

// ---------------------------------------------------------------------------
// Feature reports

struct ImuAxisCalibration {
	int16_t bias = 0;
	float sensitivity = 1.0f; // SDL's units: gyro ~64 (raw -> 1/1024 deg/s), accel ~1
};

struct Calibration {
	bool valid = false;               // false: nominal scaling, as SDL does
	ImuAxisCalibration axis[6];       // gyro pitch, yaw, roll, accel x, y, z
};

// Feature report 0x05 (41 bytes; over Bluetooth the last 4 are a CRC with seed 0xA3).
// Returns false and leaves `out` invalid (nominal scaling) if the report is short, its
// CRC is wrong, or the values fail SDL's sanity check.
bool ParseCalibration(const uint8_t *report, size_t size, bool bluetooth, Calibration *out);

struct FirmwareInfo {
	bool valid = false;
	uint16_t update_version = 0;
	uint32_t hardware_version = 0;
	uint32_t firmware_version = 0;
	char build_date[12] = {};
	char build_time[9] = {};
};

// Feature report 0x20 (64 bytes; over Bluetooth with a CRC, seed 0xA3).
bool ParseFirmwareInfo(const uint8_t *report, size_t size, bool bluetooth, FirmwareInfo *out);

// Which rumble encoding the controller wants. SDL's rule (InitDevice): the Edge,
// an unknown version (0) and update version >= 0x0224 use "improved rumble
// emulation" (block[38] bit 2, full-scale values); older firmware gets the legacy
// rumble emulation (block[0] bit 0) at half the value. The Qt GUI tests only
// `firmware_version < 0x0224`, see the note in Protocol.cpp.
bool UseRumbleV2(uint16_t product_id, uint16_t update_version);

// ---------------------------------------------------------------------------
// Input

constexpr uint32_t kSensorTicksPerUs = 3; // sensor timestamp unit: 1/3 microsecond

// Sensor timestamp at which SDL considers the Bluetooth connection LED animation
// finished, so a lightbar colour sticks (CheckPendingLEDReset): 3.4 s after power-up.
constexpr uint32_t kLedAnimationDoneTicks = 10200000;

enum class ParseResult { Ok, BadCrc, Unsupported };

// Stateful only for the sensor clock: it unwraps the 32-bit sensor timestamp into a
// 64-bit microsecond count. One parser per device; not thread-safe.
class InputParser {
public:
	void Reset();
	ParseResult Parse(const uint8_t *report, size_t size, bool bluetooth,
		const Calibration &calibration, InputState *out);

private:
	bool have_tick_ = false;
	uint32_t last_tick_ = 0;
	uint64_t ticks_ = 0;
};

// Calibrated IMU value in SDL's units, then converted: gyro -> rad/s, accel -> g.
float CalibratedGyro(const Calibration &c, int axis, int16_t raw);
float CalibratedAccel(const Calibration &c, int axis, int16_t raw);

// ---------------------------------------------------------------------------
// Output: effects (USB 0x02 / Bluetooth 0x31)
//
// The 47-byte effects block, offsets as in SDL's DS5EffectsState_t:
//   [0]  enable bits 1: 0x01 legacy rumble emulation, 0x02 rumble instead of audio
//        haptics, 0x04 right trigger effect, 0x08 left trigger effect
//   [1]  enable bits 2: 0x01 mic LED, 0x02 power save/mute control, 0x04 lightbar
//        colour, 0x08 release LEDs from the firmware, 0x10 player LEDs,
//        0x40 motor power level (block[36])
//   [2]  rumble right (light motor)       [3]  rumble left (heavy motor)
//   [8]  mic LED: 0 off, 1 on, 2 pulse    [9]  power save control / mute bits
//   [10..20] right trigger effect          [21..31] left trigger effect
//   [36] motor power level                 [38] enable bits 3: 0x04 improved rumble
//   [43] player LEDs                       [44..46] lightbar R, G, B

enum EffectGroup : uint32_t {
	kGroupRumble = 1u << 0,
	kGroupTriggers = 1u << 1,
	kGroupMotorPower = 1u << 2,
	kGroupLightbar = 1u << 3,
	kGroupPlayerLeds = 1u << 4,
	kGroupMicLed = 1u << 5,
	kGroupPowerSave = 1u << 6,
	kGroupLeds = kGroupLightbar | kGroupPlayerLeds,
};

// The complete effect state a client has set. A group's fields go into a report only
// when its bit is in `active`.
struct EffectsState {
	uint32_t active = 0;
	uint8_t rumble_left = 0, rumble_right = 0;
	uint8_t trigger_left[11] = {};
	uint8_t trigger_right[11] = {};
	uint8_t motor_power = 0;
	uint8_t lightbar[3] = {};
	uint8_t player_leds = 0;
	uint8_t mic_led = 0;
	uint8_t power_save = 0;
};

// The effects-off state written at shutdown: rumble 0 and trigger effect 0x05 (off)
// on both sides, the same as the Qt GUI's ~Controller().
EffectsState EffectsOff(uint8_t motor_power);

// Fills the 47-byte block. `include` masks `state.active` (used to hold the LEDs back
// during the Bluetooth connection animation). Motor power goes in whenever rumble or
// trigger effects do, as in SendDualSenseEffects().
void FillEffectsBlock(const EffectsState &state, uint32_t include, bool rumble_v2,
	bool release_leds, uint8_t block[kEffectsBlockSize]);

// Whole output report. Bluetooth: [0] 0x31, [1] seq << 4 (as Linux hid-playstation;
// SDL sends 0), [2] 0x10 tag, [3..49] block, CRC (seed 0xA2) in [74..77]. USB: [0]
// 0x02, [1..47] block. `out` must hold kBtOutputSize bytes. Returns the report size.
size_t PackEffectsReport(const EffectsState &state, uint32_t include, bool rumble_v2,
	bool release_leds, bool bluetooth, uint8_t bt_seq, uint8_t *out);

// SDL's player LED table (SetLightsForPlayerIndex): bit 5 switches without fading.
uint8_t PlayerLedsForIndex(int player_index);

// ---------------------------------------------------------------------------
// Output: Bluetooth haptics report 0x32 (142 bytes), as the Qt GUI's
// DualSenseBtHaptics::SendReport():
//   [0]        0x32
//   [1]        sequence number << 4
//   [2..7]     packet 0x11|0x80 (sized), length 4: audio flags 0x66, controller
//              buffer length (twice), packet counter
//   [8..9]     packet 0x12|0x80 (|0x40 when two 64-byte blocks follow), length 64
//   [10..137]  signed 8-bit stereo frames at 3 kHz (64 frames, or 32 + zero pad)
//   [138..141] CRC32, seed 0xA2
constexpr size_t kHapticsSamplesOffset = 10;
constexpr size_t kHapticsBlockSize = 64;
constexpr uint8_t kHapticsAudioFlags = 0x66;
constexpr int kHapticsSampleRate = 3000;
constexpr int kHapticsFrameBytes = 2;

void PackHapticsReport(uint8_t seq, uint8_t packet_counter, uint8_t controller_buffer,
	bool double_block, const uint8_t *samples, size_t size, uint8_t out[kBtHapticsSize]);

} // namespace plk::dualsense::protocol
