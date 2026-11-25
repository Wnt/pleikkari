// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "ToneMap.h"

#include <algorithm>
#include <cmath>

namespace plk::video {

AppToneMap appToneMap(float brightness, float peakNits, float displayHeadroom)
{
	AppToneMap t;
	t.nitsToEdr = brightness / kHdrReferenceWhiteNits;
	const float peak = peakNits * t.nitsToEdr;
	if(displayHeadroom > 0 && peak > displayHeadroom)
	{
		t.headroom = displayHeadroom;
		t.knee = std::max(0.7f * displayHeadroom, 2.0f * displayHeadroom - peak);
	}
	return t;
}

float rollOff(float x, float knee, float headroom)
{
	if(headroom <= 0 || x <= knee)
		return x;
	const float d = headroom - knee;
	return knee + d * (1.0f - std::exp(-(x - knee) / d));
}

} // namespace plk::video
