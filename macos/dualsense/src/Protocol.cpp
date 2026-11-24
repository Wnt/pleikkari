// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "Protocol.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace plk::dualsense::protocol {

namespace {

struct CrcTable {
	uint32_t entries[256];
	constexpr CrcTable() : entries()
	{
		for(uint32_t i = 0; i < 256; i++)
		{
			uint32_t c = i;
			for(int k = 0; k < 8; k++)
				c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
			entries[i] = c;
		}
	}
};

constexpr CrcTable kCrcTable;

inline uint16_t Le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline int16_t LeS16(const uint8_t *p) { return (int16_t)Le16(p); }
inline uint32_t Le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

constexpr float kGyroResPerDegree = 1024.0f;
constexpr float kAccelResPerG = 8192.0f;
constexpr float kPi = 3.14159265358979323846f;

inline int16_t AxisFromRaw(uint8_t raw) { return (int16_t)((int)raw * 257 - 32768); }

// Face buttons, d-pad and shoulder byte: identical in the simple and enhanced reports.
uint32_t DecodeButtons(uint8_t hat_face, uint8_t shoulders, uint32_t *extra)
{
	uint32_t b = 0;
	uint8_t face = hat_face >> 4;
	if(face & 0x01) b |= kButtonBox;
	if(face & 0x02) b |= kButtonCross;
	if(face & 0x04) b |= kButtonMoon;
	if(face & 0x08) b |= kButtonPyramid;
	switch(hat_face & 0x0F)
	{
		case 0: b |= kButtonDpadUp; break;
		case 1: b |= kButtonDpadUp | kButtonDpadRight; break;
		case 2: b |= kButtonDpadRight; break;
		case 3: b |= kButtonDpadDown | kButtonDpadRight; break;
		case 4: b |= kButtonDpadDown; break;
		case 5: b |= kButtonDpadDown | kButtonDpadLeft; break;
		case 6: b |= kButtonDpadLeft; break;
		case 7: b |= kButtonDpadUp | kButtonDpadLeft; break;
		default: break; // 8 = centred
	}
	if(shoulders & 0x01) b |= kButtonL1;
	if(shoulders & 0x02) b |= kButtonR1;
	if(shoulders & 0x04) *extra |= kExtraL2Digital;
	if(shoulders & 0x08) *extra |= kExtraR2Digital;
	if(shoulders & 0x10) b |= kButtonShare;
	if(shoulders & 0x20) b |= kButtonOptions;
	if(shoulders & 0x40) b |= kButtonL3;
	if(shoulders & 0x80) b |= kButtonR3;
	return b;
}

// SDL reports a full trigger when the analog value is 0 but the digital bit is set
// (third-party pads with digital triggers). Kept so values match what the GUI gets.
inline uint8_t TriggerValue(uint8_t analog, bool digital) { return (analog == 0 && digital) ? 0xFF : analog; }

void DecodeSticksAndTriggers(const uint8_t *sticks, uint8_t l2, uint8_t r2, InputState *out)
{
	out->raw_left_x = sticks[0];
	out->raw_left_y = sticks[1];
	out->raw_right_x = sticks[2];
	out->raw_right_y = sticks[3];
	out->left_x = AxisFromRaw(sticks[0]);
	out->left_y = AxisFromRaw(sticks[1]);
	out->right_x = AxisFromRaw(sticks[2]);
	out->right_y = AxisFromRaw(sticks[3]);
	out->l2_state = TriggerValue(l2, out->extra_buttons & kExtraL2Digital);
	out->r2_state = TriggerValue(r2, out->extra_buttons & kExtraR2Digital);
}

} // namespace

uint32_t Crc32(const uint8_t *data, size_t size, uint32_t crc)
{
	crc = ~crc;
	while(size--)
		crc = kCrcTable.entries[(crc ^ *data++) & 0xFF] ^ (crc >> 8);
	return ~crc;
}

uint32_t BtCrc32(uint8_t seed, const uint8_t *data, size_t size)
{
	return Crc32(data, size, Crc32(&seed, 1));
}

