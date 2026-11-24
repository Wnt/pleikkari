// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "FrameTelemetry.h"

#include "Log.h"

#include <chiaki/trace.h>

#include <cinttypes>
#include <cmath>
#include <cstdio>

namespace plk::app {

void PacketSpans::onPacket(uint16_t frame_index, double now_us)
{
	std::lock_guard<std::mutex> lk(mutex_);
	PacketSpan &s = spans_[frame_index & 0xff];
	if(s.frame_index != frame_index)
	{
		s.frame_index = frame_index;
		s.first_us = now_us;
		s.packets = 0;
	}
	s.last_us = now_us;
	s.packets++;
}

bool PacketSpans::lookup(uint16_t frame_index, PacketSpan *out) const
{
	std::lock_guard<std::mutex> lk(mutex_);
	const PacketSpan &s = spans_[frame_index & 0xff];
	if(s.frame_index != frame_index)
		return false;
	*out = s;
	return true;
}

namespace {

int64_t abs_us(double t)
{
	return t > 0 ? (int64_t)std::llround(t) : -1;
}

// b - a in microseconds, -1 if either was not reached or time ran backwards. A few
// microseconds backwards is clock sampling, not an error: frame_ready_us is translated
// from CLOCK_MONOTONIC by sampling both clocks, so a submit right after the lib stamped the
// frame can land a hair before it.
int64_t delta(double a, double b)
{
	if(a <= 0 || b <= 0)
		return -1;
	if(b < a)
		return a - b < 100.0 ? 0 : -1;
	return (int64_t)std::llround(b - a);
}

void traceCallback(ChiakiTraceEvent event, uint64_t a, uint64_t b, void *user)
{
	(void)b;
	if(event != CHIAKI_TRACE_EVENT_VIDEO_PACKET)
		return;
	static_cast<PacketSpans *>(user)->onPacket((uint16_t)a, video::hostTimeUs());
}

} // namespace

std::string formatFrameLine(const video::FrameTiming &t, const PacketSpan *span)
{
	const double swap = t.presented ? t.presentedUs : (t.offscreen ? t.gpuEndUs : 0);
	const double first = span ? span->first_us : 0;
	char buf[512];
	snprintf(buf, sizeof(buf),
			"[frame] idx=%u pkts=%u first=%" PRId64 " ready=%" PRId64 " swap=%" PRId64
			" assemble_us=%" PRId64 " to_decoder_us=%" PRId64 " decode_us=%" PRId64 " queue_us=%" PRId64
			" render_us=%" PRId64 " swap_us=%" PRId64 " total_us=%" PRId64 " total_first_us=%" PRId64
			" gpu_wait_us=%" PRId64 " gpu_us=%" PRId64 " scanout_us=%" PRId64 " key=%d%s",
			(unsigned)t.frameIndex, span ? span->packets : 0u, abs_us(first), abs_us(t.readyUs), abs_us(swap),
			delta(first, t.readyUs), delta(t.readyUs, t.submitUs), delta(t.submitUs, t.decodedUs),
			delta(t.decodedUs, t.renderStartUs), delta(t.renderStartUs, t.committedUs), delta(t.committedUs, swap),
			delta(t.readyUs, swap), delta(first, swap),
			delta(t.committedUs, t.gpuStartUs), delta(t.gpuStartUs, t.gpuEndUs),
			t.presented ? delta(t.gpuEndUs, t.presentedUs) : (int64_t)-1,
			t.keyframe ? 1 : 0, t.offscreen ? " offscreen=1" : "");
	return buf;
}

void FrameLogger::onFrame(const video::FrameTiming &t)
{
	if(!t.presented && !t.offscreen)
	{
		// The compositor never showed it (superseded by a newer drawable): no line, as
		// frames.py treats every line as a frame on glass.
		not_presented_.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	if(!enabled_)
		return;
	PacketSpan span;
	const bool have_span = spans_.lookup(t.frameIndex, &span);
	Log::instance().write('I', formatFrameLine(t, have_span ? &span : nullptr).c_str());
	logged_.fetch_add(1, std::memory_order_relaxed);
}

void installPacketTrace(PacketSpans *spans)
{
	if(spans)
		chiaki_trace_set_event_cb(traceCallback, spans);
	else
		chiaki_trace_set_event_cb(nullptr, nullptr);
}

} // namespace plk::app
