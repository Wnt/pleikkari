// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Native DualSense / DualSense Edge driver for macOS on IOKit HID: no Qt, no SDL,
// no hidapi. Replaces the SDL polling path of the Qt GUI (a 4 ms QTimer pulling
// SDL events on the GUI thread) with a report-driven one: every input report is
// parsed and handed to the client the moment IOKit delivers it.
//
// Supported: 054C:0CE6 (DualSense) and 054C:0DF2 (DualSense Edge), USB and
// Bluetooth. Input reports: USB 0x01, Bluetooth 0x31 (CRC checked) and the simple
// Bluetooth 0x01 report a controller sends before it is switched to enhanced mode.
//
// THREADING CONTRACT
//
//  * Manager owns one HID thread ("plk.ds.hid", QOS_CLASS_USER_INTERACTIVE, optionally
//    real-time, see Options::realtime_threads) with its
//    own CFRunLoop. Discovery, hotplug and every input report are handled there, and
//    every Manager::Callbacks function runs there, one at a time, in report order.
//    Callbacks must return quickly (a slow callback delays the next report), must not
//    block on another Manager call, and must not call Manager::Stop().
//  * A Device opened with output enabled owns one writer thread ("plk.ds.out",
//    user-interactive). Set*() never blocks on I/O: it stores the value under a short
//    mutex and wakes the writer, which always sends the newest complete state and
//    never holds more than that one pending state (three updates during a blocking
//    Bluetooth write become one write carrying the latest of everything).
//    Set*() may be called from any thread, including the input callback.
//  * Bluetooth haptics run on their own paced thread ("plk.ds.haptics") between
//    StartHaptics() and StopHaptics(). PushHaptics*() may be called from any thread
//    (normally the stream's haptics receive thread) and never blocks on I/O.
//  * Writes from the writer and the haptics thread to one device are serialised: at
//    most one IOHIDDeviceSetReport is in flight per device.
//  * Devices are shared_ptr. A Device stays valid after its controller disconnects;
//    its setters become no-ops and connected() turns false.
//  * Manager::Stop() (also run by the destructor) stops haptics, sends one
//    effects-off report (rumble 0, both triggers effect 0x05) to every device this
//    process has written effects to, closes all devices and joins every thread.
//    Call it from a thread that is not one of the above.
//
// OUTPUT IS OFF BY DEFAULT. Manager::Options::allow_output = false (the default)
// creates no writer, refuses every Set*()/haptics call and never calls
// IOHIDDeviceSetReport. Devices are always opened non-exclusively
// (kIOHIDOptionsTypeNone), so another process (the Qt app, SDL) keeps working.
// Opening reads feature reports 0x20 (firmware) and 0x05 (IMU calibration); over
// Bluetooth that read is also what switches a controller from the simple report to
// the enhanced 0x31 report, so no output report is needed for that (SDL and Linux
// rely on this; not observed here, the test controller was already in 0x31 mode).
// bluetoothd passes feature replies through with their CRC: 0x05 validated with the
// 0xA3 seed on a real controller.
//
// HAPTICS OVER USB are not handled here: on USB the DualSense exposes a 4-channel
// USB audio device whose channels 3 and 4 drive the actuators. That path stays with
// the audio stack (see StreamSession::ConnectHaptics in the Qt GUI). Over Bluetooth
// there is no audio device and haptics go as HID output report 0x32 (StartHaptics).

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace plk::dualsense {

enum class Model : uint8_t { DualSense, DualSenseEdge };
enum class Connection : uint8_t { USB, Bluetooth };

enum class ReportMode : uint8_t {
	Enhanced,        // USB 0x01 (64 bytes) or Bluetooth 0x31 (78 bytes): everything below
	BluetoothSimple, // Bluetooth 0x01 (10 bytes): sticks, buttons, triggers only
};

// Button bits, identical to ChiakiControllerButton (lib/include/chiaki/controller.h),
// so InputState::buttons can be copied into ChiakiControllerState::buttons as is.
enum Button : uint32_t {
	kButtonCross = 1u << 0,
	kButtonMoon = 1u << 1,     // circle
	kButtonBox = 1u << 2,      // square
	kButtonPyramid = 1u << 3,  // triangle
	kButtonDpadLeft = 1u << 4,
	kButtonDpadRight = 1u << 5,
	kButtonDpadUp = 1u << 6,
	kButtonDpadDown = 1u << 7,
	kButtonL1 = 1u << 8,
	kButtonR1 = 1u << 9,
	kButtonL3 = 1u << 10,
	kButtonR3 = 1u << 11,
	kButtonOptions = 1u << 12,
	kButtonShare = 1u << 13,   // "Create"
	kButtonTouchpad = 1u << 14, // touchpad click
	kButtonPS = 1u << 15,
};

// Buttons ChiakiControllerState has no bit for.
enum ExtraButton : uint32_t {
	kExtraMute = 1u << 0,          // the mic mute button (the Qt GUI toggles mute on release)
	kExtraL2Digital = 1u << 1,     // controller's own "L2 pressed" bit
	kExtraR2Digital = 1u << 2,
	kExtraEdgeFnLeft = 1u << 3,    // DualSense Edge only
	kExtraEdgeFnRight = 1u << 4,
	kExtraEdgePaddleLeft = 1u << 5,
	kExtraEdgePaddleRight = 1u << 6,
};

struct Touch {
	bool active = false;
	uint8_t id = 0;  // the controller's 7-bit contact counter; changes on every new contact
	uint16_t x = 0;  // 0..1919, the PS5's own touchpad range
	uint16_t y = 0;  // 0..1079
};

enum class BatteryStatus : uint8_t { Discharging, Charging, Full, Error, Unknown };

// One parsed input report. Field names follow ChiakiControllerState where one exists.
struct InputState {
	// ---- timing ----
	uint64_t hid_time = 0;   // IOKit's timestamp for the report (mach_absolute_time units),
	                         // taken when the HID system queued it for this process
	uint64_t host_time = 0;  // mach_absolute_time() when the report reached the callback
	uint8_t report_id = 0;
	ReportMode mode = ReportMode::Enhanced;
	uint32_t sequence = 0;   // enhanced payload bytes 11..14: +1 per report (SDL's
	                         // rgucPacketSequence); use this to count lost reports
	uint8_t counter = 0;     // enhanced payload byte 6 (SDL's ucCounter); constant 0x01
	                         // over Bluetooth on firmware 0x0641. Simple report: 6-bit
	                         // counter in the third button byte.
	uint8_t bt_header = 0;   // Bluetooth 0x31 byte 1: high nibble +1 per report, low 0x1
	uint32_t sensor_timestamp = 0;  // controller clock, 1/3 us per tick, wraps every ~1431 s
	uint64_t sensor_time_us = 0;    // the same, unwrapped and in microseconds since open
	                                // (enhanced reports only)
	// ---- ChiakiControllerState fields ----
	uint32_t buttons = 0;          // Button bits
	uint8_t l2_state = 0;          // 0..255
	uint8_t r2_state = 0;
	int16_t left_x = 0, left_y = 0;    // raw * 257 - 32768 (what SDL reports); +y is down
	int16_t right_x = 0, right_y = 0;
	Touch touches[2];              // enhanced reports only
	float gyro_x = 0, gyro_y = 0, gyro_z = 0;    // rad/s, calibrated (pitch, yaw, roll)
	float accel_x = 0, accel_y = 0, accel_z = 0; // g, calibrated
	bool motion_valid = false;     // false in the simple Bluetooth report
	// ---- raw values ----
	uint8_t raw_left_x = 0x80, raw_left_y = 0x80, raw_right_x = 0x80, raw_right_y = 0x80;
	int16_t raw_gyro[3] = {0, 0, 0};
	int16_t raw_accel[3] = {0, 0, 0};
	// ---- extras ----
	uint32_t extra_buttons = 0;    // ExtraButton bits
	bool battery_valid = false;
	uint8_t battery_percent = 0;
	BatteryStatus battery_status = BatteryStatus::Unknown;
	bool headphones = false;       // enhanced payload byte 53 bit 0
	bool microphone = false;       // enhanced payload byte 53 bit 1 (mic on the jack)
};

struct DeviceInfo {
	Model model = Model::DualSense;
	Connection connection = Connection::USB;
	uint16_t vendor_id = 0;
	uint16_t product_id = 0;
	uint64_t registry_id = 0;      // stable for the lifetime of the HID device
	std::string product_name;
	// Feature report 0x20
	bool firmware_valid = false;
	uint16_t update_version = 0;   // what SDL calls firmware_version (feature 0x20 bytes 44..45)
	uint32_t hardware_version = 0; // feature 0x20 bytes 24..27
	uint32_t firmware_version = 0; // feature 0x20 bytes 28..31
	std::string build_date;        // e.g. "Jun 12 2024"
	std::string build_time;
	// Feature report 0x05
	bool calibration_valid = false; // false: raw IMU values are scaled with nominal factors
	// Effects
	bool rumble_v2 = false;        // "improved rumble emulation" (effects block[38] bit 2) instead of
	                               // the halved legacy rumble (block[0] bit 0); see Protocol.h
	size_t max_input_report_size = 0;
	size_t max_output_report_size = 0;
	// Raw feature reports as read at open (0x05 and 0x20 only; 0x09 is never read).
	std::vector<uint8_t> raw_calibration;
	std::vector<uint8_t> raw_firmware_info;
};

struct InputStats {
	uint64_t reports = 0;        // parsed and delivered
	uint64_t crc_errors = 0;     // Bluetooth 0x31 reports dropped for a bad CRC
	uint64_t other_reports = 0;  // reports of an id or size the parser does not handle
};

struct WriterStats {
	uint64_t updates = 0;          // Set*() calls that changed the pending state
	uint64_t writes = 0;           // output reports written
	uint64_t write_failures = 0;
	uint64_t last_write_ns = 0;    // duration of the blocking IOHIDDeviceSetReport
	uint64_t max_write_ns = 0;
	uint64_t total_write_ns = 0;
};

// Tuning of the Bluetooth haptics sender. Defaults are those of the Qt GUI's
// DualSenseBtHaptics; FromEnvironment() reads the same CHIAKI_BT_HAPTICS_* variables.
struct HapticsConfig {
	bool double_block = true;     // 64 frames per report every 21.33 ms (false: 32 / 10.67 ms)
	uint8_t controller_buffer = 48; // controller-side buffer length field, 16..128
	int prefill_ms = 32;          // host jitter buffer before playback starts, 0..500
	int max_ms = 96;              // host queue cap before the oldest data is dropped, 0..1000
	static HapticsConfig FromEnvironment();
};

struct HapticsStats {
	uint64_t reports = 0;
	uint64_t underruns = 0;       // playback gaps: the queue ran dry
	uint64_t dropped_bytes = 0;   // backlog dropped to bound latency
	uint64_t write_failures = 0;
	bool failed = false;          // 50 consecutive write failures: sender gave up
};

enum class MicLed : uint8_t { Off = 0, On = 1, Pulse = 2 };

class Device {
public:
	struct Impl;
	explicit Device(std::unique_ptr<Impl> impl);
	~Device();
	Device(const Device &) = delete;
	Device &operator=(const Device &) = delete;

	const DeviceInfo &info() const;
	bool connected() const;
	bool output_enabled() const;
	InputStats input_stats() const;

	// ---- effects (no-ops unless output is enabled) ----
	// Each group goes into every report once it has been set, as the console does.
	// Rumble is the exception: once a zero rumble has been sent, rumble leaves the
	// report again so the controller returns to audio haptics (as the Qt GUI does).
	void SetRumble(uint8_t left, uint8_t right);
	// Raw 11-byte blocks exactly as the PS5 sends them: [0] effect type, [1..10] params.
	void SetTriggerEffects(const uint8_t left[11], const uint8_t right[11]);
	// Motor power level (effects block byte 36): the Qt GUI's "DualSense intensity",
	// which it packs as trigger reduction in the high nibble and rumble reduction in
	// the low nibble. Sent with every report that carries rumble or trigger effects,
	// as the GUI does.
	void SetMotorPowerLevel(uint8_t value);
	void SetLightbar(uint8_t r, uint8_t g, uint8_t b);
	void SetPlayerLeds(uint8_t mask); // bits 0..4 LEDs, bit 5 = switch instantly
	static uint8_t PlayerLedsForIndex(int player_index); // SDL's table; <0 = all off
	// Mic LED and the power save / mute byte (effects block byte 9), together, as the
	// GUI's Controller::SetDualsenseMic(): muted = (On, 0x08), unmuted = (Off, 0x00).
	void SetMic(MicLed led, uint8_t power_save_bits);
	void ClearEffects(); // rumble 0 and both triggers effect 0x05 (off)
	WriterStats writer_stats() const;

	// ---- Bluetooth haptics (report 0x32) ----
	bool StartHaptics(const HapticsConfig &config = HapticsConfig());
	void StopHaptics();
	bool haptics_running() const;
	// Interleaved stereo (L, R) signed 8-bit samples at 3 kHz.
	void PushHaptics(const int8_t *interleaved, size_t bytes);
	// The console's haptics stream as the lib delivers it (interleaved stereo int16 at
	// 3 kHz) converted like the Qt GUI does: s8 = clamp(int(s16 * gain) / 256).
	void PushHapticsPcm16(const int16_t *interleaved, size_t samples, float gain);
	// True if a non-silent sample was pushed within the last window_ms. The Qt GUI
	// drops the console's rumble for a Bluetooth controller while this holds for
	// 250 ms (rumble is a copy of the same effect and costs extra reports).
	bool HapticsRecentlyNonSilent(uint32_t window_ms) const;
	HapticsStats haptics_stats() const;

	Impl *impl() { return impl_.get(); }

private:
	std::unique_ptr<Impl> impl_;
};

class Manager {
public:
	struct Options {
		bool allow_output = false; // see "OUTPUT IS OFF BY DEFAULT" above
		// Put the HID, writer and haptics threads under the Mach time-constraint
		// (real-time) policy on top of user-interactive QoS. A QoS request can be
		// clamped by the process context and its timers are coalesced: measured from a
		// background shell, a user-interactive thread ran at priority 31 and its 2 ms
		// timers fired p50 0.40 / p99 3.1 / max 7.9 ms late; the same thread under the
		// time-constraint policy ran at priority 97, p50 5 / p99 27 / max 44 us
		// (plk-dualsense-probe --sched-test). Off by default so the effect can be A/B
		// tested in the client; the kernel demotes a real-time thread that overruns its
		// budget, so a slow callback cannot starve the system.
		bool realtime_threads = false;
	};
	struct Callbacks {
		// All run on the HID thread (see THREADING CONTRACT).
		std::function<void(const std::shared_ptr<Device> &)> connected;
		std::function<void(const std::shared_ptr<Device> &)> disconnected;
		std::function<void(Device &, const InputState &)> input;
		// Every input report before parsing, with IOKit's timestamp. Optional.
		std::function<void(Device &, const uint8_t *report, size_t size, uint64_t hid_time)> raw_input;
		// Diagnostics. Optional; default prints nothing.
		std::function<void(const char *message)> log;
	};

	Manager();
	~Manager();
	Manager(const Manager &) = delete;
	Manager &operator=(const Manager &) = delete;

	// Starts the HID thread and waits until it is matching devices. Controllers that
	// are already connected are reported through Callbacks::connected right after.
	bool Start(const Options &options, Callbacks callbacks);
	void Stop();
	std::vector<std::shared_ptr<Device>> devices() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

// mach_absolute_time() units to nanoseconds.
uint64_t MachToNanos(uint64_t mach_time);
uint64_t NowMach();

} // namespace plk::dualsense
