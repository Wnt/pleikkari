// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Input report and feature report parsing: synthetic reports with known values
// (layouts from SDL's PS5StatePacket_t / PS5SimpleStatePacket_t and Linux
// hid-playstation), and real reports captured read-only from a DualSense over
// Bluetooth (tests/fixtures, see the README there).

#include "Protocol.h"
#include "test.h"

#include <cmath>
#include <cstring>

using namespace plk::dualsense;
using namespace plk::dualsense::protocol;

namespace {

constexpr float kPi = 3.14159265358979323846f;

void Put16(uint8_t *p, int v)
{
	p[0] = (uint8_t)(v & 0xFF);
	p[1] = (uint8_t)((v >> 8) & 0xFF);
}

void Put32(uint8_t *p, uint32_t v)
{
	for(int i = 0; i < 4; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}

// Enhanced payload (starts at the left stick X byte) with known values.
void FillPayload(uint8_t *p, uint32_t sensor_ts)
{
	p[0] = 0x00;  // LX
	p[1] = 0xFF;  // LY
	p[2] = 0x80;  // RX
	p[3] = 0x7F;  // RY
	p[4] = 0x10;  // L2
	p[5] = 0xF0;  // R2
	p[6] = 0x42;  // counter
	p[7] = 0xA3;  // cross (0x20) + triangle (0x80), hat 3 = down-right
	p[8] = 0xA1;  // L1, options, R3
	p[9] = 0x97;  // PS, touchpad, mute, Edge left Fn, Edge right paddle
	Put16(p + 15, 100);
	Put16(p + 17, -200);
	Put16(p + 19, 0x7FFF);
	Put16(p + 21, 8192);
	Put16(p + 23, -8192);
	Put16(p + 25, 0);
	Put32(p + 27, sensor_ts);
	// touch 0: down, id 5, x 1919, y 1079
	p[32] = 0x05;
	p[33] = 0x7F;
	p[34] = 0x77;
	p[35] = 0x43;
	// touch 1: up, id 6, x 3, y 4
	p[36] = 0x86;
	p[37] = 0x03;
	p[38] = 0x40;
	p[39] = 0x00;
	p[52] = 0x15; // charging, level 5
	p[53] = 0x03; // headphones, mic
}

std::vector<uint8_t> UsbReport(uint32_t sensor_ts)
{
	std::vector<uint8_t> r(kUsbInputSize, 0);
	r[0] = kReportIdUsbInput;
	FillPayload(r.data() + 1, sensor_ts);
	return r;
}

std::vector<uint8_t> BtReport(uint32_t sensor_ts, uint8_t header)
{
	std::vector<uint8_t> r(kBtInputSize, 0);
	r[0] = kReportIdBtInput;
	r[1] = header;
	FillPayload(r.data() + 2, sensor_ts);
	BtCrcStore(kCrcSeedInput, r.data(), r.size());
	return r;
}

void CheckKnownValues(const InputState &s)
{
	CHECK(s.mode == ReportMode::Enhanced);
	CHECK_EQ(s.left_x, (int16_t)-32768);
	CHECK_EQ(s.left_y, (int16_t)32767);
	CHECK_EQ(s.right_x, (int16_t)128);
	CHECK_EQ(s.right_y, (int16_t)-129);
	CHECK_EQ(s.raw_left_y, 0xFF);
	CHECK_EQ(s.l2_state, 0x10);
	CHECK_EQ(s.r2_state, 0xF0);
	CHECK_EQ(s.counter, 0x42);
	CHECK_EQ(s.buttons, (uint32_t)(kButtonCross | kButtonPyramid | kButtonDpadDown | kButtonDpadRight
		| kButtonL1 | kButtonOptions | kButtonR3 | kButtonPS | kButtonTouchpad));
	CHECK_EQ(s.extra_buttons, (uint32_t)(kExtraMute | kExtraEdgeFnLeft | kExtraEdgePaddleRight));
	CHECK_EQ(s.raw_gyro[0], 100);
	CHECK_EQ(s.raw_gyro[1], -200);
	CHECK_EQ(s.raw_gyro[2], 0x7FFF);
	CHECK_EQ(s.raw_accel[0], 8192);
	CHECK_EQ(s.raw_accel[1], -8192);
	CHECK(s.motion_valid);
	// No calibration: SDL's nominal scaling, raw * 64 / 1024 deg/s and raw / 8192 g.
	CHECK_NEAR(s.gyro_x, 6.25 * kPi / 180.0, 1e-6);
	CHECK_NEAR(s.gyro_y, -12.5 * kPi / 180.0, 1e-6);
	CHECK_NEAR(s.accel_x, 1.0, 1e-6);
	CHECK_NEAR(s.accel_y, -1.0, 1e-6);
	CHECK_NEAR(s.accel_z, 0.0, 1e-6);
	CHECK(s.touches[0].active);
	CHECK_EQ(s.touches[0].id, 5);
	CHECK_EQ(s.touches[0].x, 1919);
	CHECK_EQ(s.touches[0].y, 1079);
	CHECK(!s.touches[1].active);
	CHECK_EQ(s.touches[1].id, 6);
	CHECK_EQ(s.touches[1].x, 3);
	CHECK_EQ(s.touches[1].y, 4);
	CHECK(s.battery_valid);
	CHECK(s.battery_status == BatteryStatus::Charging);
	CHECK_EQ(s.battery_percent, 55);
	CHECK(s.headphones);
	CHECK(s.microphone);
}

std::vector<uint8_t> CalibrationReport(int gyro_bias, int accel_x_offset, bool bluetooth)
{
	std::vector<uint8_t> r(kCalibrationSize, 0);
	r[0] = kFeatureIdCalibration;
	Put16(&r[1], gyro_bias);   // pitch bias
	Put16(&r[3], -gyro_bias);  // yaw bias
	Put16(&r[5], 0);           // roll bias
	for(int axis = 0; axis < 3; axis++)
	{
		Put16(&r[7 + 4 * axis], 8640);   // plus
		Put16(&r[9 + 4 * axis], -8640);  // minus
	}
	Put16(&r[19], 540); // speed plus
	Put16(&r[21], 540); // speed minus -> sensitivity 1080 * 1024 / 17280 = 64
	Put16(&r[23], 8192 + accel_x_offset);
	Put16(&r[25], -8192 + accel_x_offset);
	Put16(&r[27], 8192);
	Put16(&r[29], -8192);
	Put16(&r[31], 8192);
	Put16(&r[33], -8192);
	if(bluetooth)
		BtCrcStore(kCrcSeedFeature, r.data(), r.size());
	return r;
}

} // namespace

TEST(input_usb_report_known_values)
{
	InputParser parser;
	Calibration none;
	auto r = UsbReport(1000);
	InputState s;
	CHECK(parser.Parse(r.data(), r.size(), false, none, &s) == ParseResult::Ok);
	CHECK_EQ(s.report_id, 0x01);
	CHECK_EQ(s.bt_header, 0);
	CheckKnownValues(s);
}

TEST(input_bt_report_known_values_and_crc)
{
	InputParser parser;
	Calibration none;
	auto r = BtReport(1000, 0x50);
	InputState s;
	CHECK(parser.Parse(r.data(), r.size(), true, none, &s) == ParseResult::Ok);
	CHECK_EQ(s.report_id, 0x31);
	CHECK_EQ(s.bt_header, 0x50);
	CheckKnownValues(s);

	r[20] ^= 0x40;
	CHECK(parser.Parse(r.data(), r.size(), true, none, &s) == ParseResult::BadCrc);
	auto short_report = BtReport(1000, 0x50);
	CHECK(parser.Parse(short_report.data(), short_report.size() - 1, true, none, &s) == ParseResult::Unsupported);
	// A 0x31 report on USB, or a USB-sized 0x01 on Bluetooth, is not what it seems.
	auto bt = BtReport(1000, 0);
	CHECK(parser.Parse(bt.data(), bt.size(), false, none, &s) == ParseResult::Unsupported);
}

TEST(input_bt_simple_report)
{
	InputParser parser;
	Calibration none;
	const uint8_t r[10] = { 0x01, 0x10, 0x20, 0x30, 0x40, 0x18, 0x14, (0x2A << 2) | 0x01, 0x00, 0x99 };
	InputState s;
	CHECK(parser.Parse(r, sizeof(r), true, none, &s) == ParseResult::Ok);
	CHECK(s.mode == ReportMode::BluetoothSimple);
	CHECK_EQ(s.buttons, (uint32_t)(kButtonBox | kButtonShare | kButtonPS)); // hat 8 = centred
	CHECK_EQ(s.extra_buttons, (uint32_t)kExtraL2Digital);
	CHECK_EQ(s.counter, 0x2A);
	CHECK_EQ(s.raw_left_x, 0x10);
	CHECK_EQ(s.left_x, (int16_t)(0x10 * 257 - 32768));
	CHECK_EQ(s.right_y, (int16_t)(0x40 * 257 - 32768));
	CHECK_EQ(s.l2_state, 0xFF); // analog 0 with the digital bit: SDL reports full
	CHECK_EQ(s.r2_state, 0x99);
	CHECK(!s.motion_valid);
	CHECK(!s.touches[0].active);
	CHECK(!s.battery_valid);
}

TEST(input_hat_directions)
{
	InputParser parser;
	Calibration none;
	const uint32_t expect[9] = {
		kButtonDpadUp, kButtonDpadUp | kButtonDpadRight, kButtonDpadRight, kButtonDpadDown | kButtonDpadRight,
		kButtonDpadDown, kButtonDpadDown | kButtonDpadLeft, kButtonDpadLeft, kButtonDpadUp | kButtonDpadLeft, 0
	};
	for(uint8_t hat = 0; hat < 9; hat++)
	{
		auto r = UsbReport(0);
		r[8] = hat;
		r[9] = 0;
		r[10] = 0;
		InputState s;
		CHECK(parser.Parse(r.data(), r.size(), false, none, &s) == ParseResult::Ok);
		CHECK_EQ(s.buttons, expect[hat]);
	}
}

TEST(input_sensor_clock_unwraps)
{
	InputParser parser;
	Calibration none;
	InputState s;
	const uint32_t ticks[] = { 0xFFFFFFF0u, 0x00000010u, 0x00000010u, 0x00000310u };
	const uint64_t expect_us[] = { 0, 32 / 3, 32 / 3, (32 + 0x300) / 3 };
	for(int i = 0; i < 4; i++)
	{
		auto r = UsbReport(ticks[i]);
		CHECK(parser.Parse(r.data(), r.size(), false, none, &s) == ParseResult::Ok);
		CHECK_EQ(s.sensor_timestamp, ticks[i]);
		CHECK_EQ(s.sensor_time_us, expect_us[i]);
	}
	parser.Reset();
	auto r = UsbReport(5);
	CHECK(parser.Parse(r.data(), r.size(), false, none, &s) == ParseResult::Ok);
	CHECK_EQ(s.sensor_time_us, (uint64_t)0);
}

TEST(input_battery_states)
{
	InputParser parser;
	Calibration none;
	struct { uint8_t raw; BatteryStatus status; uint8_t percent; } cases[] = {
		{ 0x00, BatteryStatus::Discharging, 5 },
		{ 0x0A, BatteryStatus::Discharging, 100 },
		{ 0x08, BatteryStatus::Discharging, 85 },
		{ 0x13, BatteryStatus::Charging, 35 },
		{ 0x20, BatteryStatus::Full, 100 },
		{ 0xB0, BatteryStatus::Error, 0 },
		{ 0x50, BatteryStatus::Unknown, 0 },
	};
	for(auto &c : cases)
	{
		auto r = UsbReport(0);
		r[1 + 52] = c.raw;
		InputState s;
		CHECK(parser.Parse(r.data(), r.size(), false, none, &s) == ParseResult::Ok);
		CHECK(s.battery_status == c.status);
		CHECK_EQ(s.battery_percent, c.percent);
	}
}

TEST(calibration_parse_and_apply)
{
	Calibration c;
	auto usb = CalibrationReport(10, 100, false);
	CHECK(ParseCalibration(usb.data(), usb.size(), false, &c));
	CHECK(c.valid);
	CHECK_EQ(c.axis[0].bias, 10);
	CHECK_EQ(c.axis[1].bias, -10);
	CHECK_NEAR(c.axis[0].sensitivity, 64.0, 1e-4);
	CHECK_EQ(c.axis[3].bias, 100);
	CHECK_NEAR(c.axis[3].sensitivity, 1.0, 1e-6);
	// (110 - 10) * 64 / 1024 = 6.25 deg/s; (8292 - 100) / 8192 = 1 g
	CHECK_NEAR(CalibratedGyro(c, 0, 110), 6.25 * kPi / 180.0, 1e-6);
	CHECK_NEAR(CalibratedAccel(c, 0, 8292), 1.0, 1e-6);

	auto bt = CalibrationReport(10, 100, true);
	CHECK(ParseCalibration(bt.data(), bt.size(), true, &c));
	bt[5] ^= 1;
	CHECK(!ParseCalibration(bt.data(), bt.size(), true, &c));
	CHECK(!c.valid);
	CHECK(!ParseCalibration(usb.data(), usb.size(), true, &c)); // no CRC on a USB report
}

TEST(calibration_rejects_bad_values)
{
	Calibration c;
	auto big_bias = CalibrationReport(2000, 0, false);
	CHECK(!ParseCalibration(big_bias.data(), big_bias.size(), false, &c));
	auto zero_span = CalibrationReport(0, 0, false);
	Put16(&zero_span[7], 0);
	Put16(&zero_span[9], 0);
	CHECK(!ParseCalibration(zero_span.data(), zero_span.size(), false, &c));
	auto short_report = CalibrationReport(0, 0, false);
	CHECK(!ParseCalibration(short_report.data(), 30, false, &c));
	// Uncalibrated fallback scaling.
	CHECK_NEAR(CalibratedGyro(c, 0, 16), 1.0 * kPi / 180.0, 1e-6);
	CHECK_NEAR(CalibratedAccel(c, 2, -4096), -0.5, 1e-6);
}

TEST(firmware_info_parse)
{
	std::vector<uint8_t> r(kFirmwareInfoSize, 0);
	r[0] = kFeatureIdFirmwareInfo;
	memcpy(&r[1], "Jun 12 2024", 11);
	memcpy(&r[12], "10:21:33", 8);
	Put32(&r[24], 0x00000419);
	Put32(&r[28], 0x0110002A);
	Put16(&r[44], 0x0630);
	FirmwareInfo f;
	CHECK(ParseFirmwareInfo(r.data(), r.size(), false, &f));
	CHECK_EQ(std::string(f.build_date), std::string("Jun 12 2024"));
	CHECK_EQ(std::string(f.build_time), std::string("10:21:33"));
	CHECK_EQ(f.hardware_version, 0x00000419u);
	CHECK_EQ(f.firmware_version, 0x0110002Au);
	CHECK_EQ(f.update_version, 0x0630);
	CHECK(!ParseFirmwareInfo(r.data(), r.size(), true, &f)); // Bluetooth needs the CRC
	BtCrcStore(kCrcSeedFeature, r.data(), r.size());
	CHECK(ParseFirmwareInfo(r.data(), r.size(), true, &f));
	CHECK(!ParseFirmwareInfo(r.data(), 40, false, &f));
}