bool BtCrcValid(uint8_t seed, const uint8_t *report, size_t size)
{
	if(size < 5)
		return false;
	return BtCrc32(seed, report, size - 4) == Le32(report + size - 4);
}

void BtCrcStore(uint8_t seed, uint8_t *report, size_t size)
{
	uint32_t crc = BtCrc32(seed, report, size - 4);
	report[size - 4] = (uint8_t)crc;
	report[size - 3] = (uint8_t)(crc >> 8);
	report[size - 2] = (uint8_t)(crc >> 16);
	report[size - 1] = (uint8_t)(crc >> 24);
}

bool ParseCalibration(const uint8_t *d, size_t size, bool bluetooth, Calibration *out)
{
	*out = Calibration();
	if(size < 35 || d[0] != kFeatureIdCalibration)
		return false;
	if(bluetooth && (size < kCalibrationSize || !BtCrcValid(kCrcSeedFeature, d, kCalibrationSize)))
		return false;

	int16_t gyro_pitch_bias = LeS16(d + 1), gyro_yaw_bias = LeS16(d + 3), gyro_roll_bias = LeS16(d + 5);
	int16_t gyro_pitch_plus = LeS16(d + 7), gyro_pitch_minus = LeS16(d + 9);
	int16_t gyro_yaw_plus = LeS16(d + 11), gyro_yaw_minus = LeS16(d + 13);
	int16_t gyro_roll_plus = LeS16(d + 15), gyro_roll_minus = LeS16(d + 17);
	int16_t gyro_speed_plus = LeS16(d + 19), gyro_speed_minus = LeS16(d + 21);
	int16_t acc_x_plus = LeS16(d + 23), acc_x_minus = LeS16(d + 25);
	int16_t acc_y_plus = LeS16(d + 27), acc_y_minus = LeS16(d + 29);
	int16_t acc_z_plus = LeS16(d + 31), acc_z_minus = LeS16(d + 33);

	// SDL divides by these without a check; a zero span would give inf/NaN, and a NaN
	// slips through SDL's sanity test. Treat it as a bad calibration instead.
	int gyro_span[3] = { gyro_pitch_plus - gyro_pitch_minus, gyro_yaw_plus - gyro_yaw_minus, gyro_roll_plus - gyro_roll_minus };
	int16_t acc_plus[3] = { acc_x_plus, acc_y_plus, acc_z_plus };
	int16_t acc_minus[3] = { acc_x_minus, acc_y_minus, acc_z_minus };
	int16_t gyro_bias[3] = { gyro_pitch_bias, gyro_yaw_bias, gyro_roll_bias };

	Calibration c;
	float numerator = (float)(gyro_speed_plus + gyro_speed_minus) * kGyroResPerDegree;
	for(int i = 0; i < 3; i++)
	{
		if(gyro_span[i] == 0)
			return false;
		c.axis[i].bias = gyro_bias[i];
		c.axis[i].sensitivity = numerator / (float)gyro_span[i];
	}
	for(int i = 0; i < 3; i++)
	{
		int16_t range_2g = (int16_t)(acc_plus[i] - acc_minus[i]); // SDL keeps this in a Sint16
		if(range_2g == 0)
			return false;
		c.axis[3 + i].bias = (int16_t)(acc_plus[i] - range_2g / 2);
		c.axis[3 + i].sensitivity = 2.0f * kAccelResPerG / (float)range_2g;
	}
	for(int i = 0; i < 6; i++)
	{
		float divisor = i < 3 ? 64.0f : 1.0f;
		if(std::abs((int)c.axis[i].bias) > 1024 || !(std::fabs(1.0f - c.axis[i].sensitivity / divisor) <= 0.5f))
			return false;
	}
	c.valid = true;
	*out = c;
	return true;
}

