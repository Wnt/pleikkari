// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// One DualSense input report -> ChiakiControllerState, as the Qt app's Controller does it
// with SDL events (gui/src/controllermanager.cpp), with one deliberate difference: motion
// is integrated on the controller's own sensor clock.
//
//   buttons    copied: DualSenseHID's Button bits are ChiakiControllerButton's. The mic
//              button, the Edge Fn buttons and paddles have no bit and are not sent (the
//              Qt app toggles its microphone with the mic button; this app has none).
//   sticks     the int16 values SDL reports (raw * 257 - 32768), +y down, as the Qt app.
//   triggers   0..255 analog. SDL's trigger axis >> 7, which the Qt app sends, is the
//              raw value again.
//   touchpad   each contact the controller reports (its 7-bit contact id changes on every
//              new touch) starts a touch with chiaki_controller_state_start_touch(), moves
//              it while it lasts and stops it when it lifts. Coordinates are the raw
//              0..1919 x 0..1079, the PS5's own range (the Qt app scales SDL's normalised
//              values back to 1920 x 1079).
//   motion     gyro (rad/s) and accel (g) go through the lib's orientation tracker, one
//              update per report with both sensors. Its time step is the difference of
//              the reports' sensor timestamps (1/3 us ticks from the controller, unwrapped
//              by the parser), not of host arrival times: Bluetooth delivers reports in
//              bursts, and the Qt app's SDL event timestamps (host time, ms resolution,
//              two events per report) make the integration jitter with the radio.
//   motion reset  CHIAKI_EVENT_MOTION_RESET re-zeroes the accelerometer at its current
//              reading and restarts the tracker, as Controller::resetMotionControls().
//
// Not thread-safe: one PadMapper per controller, used on the HID thread only.

#ifndef PLK_APP_PADMAPPER_H
#define PLK_APP_PADMAPPER_H

#include <DualSenseHID.h>

#include <chiaki/controller.h>
#include <chiaki/orientation.h>

#include <cstdint>

namespace plk::app {

class PadMapper
{
public:
	PadMapper();

	/** Apply one parsed report. */
	void apply(const dualsense::InputState &in);
	/** CHIAKI_EVENT_MOTION_RESET. */
	void resetMotion();
	/** All buttons up, sticks centred, touches lifted (controller gone). */
	void reset();

	const ChiakiControllerState &state() const { return state_; }
	/** Sensor-clock step of the last motion update, microseconds (0 before the second report). */
	uint32_t lastMotionStepUs() const { return last_step_us_; }

private:
	struct TouchSlot
	{
		bool down = false;
		uint8_t contact = 0;  // the controller's contact id
		int8_t chiaki_id = -1;
	};

	ChiakiControllerState state_;
	ChiakiOrientationTracker tracker_;
	ChiakiAccelNewZero accel_zero_;  // subtracted from accel (set by a motion reset)
	ChiakiAccelNewZero real_accel_;  // the last raw accel reading
	TouchSlot touch_[2];
	bool have_motion_ = false;
	uint32_t last_motion_us_ = 0;
	uint32_t last_step_us_ = 0;
};

} // namespace plk::app

#endif // PLK_APP_PADMAPPER_H
