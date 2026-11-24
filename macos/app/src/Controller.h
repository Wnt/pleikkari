// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// The DualSense side of a stream: input reports in, console feedback out.
//
// INPUT. Every report the HID thread delivers is mapped (PadMapper) and handed to the
// state sink on that same thread, which calls SessionHost::setControllerState(): no timer
// and no thread hop between the controller and the lib's feedback sender. With several
// controllers their states are merged with chiaki_controller_state_or(), as the Qt app
// merges its controllers.
//
// OUTPUT (only with Config::output). The console's events arrive on the lib's Takion
// thread and go straight to the devices' setters, which never block:
//   rumble          DualSenseHID::SetRumble (raw values; the console's intensity setting
//                   travels in the motor power byte). Dropped while the haptic intensity
//                   is Off. A Bluetooth controller playing the haptics stream drops the
//                   console's rumble while a non-silent haptics sample was pushed within
//                   250 ms, after one SetRumble(0, 0) (as the Qt app; env
//                   CHIAKI_BT_HAPTICS_KEEP_RUMBLE=1 keeps it).
//   trigger effects SetTriggerEffects with the event's mask: only the named trigger is
//                   written. Dropped while the trigger intensity is Off.
//   intensities     the motor power byte, as the Qt app encodes it: trigger reduction in
//                   the high nibble (Off 0xF0, Weak 0x90, Medium 0x60, Strong 0x00), rumble
//                   reduction in the low nibble (Off 0x0F, Weak 0x03, Medium 0x02,
//                   Strong 0x00).
//   LED colour      lightbar.  player index -> player LEDs (SDL's pattern table).
//   motion reset    re-zeroes every controller's motion on the HID thread at its next report.
//   haptics stream  Bluetooth: report 0x32 via StartHaptics/PushHapticsPcm16. USB: the
//                   DualSense's own USB audio device, channels 3/4 (UsbHaptics), opened on a
//                   utility queue while a session is up. Gain = the Qt setting haptic_override.
//                   Both are dropped while the haptic intensity is Off.
// A controller that connects also gets the mic LED on with the mic muted (the Qt app's
// initial SetDualsenseMic(true)), and the last LED colour, player index and motor power.
//
// stop() runs DualSenseHID::Manager::Stop(): haptics stop, one effects-off report to every
// controller that was written to, devices closed, threads joined.

#ifndef PLK_APP_CONTROLLER_H
#define PLK_APP_CONTROLLER_H

#include "PadMapper.h"
#include "UsbHaptics.h"

#include <DualSenseHID.h>

#include <chiaki/session.h>

#include <atomic>
#include <dispatch/dispatch.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace plk::app {

/** The Qt app's motor power byte for these intensities (high nibble triggers, low rumble). */
uint8_t encodeMotorPower(ChiakiDualSenseEffectIntensity trigger, ChiakiDualSenseEffectIntensity rumble);

class ControllerHub
{
public:
	struct Config
	{
		bool output = false;              // write effects, LEDs and haptics to the controllers
		bool realtime_threads = true;     // DualSenseHID real-time thread policy
		bool bt_haptics = true;           // haptics over Bluetooth report 0x32
		bool usb_haptics = true;          // haptics over the USB DualSense's audio device
		float haptic_gain = 1.0f;
		bool keep_rumble_with_haptics = false; // CHIAKI_BT_HAPTICS_KEEP_RUMBLE
		bool trigger_log = false;         // CHIAKI_TRIGGER_LOG: log what is applied
	};
	using StateSink = std::function<void(const ChiakiControllerState &)>;
	using LogSink = std::function<void(char level, const std::string &message)>;

	ControllerHub();
	~ControllerHub();
	ControllerHub(const ControllerHub &) = delete;
	ControllerHub &operator=(const ControllerHub &) = delete;

	bool start(const Config &config, StateSink sink, LogSink log);
	/** Not from a HID callback. Idempotent. */
	void stop();

	/** The session is up (Bluetooth haptics start) or gone (they stop). Any thread. */
	void setSessionActive(bool active);

	// Console events, any thread (normally the lib's Takion thread). Never block.
	void onRumble(uint8_t left, uint8_t right);
	void onTriggerEffects(const ChiakiTriggerEffectsEvent &effects);
	void onHapticIntensity(ChiakiDualSenseEffectIntensity intensity);
	void onTriggerIntensity(ChiakiDualSenseEffectIntensity intensity);
	void onLedColor(const uint8_t rgb[3]);
	void onPlayerIndex(uint8_t index);
	void onMotionReset();
	void onHapticsFrame(const uint8_t *buf, size_t size);

	/** One line of counters for the periodic stats log. */
	std::string statsLine();
	size_t deviceCount() const;

private:
	struct Pad;   // HID-thread side of one controller
	struct Out;   // output side of one controller (any thread, under out_mutex_)

	void onConnected(const std::shared_ptr<dualsense::Device> &device);
	void onDisconnected(const std::shared_ptr<dualsense::Device> &device);
	void onInput(dualsense::Device &device, const dualsense::InputState &in);
	void pushMergedState();
	void applyInitialEffects(Out &out);
	void startHapticsLocked(Out &out);
	/** Open or close the USB haptics output to match "session up and a USB pad with output". */
	void updateUsbHapticsLocked();

	Config config_;
	StateSink sink_;
	LogSink log_;
	std::unique_ptr<dualsense::Manager> manager_;
	bool started_ = false;

	// HID thread only.
	std::unordered_map<uint64_t, std::unique_ptr<Pad>> pads_;
	ChiakiControllerState merged_;

	std::atomic<uint32_t> motion_reset_gen_{0};

	mutable std::mutex out_mutex_;
	std::vector<std::unique_ptr<Out>> outs_;
	bool session_active_ = false;
	dispatch_queue_t usb_queue_ = nullptr;   // opens/closes the USB haptics output
	std::unique_ptr<UsbHaptics> usb_haptics_; // under out_mutex_
	bool usb_wanted_ = false;                 // what the last update asked for
	uint64_t usb_generation_ = 0;
	bool have_led_ = false;
	uint8_t led_[3] = {0, 0, 0};
	int player_index_ = -1;
	ChiakiDualSenseEffectIntensity haptic_intensity_ = Strong;
	ChiakiDualSenseEffectIntensity trigger_intensity_ = Strong;

	// Counters.
	std::atomic<uint64_t> input_reports_{0};
	std::atomic<uint64_t> rumble_events_{0}, rumble_dropped_{0}, rumble_suppressed_{0};
	std::atomic<uint64_t> trigger_events_{0}, trigger_dropped_{0};
	std::atomic<uint64_t> haptics_frames_{0};
	std::atomic<uint32_t> max_motion_step_us_{0};
	uint64_t last_stats_reports_ = 0;
	double last_stats_time_ = 0;
};

} // namespace plk::app

#endif // PLK_APP_CONTROLLER_H