bool ParseFirmwareInfo(const uint8_t *d, size_t size, bool bluetooth, FirmwareInfo *out)
{
	*out = FirmwareInfo();
	if(size < 46 || d[0] != kFeatureIdFirmwareInfo)
		return false;
	if(bluetooth && (size < kFirmwareInfoSize || !BtCrcValid(kCrcSeedFeature, d, kFirmwareInfoSize)))
		return false;
	FirmwareInfo f;
	for(int i = 0; i < 11; i++)
		f.build_date[i] = (d[1 + i] >= 0x20 && d[1 + i] < 0x7F) ? (char)d[1 + i] : '?';
	for(int i = 0; i < 8; i++)
		f.build_time[i] = (d[12 + i] >= 0x20 && d[12 + i] < 0x7F) ? (char)d[12 + i] : '?';
	f.hardware_version = Le32(d + 24);
	f.firmware_version = Le32(d + 28);
	f.update_version = Le16(d + 44);
	f.valid = true;
	*out = f;
	return true;
}

// The Qt GUI decides with `firmware_version < 0x0224` alone, where firmware_version
// is SDL's. SDL only reads feature 0x20 at init when the first report is already an
// enhanced one; a Bluetooth controller that has just connected is still in simple
// mode then, so SDL reports 0 and the GUI falls back to the legacy, halved rumble
// although the controller supports the improved one (SDL itself treats 0 as new).
// The Edge's update version is numbered differently (Linux hid-playstation notes
// this), which SDL handles by always using the improved rumble for it.
bool UseRumbleV2(uint16_t product_id, uint16_t update_version)
{
	return product_id == kProductDualSenseEdge || update_version == 0 || update_version >= 0x0224;
}

float CalibratedGyro(const Calibration &c, int axis, int16_t raw)
{
	float v = c.valid ? (float)(raw - c.axis[axis].bias) * c.axis[axis].sensitivity : (float)raw * 64.0f;
	return v / kGyroResPerDegree * kPi / 180.0f;
}

float CalibratedAccel(const Calibration &c, int axis, int16_t raw)
{
	float v = c.valid ? (float)(raw - c.axis[3 + axis].bias) * c.axis[3 + axis].sensitivity : (float)raw;
	return v / kAccelResPerG;
}

void InputParser::Reset()
{
	have_tick_ = false;
	last_tick_ = 0;
	ticks_ = 0;
}

