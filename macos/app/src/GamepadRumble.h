// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// What a controller with two rumble motors (Xbox, DualShock 4, Switch Pro) should be doing,
// from the two things a console sends:
//
//   rumble events   left = the heavy low-frequency motor, right = the light high-frequency
//                   one, raw 0..255 each.
//   haptics stream  the session asks for DualSense features, so a PS5 game sends its
//                   haptics as audio (stereo, 3 kHz, 10 ms packets) and often no rumble
//                   event at all. A rumble motor cannot play that, so its loudness drives
//                   both motors, as the Qt app does for controllers that are not a
//                   DualSense ("rumble haptics", intensity Normal): the louder channel's
//                   mean absolute sample, doubled, averaged over the last 30 ms, and
//                   nothing below 100/65535. No packet for 50 ms means silence.
//
// The larger of the two wins per motor, scaled by the console's vibration intensity setting.

#ifndef PLK_APP_GAMEPADRUMBLE_H
#define PLK_APP_GAMEPADRUMBLE_H

#include <chiaki/session.h>

#include <cstddef>
#include <cstdint>

namespace plk::app {

class RumbleMixer
{
public:
	struct Motors
	{
		float low = 0;  // heavy motor (left handle), 0..1
		float high = 0; // light motor (right handle), 0..1
	};

	void setRumble(uint8_t left, uint8_t right);
	/** One packet of the haptics stream: interleaved stereo. */
	void pushHaptics(const int16_t *stereo, size_t frames, double now_s);
	void setIntensity(ChiakiDualSenseEffectIntensity intensity);
	/** Forget everything: the session ended. */
	void reset();

	Motors motors(double now_s) const;

private:
	static constexpr int kPackets = 3;
	static constexpr double kSilenceAfterS = 0.05;

	uint8_t left_ = 0, right_ = 0;
	float packet_level_[kPackets] = {0, 0, 0};
	int next_packet_ = 0;
	double last_packet_s_ = -1;
	float scale_ = 1.0f;
};

} // namespace plk::app

#endif // PLK_APP_GAMEPADRUMBLE_H
