// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "GamepadRumble.h"

#include <algorithm>
#include <cstdlib>

namespace plk::app {

void RumbleMixer::setRumble(uint8_t left, uint8_t right)
{
	left_ = left;
	right_ = right;
}

void RumbleMixer::pushHaptics(const int16_t *stereo, size_t frames, double now_s)
{
	if(!stereo || !frames)
		return;
	uint64_t sum_l = 0, sum_r = 0;
	for(size_t i = 0; i < frames; i++)
	{
		sum_l += (uint64_t)std::abs((int)stereo[2 * i]);
		sum_r += (uint64_t)std::abs((int)stereo[2 * i + 1]);
	}
	// The Qt app's numbers: mean absolute sample times two, on a 16-bit scale.
	const uint64_t level = std::max(sum_l, sum_r) * 2 / frames;
	const float v = level > 100 ? std::min((float)level / 65535.0f, 1.0f) : 0.0f;
	// A gap means the stream had stopped: what is left of it is old.
	if(last_packet_s_ >= 0 && now_s - last_packet_s_ > kSilenceAfterS)
		std::fill(std::begin(packet_level_), std::end(packet_level_), 0.0f);
	packet_level_[next_packet_] = v;
	next_packet_ = (next_packet_ + 1) % kPackets;
	last_packet_s_ = now_s;
}

void RumbleMixer::setIntensity(ChiakiDualSenseEffectIntensity intensity)
{
	switch(intensity)
	{
		case Off: scale_ = 0.0f; break;
		case Weak: scale_ = 0.4f; break;
		case Medium: scale_ = 0.7f; break;
		case Strong: scale_ = 1.0f; break;
	}
}

void RumbleMixer::reset()
{
	left_ = right_ = 0;
	std::fill(std::begin(packet_level_), std::end(packet_level_), 0.0f);
	next_packet_ = 0;
	last_packet_s_ = -1;
}

RumbleMixer::Motors RumbleMixer::motors(double now_s) const
{
	float haptics = 0;
	if(last_packet_s_ >= 0 && now_s - last_packet_s_ <= kSilenceAfterS)
	{
		for(float v : packet_level_)
			haptics += v;
		haptics /= kPackets;
	}
	Motors m;
	m.low = std::min(std::max(left_ / 255.0f, haptics) * scale_, 1.0f);
	m.high = std::min(std::max(right_ / 255.0f, haptics) * scale_, 1.0f);
	return m;
}

} // namespace plk::app