ParseResult InputParser::Parse(const uint8_t *report, size_t size, bool bluetooth,
	const Calibration &calibration, InputState *out)
{
	if(size < 1)
		return ParseResult::Unsupported;
	const uint8_t id = report[0];
	const uint8_t *p = nullptr; // enhanced payload: byte 0 = left stick X
	size_t payload_size = 0;

	if(bluetooth && id == kReportIdBtInput)
	{
		if(size < kBtInputSize)
			return ParseResult::Unsupported;
		if(!BtCrcValid(kCrcSeedInput, report, kBtInputSize))
			return ParseResult::BadCrc;
		out->bt_header = report[1];
		p = report + 2;
		payload_size = kBtInputSize - 2 - 4;
	}
	else if(id == kReportIdUsbInput && !bluetooth && size >= kUsbInputSize)
	{
		out->bt_header = 0;
		p = report + 1;
		payload_size = size - 1;
	}
	else if(id == kReportIdUsbInput && bluetooth && size >= kBtSimpleInputSize)
	{
		// Simple report: sticks, buttons (3 bytes, counter in the top 6 bits of the
		// third), triggers. SDL's PS5SimpleStatePacket_t.
		const uint8_t *s = report + 1;
		out->report_id = id;
		out->mode = ReportMode::BluetoothSimple;
		out->bt_header = 0;
		out->extra_buttons = 0;
		out->buttons = DecodeButtons(s[4], s[5], &out->extra_buttons);
		if(s[6] & 0x01) out->buttons |= kButtonPS;
		if(s[6] & 0x02) out->buttons |= kButtonTouchpad;
		out->counter = s[6] >> 2;
		DecodeSticksAndTriggers(s, s[7], s[8], out);
		out->motion_valid = false;
		out->touches[0] = Touch();
		out->touches[1] = Touch();
		out->sensor_timestamp = 0;
		out->sensor_time_us = 0;
		out->battery_valid = false;
		out->battery_status = BatteryStatus::Unknown;
		out->battery_percent = 0;
		out->headphones = out->microphone = false;
		return ParseResult::Ok;
	}
	else
		return ParseResult::Unsupported;

	if(payload_size < 54)
		return ParseResult::Unsupported;

	// Enhanced payload (SDL PS5StatePacket_t, Linux struct dualsense_input_report):
	//  0..3 sticks, 4..5 triggers, 6 counter, 7..10 buttons, 11..14 ?, 15..20 gyro,
	//  21..26 accel, 27..30 sensor timestamp, 31 temperature, 32..39 touch points,
	//  40..51 ?, 52 battery, 53 status.
	out->report_id = id;
	out->mode = ReportMode::Enhanced;
	out->counter = p[6];
	out->extra_buttons = 0;
	out->buttons = DecodeButtons(p[7], p[8], &out->extra_buttons);
	const uint8_t b3 = p[9];
	if(b3 & 0x01) out->buttons |= kButtonPS;
	if(b3 & 0x02) out->buttons |= kButtonTouchpad;
	if(b3 & 0x04) out->extra_buttons |= kExtraMute;
	if(b3 & 0x10) out->extra_buttons |= kExtraEdgeFnLeft;
	if(b3 & 0x20) out->extra_buttons |= kExtraEdgeFnRight;
	if(b3 & 0x40) out->extra_buttons |= kExtraEdgePaddleLeft;
	if(b3 & 0x80) out->extra_buttons |= kExtraEdgePaddleRight;
	DecodeSticksAndTriggers(p, p[4], p[5], out);

	for(int i = 0; i < 3; i++)
	{
		out->raw_gyro[i] = LeS16(p + 15 + 2 * i);
		out->raw_accel[i] = LeS16(p + 21 + 2 * i);
	}
	out->gyro_x = CalibratedGyro(calibration, 0, out->raw_gyro[0]);
	out->gyro_y = CalibratedGyro(calibration, 1, out->raw_gyro[1]);
	out->gyro_z = CalibratedGyro(calibration, 2, out->raw_gyro[2]);
	out->accel_x = CalibratedAccel(calibration, 0, out->raw_accel[0]);
	out->accel_y = CalibratedAccel(calibration, 1, out->raw_accel[1]);
	out->accel_z = CalibratedAccel(calibration, 2, out->raw_accel[2]);
	out->motion_valid = true;

	// Unwrap with modular arithmetic. (SDL counts an unchanged timestamp as a full
	// 2^32 wrap, adding ~1431 s; a delta of 0 is just 0.)
	const uint32_t tick = Le32(p + 27);
	out->sensor_timestamp = tick;
	if(!have_tick_)
	{
		have_tick_ = true;
		ticks_ = 0;
	}
	else
		ticks_ += (uint32_t)(tick - last_tick_);
	last_tick_ = tick;
	out->sensor_time_us = ticks_ / kSensorTicksPerUs;

	for(int i = 0; i < 2; i++)
	{
		const uint8_t *t = p + 32 + 4 * i;
		out->touches[i].active = (t[0] & 0x80) == 0;
		out->touches[i].id = t[0] & 0x7F;
		out->touches[i].x = (uint16_t)(t[1] | ((t[2] & 0x0F) << 8));
		out->touches[i].y = (uint16_t)((t[2] >> 4) | (t[3] << 4));
	}

	const uint8_t level = p[52] & 0x0F;
	const uint8_t status = p[52] >> 4;
	out->battery_valid = true;
	switch(status)
	{
		case 0x0:
			out->battery_status = BatteryStatus::Discharging;
			out->battery_percent = (uint8_t)std::min(level * 10 + 5, 100);
			break;
		case 0x1:
			out->battery_status = BatteryStatus::Charging;
			out->battery_percent = (uint8_t)std::min(level * 10 + 5, 100);
			break;
		case 0x2:
			out->battery_status = BatteryStatus::Full;
			out->battery_percent = 100;
			break;
		case 0xA: case 0xB: case 0xF:
			out->battery_status = BatteryStatus::Error;
			out->battery_percent = 0;
			break;
		default:
			out->battery_status = BatteryStatus::Unknown;
			out->battery_percent = 0;
			break;
	}
	out->headphones = (p[53] & 0x01) != 0;
	out->microphone = (p[53] & 0x02) != 0;
	return ParseResult::Ok;
}

