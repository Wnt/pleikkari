// Unit tests for the app's HDR tone mapping parameters (no GPU).
#include "ToneMap.h"
#include "plk/VideoPipeline.h"

#include <cmath>
#include <cstdio>

using namespace plk::video;

static int failures = 0;
#define CHECK(c) do { if(!(c)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); failures++; } } while(0)

static bool near(float a, float b, float tol = 1e-4f) { return std::fabs(a - b) <= tol; }

int main()
{
	// Reference white lands on SDR white; the brightness is a factor on that.
	AppToneMap t = appToneMap(1.0f, 1000.0f, 0.0f);
	CHECK(near(203.0f * t.nitsToEdr, 1.0f));
	CHECK(t.headroom == 0);
	CHECK(near(203.0f * appToneMap(0.5f, 1000.0f, 0.0f).nitsToEdr, 0.5f));

	// The peak fits under the headroom (1000 nits is 4.93): nothing to roll off.
	t = appToneMap(1.0f, 1000.0f, 6.7f);
	CHECK(t.headroom == 0);
	CHECK(rollOff(4.9f, t.knee, t.headroom) == 4.9f);
	// A bright display, little headroom: knee at 70 % of it.
	t = appToneMap(1.0f, 1000.0f, 2.0f);
	CHECK(near(t.headroom, 2.0f) && near(t.knee, 1.4f));
	// Brighter picture, same display: the peak is higher, the roll-off is needed sooner.
	CHECK(appToneMap(2.0f, 1000.0f, 6.7f).headroom > 0);
	// A darker picture fits again.
	CHECK(appToneMap(0.4f, 1000.0f, 2.0f).headroom == 0);
	// A stream that says it was mastered at 4000 nits.
	CHECK(appToneMap(1.0f, 4000.0f, 6.7f).headroom > 0);

	// The curve: identity up to the knee, slope 1 there, monotonic, below the headroom.
	t = appToneMap(1.0f, 1000.0f, 2.0f);
	CHECK(rollOff(0.5f, t.knee, t.headroom) == 0.5f);
	CHECK(rollOff(1.4f, t.knee, t.headroom) == 1.4f);
	CHECK(near(rollOff(1.401f, t.knee, t.headroom), 1.401f, 1e-5f));
	float last = 0;
	for(float x = 0; x < 20.0f; x += 0.01f)
	{
		const float y = rollOff(x, t.knee, t.headroom);
		CHECK(y >= last);
		CHECK(y <= 2.0f);
		CHECK(y <= x + 1e-6f);
		last = y;
	}
	CHECK(near(rollOff(4.926f, t.knee, t.headroom), 1.4f + 0.6f * (1.0f - std::exp(-(4.926f - 1.4f) / 0.6f))));

	// No step where the roll-off switches off: just below the peak's fit the curve is
	// within a hair of the identity everywhere.
	const float peak = 1000.0f / 203.0f;
	for(float h : { peak * 0.999f, peak * 0.99f, peak * 0.95f })
	{
		t = appToneMap(1.0f, 1000.0f, h);
		CHECK(t.headroom > 0);
		float worst = 0;
		for(float x = 0; x <= peak; x += 0.001f)
			worst = std::fmax(worst, x - rollOff(x, t.knee, t.headroom));
		CHECK(worst <= 1.2f * (peak - h) + 1e-4f);
	}

	// What the display can be asked for: its current headroom, but never less than 2 (or
	// its potential) while the system has not yet made room, and 1 on an SDR display.
	CHECK(usableDisplayHeadroom(6.7f, 16.0f) == 6.7f);
	CHECK(usableDisplayHeadroom(1.0f, 16.0f) == 2.0f);
	CHECK(usableDisplayHeadroom(1.0f, 1.5f) == 1.5f);
	CHECK(usableDisplayHeadroom(1.0f, 1.0f) == 1.0f);
	CHECK(usableDisplayHeadroom(1.0f, 0.0f) == 1.0f);
	CHECK(usableDisplayHeadroom(3.2f, 16.0f) == 3.2f);

	if(failures)
	{
		fprintf(stderr, "%d failed\n", failures);
		return 1;
	}
	printf("tone map tests passed\n");
	return 0;
}
