// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "FrameTelemetry.h"
#include "TestHarness.h"

#include <chiaki/time.h>

#include <cmath>
#include <map>
#include <regex>

using namespace plk;
using namespace plk::app;

namespace {

// frames.py's KV pattern.
std::map<std::string, long long> parseKv(const std::string &line)
{
	std::map<std::string, long long> kv;
	static const std::regex re("([a-z_]+)=(-?\\d+)");
	for(auto it = std::sregex_iterator(line.begin(), line.end(), re); it != std::sregex_iterator(); ++it)
		kv[(*it)[1].str()] = std::stoll((*it)[2].str());
	return kv;
}

video::FrameTiming presentedFrame()
{
	video::FrameTiming t;
	t.frameIndex = 4242;
	t.keyframe = true;
	t.presented = true;
	t.readyUs = 1000000;
	t.submitUs = 1000050;
	t.decodedUs = 1002550;
	t.renderStartUs = 1002600;
	t.committedUs = 1003100;
	t.gpuStartUs = 1003200;
	t.gpuEndUs = 1003700;
	t.presentedUs = 1010000;
	return t;
}

} // namespace

PLK_TEST(telemetry_frame_line_stages_add_up)
{
	PacketSpan span;
	span.frame_index = 4242;
	span.first_us = 996000;
	span.last_us = 999900;
	span.packets = 37;
	const std::string line = formatFrameLine(presentedFrame(), &span);
	CHECK(line.rfind("[frame] idx=4242 ", 0) == 0);
	auto kv = parseKv(line);
	CHECK(kv["idx"] == 4242);
	CHECK(kv["pkts"] == 37);
	CHECK(kv["first"] == 996000);
	CHECK(kv["ready"] == 1000000);
	CHECK(kv["swap"] == 1010000);
	CHECK(kv["assemble_us"] == 4000);
	CHECK(kv["to_decoder_us"] == 50);
	CHECK(kv["decode_us"] == 2500);
	CHECK(kv["queue_us"] == 50);
	CHECK(kv["render_us"] == 500);
	CHECK(kv["swap_us"] == 6900);
	CHECK(kv["total_us"] == 10000);
	CHECK(kv["total_first_us"] == 14000);
	CHECK(kv["to_decoder_us"] + kv["decode_us"] + kv["queue_us"] + kv["render_us"] + kv["swap_us"] == kv["total_us"]);
	CHECK(kv["gpu_wait_us"] == 100 && kv["gpu_us"] == 500 && kv["scanout_us"] == 6300);
	CHECK(kv["key"] == 1);
	CHECK(kv.count("offscreen") == 0);
}

PLK_TEST(telemetry_missing_values_are_minus_one)
{
	video::FrameTiming t = presentedFrame();
	t.decodedUs = 0;
	const std::string line = formatFrameLine(t, nullptr);
	auto kv = parseKv(line);
	CHECK(kv["pkts"] == 0);
	CHECK(kv["first"] == -1);
	CHECK(kv["assemble_us"] == -1);
	CHECK(kv["total_first_us"] == -1);
	CHECK(kv["decode_us"] == -1);
	CHECK(kv["queue_us"] == -1);
	CHECK(kv["total_us"] == 10000);
}

PLK_TEST(telemetry_clock_skew_is_not_a_missing_stage)
{
	video::FrameTiming t = presentedFrame();
	t.submitUs = t.readyUs - 3; // translated ready a hair after submit
	auto kv = parseKv(formatFrameLine(t, nullptr));
	CHECK(kv["to_decoder_us"] == 0);
	t.submitUs = t.readyUs - 5000; // really backwards
	kv = parseKv(formatFrameLine(t, nullptr));
	CHECK(kv["to_decoder_us"] == -1);
}

PLK_TEST(telemetry_offscreen_frame_uses_gpu_end)
{
	video::FrameTiming t = presentedFrame();
	t.presented = false;
	t.offscreen = true;
	t.presentedUs = 0;
	auto kv = parseKv(formatFrameLine(t, nullptr));
	CHECK(kv["swap"] == 1003700);
	CHECK(kv["offscreen"] == 1);
	CHECK(kv["scanout_us"] == -1);
	CHECK(kv["total_us"] == 3700);
}

PLK_TEST(telemetry_packet_spans_ring)
{
	PacketSpans spans;
	spans.onPacket(10, 100.0);
	spans.onPacket(10, 150.0);
	spans.onPacket(10, 180.0);
	PacketSpan s;
	REQUIRE(spans.lookup(10, &s));
	CHECK(s.first_us == 100.0 && s.last_us == 180.0 && s.packets == 3);
	// 256 frames later the slot holds another frame.
	spans.onPacket(10 + 256, 900.0);
	CHECK(!spans.lookup(10, &s));
	REQUIRE(spans.lookup(10 + 256, &s));
	CHECK(s.packets == 1);
	CHECK(!spans.lookup(11, &s));
	// 16-bit wrap.
	spans.onPacket(65535, 1.0);
	spans.onPacket(0, 2.0);
	CHECK(spans.lookup(65535, &s) && spans.lookup(0, &s));
}

PLK_TEST(telemetry_clock_translation)
{
	// The lib's CLOCK_MONOTONIC and the pipeline's mach clock differ by the time the Mac
	// slept; translated, "now" on one is "now" on the other.
	const uint64_t mono = chiaki_time_now_monotonic_us();
	const double host = video::hostTimeUs();
	const double translated = video::monotonicToHostUs(mono);
	CHECK_MSG(std::fabs(translated - host) < 2000.0, "translated %.0f host %.0f", translated, host);
}