EffectsState EffectsOff(uint8_t motor_power)
{
	EffectsState s;
	s.active = kGroupRumble | kGroupTriggers;
	s.trigger_left[0] = 0x05;
	s.trigger_right[0] = 0x05;
	s.motor_power = motor_power;
	return s;
}

void FillEffectsBlock(const EffectsState &s, uint32_t include, bool rumble_v2,
	bool release_leds, uint8_t block[kEffectsBlockSize])
{
	memset(block, 0, kEffectsBlockSize);
	const uint32_t g = s.active & include;
	if(g & (kGroupRumble | kGroupTriggers))
	{
		block[1] |= 0x40;
		block[36] = s.motor_power;
	}
	if(g & kGroupRumble)
	{
		if(rumble_v2)
		{
			block[38] |= 0x04;
			block[3] = s.rumble_left;
			block[2] = s.rumble_right;
		}
		else
		{
			block[0] |= 0x01;
			block[3] = s.rumble_left >> 1;
			block[2] = s.rumble_right >> 1;
		}
		block[0] |= 0x02;
	}
	if(g & kGroupTriggers)
	{
		block[0] |= 0x04 | 0x08;
		memcpy(block + 10, s.trigger_right, 11);
		memcpy(block + 21, s.trigger_left, 11);
	}
	if(g & kGroupMicLed)
	{
		block[1] |= 0x01;
		block[8] = s.mic_led;
	}
	if(g & kGroupPowerSave)
	{
		block[1] |= 0x02;
		block[9] = s.power_save;
	}
	if(g & kGroupLightbar)
	{
		block[1] |= 0x04;
		block[44] = s.lightbar[0];
		block[45] = s.lightbar[1];
		block[46] = s.lightbar[2];
	}
	if(release_leds)
		block[1] |= 0x08;
	if(g & kGroupPlayerLeds)
	{
		block[1] |= 0x10;
		block[43] = s.player_leds;
	}
}

size_t PackEffectsReport(const EffectsState &state, uint32_t include, bool rumble_v2,
	bool release_leds, bool bluetooth, uint8_t bt_seq, uint8_t *out)
{
	if(bluetooth)
	{
		memset(out, 0, kBtOutputSize);
		out[0] = kReportIdBtOutput;
		out[1] = (uint8_t)((bt_seq & 0x0F) << 4);
		out[2] = 0x10;
		FillEffectsBlock(state, include, rumble_v2, release_leds, out + 3);
		BtCrcStore(kCrcSeedOutput, out, kBtOutputSize);
		return kBtOutputSize;
	}
	memset(out, 0, kUsbOutputSize);
	out[0] = kReportIdUsbOutput;
	FillEffectsBlock(state, include, rumble_v2, release_leds, out + 1);
	return kUsbOutputSize;
}

uint8_t PlayerLedsForIndex(int player_index)
{
	static const uint8_t lights[] = { 0x04, 0x0A, 0x15, 0x1B, 0x1F, 0x11, 0x0E };
	if(player_index < 0)
		return 0x00;
	return lights[player_index % (int)sizeof(lights)] | 0x20;
}

void PackHapticsReport(uint8_t seq, uint8_t packet_counter, uint8_t controller_buffer,
	bool double_block, const uint8_t *samples, size_t size, uint8_t out[kBtHapticsSize])
{
	memset(out, 0, kBtHapticsSize);
	out[0] = kReportIdBtHaptics;
	out[1] = (uint8_t)(seq << 4);
	out[2] = 0x11 | 0x80;
	out[3] = 4;
	out[4] = kHapticsAudioFlags;
	out[5] = controller_buffer;
	out[6] = controller_buffer;
	out[7] = packet_counter;
	out[8] = 0x12 | 0x80 | (double_block ? 0x40 : 0);
	out[9] = kHapticsBlockSize;
	memcpy(out + kHapticsSamplesOffset, samples, std::min(size, kBtHapticsSize - 4 - kHapticsSamplesOffset));
	BtCrcStore(kCrcSeedOutput, out, kBtHapticsSize);
}

} // namespace plk::dualsense::protocol
