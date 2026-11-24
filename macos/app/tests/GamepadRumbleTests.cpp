// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "GamepadRumble.h"
#include "TestHarness.h"

#include <vector>

using namespace plk::app;

namespace {

std::vector<int16_t> packet(int16_t left, int16_t right, size_t frames = 30)
{
	std::vector<int16_t> pcm(frames * 2);
	for(size_t i = 0; i < frames; i++)
	{
		// A square wave: the mean absolute sample is the amplitude.
		pcm[2 * i] = (int16_t)((i & 1) ? left : -left);
		pcm[2 * i + 1] = (int16_t)((i & 1) ? right : -right);
	}
	return pcm;
}

bool near(float a, float b)
{
	return a > b - 0.005f && a < b + 0.005f;
}

} // namespace

PLK_TEST(rumble_events_drive_their_own_motor)
{
	RumbleMixer m;
	CHECK(m.motors(0).low == 0 && m.motors(0).high == 0);
	m.setRumble(255, 0);
	CHECK(near(m.motors(0).low, 1.0f) && m.motors(0).high == 0);
	m.setRumble(0, 51);
	CHECK(m.motors(0).low == 0 && near(m.motors(0).high, 0.2f));
	m.setRumble(0, 0);
	CHECK(m.motors(0).low == 0 && m.motors(0).high == 0);
}

PLK_TEST(rumble_follows_the_haptics_stream_loudness)
{
	RumbleMixer m;
	// Amplitude 8192 on the louder channel: 2 * 8192 / 65535 = 0.25, once three packets are in.
	const std::vector<int16_t> pcm = packet(8192, 1000);
	m.pushHaptics(pcm.data(), 30, 1.00);
	CHECK(near(m.motors(1.00).low, 0.25f / 3));
	m.pushHaptics(pcm.data(), 30, 1.01);
	m.pushHaptics(pcm.data(), 30, 1.02);
	CHECK(near(m.motors(1.02).low, 0.25f));
	CHECK(near(m.motors(1.02).high, 0.25f)); // both motors
	// The stream stops: silence after 50 ms without a packet.
	CHECK(near(m.motors(1.06).low, 0.25f));
	CHECK(m.motors(1.08).low == 0);
	// And what it left behind does not come back with the next packet.
	m.pushHaptics(pcm.data(), 30, 2.00);
	CHECK(near(m.motors(2.00).low, 0.25f / 3));

	// Below the floor it is noise, not an effect.
	RumbleMixer quiet;
	const std::vector<int16_t> hiss = packet(40, 40);
	for(int i = 0; i < 3; i++)
		quiet.pushHaptics(hiss.data(), 30, 1.0 + i * 0.01);
	CHECK(quiet.motors(1.02).low == 0);

	// Full scale does not go past 1.
	RumbleMixer loud;
	const std::vector<int16_t> full = packet(32767, 32767);
	for(int i = 0; i < 3; i++)
		loud.pushHaptics(full.data(), 30, 1.0 + i * 0.01);
	CHECK(near(loud.motors(1.02).low, 1.0f));
}

PLK_TEST(rumble_larger_source_wins_and_intensity_scales)
{
	RumbleMixer m;
	const std::vector<int16_t> pcm = packet(8192, 8192);
	for(int i = 0; i < 3; i++)
		m.pushHaptics(pcm.data(), 30, 1.0 + i * 0.01);
	m.setRumble(204, 26); // 0.8 and 0.1
	CHECK(near(m.motors(1.02).low, 0.8f));
	CHECK(near(m.motors(1.02).high, 0.25f));
	m.setIntensity(Medium);
	CHECK(near(m.motors(1.02).low, 0.56f));
	m.setIntensity(Off);
	CHECK(m.motors(1.02).low == 0 && m.motors(1.02).high == 0);
	m.setIntensity(Strong);
	m.reset();
	CHECK(m.motors(1.02).low == 0 && m.motors(1.02).high == 0);
}
