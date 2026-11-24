// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Controllers other than the DualSense, through macOS's GameController framework: an Xbox
// controller, a DualShock 4, a Switch Pro controller, any MFi pad. The DualSense is left to
// ControllerHub, which talks HID to it directly (adaptive triggers, haptics, 1 ms motion).
//
// Every change the framework reports is mapped (GamepadMapper) and handed to the state sink
// on the hub's own queue; with several controllers their states are merged with
// chiaki_controller_state_or(). The framework is told to deliver input while the app is in
// the background too, as the DualSense path and the Qt app do, and to leave the Home, Menu,
// View and Share buttons to the app instead of its own shortcuts.
//
// Rumble: the console's rumble events and the loudness of its haptics stream (RumbleMixer)
// drive the controller's motors through its CoreHaptics engines, the heavy motor in the
// left handle and the light one in the right. Adaptive trigger effects have no counterpart
// on these controllers and are dropped.
//
// The framework needs the main run loop: this is for the windowed app, not headless runs.

#ifndef PLK_APP_GAMEPADS_H
#define PLK_APP_GAMEPADS_H

#include <chiaki/controller.h>
#include <chiaki/session.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace plk::app {

class GamepadHub
{
public:
	struct Config
	{
		bool output = true; // false: read only, no rumble
	};

	using StateSink = std::function<void(const ChiakiControllerState &)>;
	using LogSink = std::function<void(char level, const std::string &message)>;

	GamepadHub();
	/** Main thread. */
	~GamepadHub();
	GamepadHub(const GamepadHub &) = delete;
	GamepadHub &operator=(const GamepadHub &) = delete;

	/** Main thread. sink runs on the hub's queue. */
	void start(const Config &config, StateSink sink, LogSink log);
	/** Any thread. After it returns the sink is not called again and the motors are off. Idempotent. */
	void stop();

	// From the session, any thread. Nothing reaches a motor outside a session.
	void setSessionActive(bool active);
	void onRumble(uint8_t left, uint8_t right);
	void onHapticsFrame(const uint8_t *buf, size_t size);
	void onHapticIntensity(ChiakiDualSenseEffectIntensity intensity);

	/** One line of counters for the periodic stats log. Any thread. */
	std::string statsLine() const;
	size_t deviceCount() const;

private:
	struct Impl;
	Impl *const impl_;
};

} // namespace plk::app

#endif // PLK_APP_GAMEPADS_H
