// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "FrameHud.h"
#include "TestHarness.h"

#include <cstring>

using namespace plk;
using namespace plk::app;

namespace {

PaceSnapshot steady(double fps = 60.0)
{
	PaceSnapshot p;
	p.fps = fps;
	p.frame_ms = 1000.0 / fps;
	p.budget_ms = 1000.0 / 60.0;
	p.intervals_ms.assign(120, (float)p.frame_ms);
	return p;
}

const uint8_t *pixel(const HudImage &image, int x, int y)
{
	return image.bgra.data() + (size_t)y * image.bytes_per_row + (size_t)x * 4;
}

} // namespace

PLK_TEST(hud_image_shape)
{
	for(int scale = 1; scale <= 2; scale++)
	{
		const HudImage image = renderFrameHud(steady(), scale);
		CHECK(image.width == kHudWidth * scale);
		CHECK(image.height == kHudHeight * scale);
		CHECK(image.bytes_per_row >= (size_t)image.width * 4);
		REQUIRE(image.bgra.size() == image.bytes_per_row * (size_t)image.height);
		// Premultiplied: no channel above its alpha. The plate is see-through, never clear
		// in its middle and never there in its rounded corners.
		bool premultiplied = true;
		for(int y = 0; y < image.height; y++)
			for(int x = 0; x < image.width; x++)
			{
				const uint8_t *p = pixel(image, x, y);
				premultiplied = premultiplied && p[0] <= p[3] && p[1] <= p[3] && p[2] <= p[3];
			}
		CHECK(premultiplied);
		CHECK(pixel(image, 0, 0)[3] == 0);
		CHECK(pixel(image, image.width - 1, image.height - 1)[3] == 0);
		const uint8_t middle = pixel(image, image.width / 2, 4 * scale)[3];
		CHECK(middle > 120 && middle < 255);
	}
	// A scale out of range is brought into it.
	CHECK(renderFrameHud(steady(), 0).width == kHudWidth);
	CHECK(renderFrameHud(steady(), 9).width == kHudWidth * 4);
}

PLK_TEST(hud_draws_what_it_is_given)
{
	// The number is light text, the graph a green line: both are there, and a stall in the
	// intervals changes the picture.
	const HudImage image = renderFrameHud(steady(), 2);
	int light = 0, green = 0;
	for(int y = 0; y < image.height; y++)
		for(int x = 0; x < image.width; x++)
		{
			const uint8_t *p = pixel(image, x, y);
			if(p[0] > 200 && p[1] > 200 && p[2] > 200)
				light++;
			if(p[1] > 150 && p[2] < 110 && p[0] < 160)
				green++;
		}
	CHECK(light > 100);
	CHECK(green > 200);
	PaceSnapshot stalled = steady();
	stalled.intervals_ms[60] = 50.0f;
	const HudImage other = renderFrameHud(stalled, 2);
	REQUIRE(other.bgra.size() == image.bgra.size());
	CHECK(memcmp(other.bgra.data(), image.bgra.data(), image.bgra.size()) != 0);
	// Before the first frame there is still a plate, with dashes.
	const HudImage empty = renderFrameHud(PaceSnapshot(), 1);
	CHECK(empty.width == kHudWidth && !empty.bgra.empty());
	CHECK(pixel(empty, empty.width / 2, 4)[3] > 120);
}

PLK_TEST(hud_pace_health)
{
	CHECK(paceHealth(steady(60)) == 0);
	CHECK(paceHealth(steady(57.5)) == 0);
	CHECK(paceHealth(steady(52)) == 1);
	CHECK(paceHealth(steady(40)) == 2);
	// Nothing known yet is not a fault.
	CHECK(paceHealth(PaceSnapshot()) == 0);
}
