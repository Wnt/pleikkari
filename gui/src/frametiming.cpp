// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "frametiming.h"
#include "qmlmainwindow.h" // chiakiGui logging category

#include <chiaki/time.h>
#include <chiaki/trace.h>

#include <QLoggingCategory>

#include <array>
#include <mutex>

namespace FrameTiming
{

namespace
{

constexpr int64_t kNoPts = INT64_MIN;

// What the Takion thread saw of one frame's packets, written from the lib's packet trace point
// and read by the frame thread when the decoded frame turns up.
struct PacketSpan
{
	uint32_t frame_index = UINT32_MAX;
	uint64_t first_us = 0;
	uint64_t last_us = 0;
	uint32_t packets = 0;
};

struct Record
{
	int64_t pts = kNoPts;
	uint16_t frame_index = 0;
	uint32_t packets = 0;
	uint64_t first_packet_us = 0;
	uint64_t last_packet_us = 0;
	uint64_t ready_us = 0;
	uint64_t sample_us = 0;
	uint64_t decoded_us = 0;
	uint64_t delivered_us = 0;
	uint64_t prepared_us = 0;
	uint64_t present_entry_us = 0;
	uint64_t render_entry_us = 0;
	uint64_t render_ready_us = 0;
	uint64_t swap_begin_us = 0;
	bool logged = false;
};

std::mutex packet_spans_mutex;
std::array<PacketSpan, 256> packet_spans;

std::mutex records_mutex;
std::array<Record, 64> records;
size_t records_next = 0;

Record *FindLocked(int64_t pts)
{
	for(auto &r : records)
	{
		if(r.pts == pts)
			return &r;
	}
	return nullptr;
}

// Stage b minus stage a in microseconds, or -1 when either was never marked.
qint64 Delta(uint64_t a, uint64_t b)
{
	if(!a || !b || b < a)
		return -1;
	return static_cast<qint64>(b - a);
}

void TraceEvent(ChiakiTraceEvent event, uint64_t a, uint64_t b, void *user)
{
	(void)b;
	(void)user;
	if(event != CHIAKI_TRACE_EVENT_VIDEO_PACKET)
		return;
	const uint64_t now_us = chiaki_time_now_monotonic_us();
	std::lock_guard<std::mutex> lock(packet_spans_mutex);
	PacketSpan &span = packet_spans[a & 0xff];
	if(span.frame_index != static_cast<uint32_t>(a))
	{
		span.frame_index = static_cast<uint32_t>(a);
		span.first_us = now_us;
		span.packets = 0;
	}
	span.last_us = now_us;
	span.packets++;
}

bool EnvEnabled()
{
	static const bool enabled = qEnvironmentVariableIntValue("CHIAKI_VERBOSE_LOG_NOISE") == 1;
	return enabled;
}

} // namespace

bool Enabled()
{
	return EnvEnabled() && chiakiGui().isDebugEnabled();
}

void Install()
{
	// The lib takes the callback once, before any session; packets cost a clock read each
	// while it is set, so it is only set for a run that asked for the telemetry.
	static bool installed = false;
	if(installed || !EnvEnabled())
		return;
	installed = true;
	chiaki_trace_set_event_cb(TraceEvent, nullptr);
}

void Begin(int64_t pts, uint16_t frame_index, uint64_t frame_ready_us, uint64_t decode_begin_us, uint64_t decode_end_us)
{
	if(!Enabled())
		return;
	const uint64_t now_us = chiaki_time_now_monotonic_us();
	PacketSpan span;
	{
		std::lock_guard<std::mutex> lock(packet_spans_mutex);
		span = packet_spans[frame_index & 0xff];
	}
	const bool span_valid = decode_end_us && span.frame_index == frame_index;

	std::lock_guard<std::mutex> lock(records_mutex);
	Record &r = records[records_next];
	records_next = (records_next + 1) % records.size();
	r = Record();
	r.pts = pts;
	r.frame_index = frame_index;
	r.packets = span_valid ? span.packets : 0;
	r.first_packet_us = span_valid ? span.first_us : 0;
	r.last_packet_us = span_valid ? span.last_us : 0;
	r.ready_us = frame_ready_us;
	r.sample_us = decode_begin_us;
	r.decoded_us = decode_end_us;
	r.delivered_us = now_us;
}

void Mark(int64_t pts, Stage stage, uint64_t t_us)
{
	if(!Enabled())
		return;
	if(!t_us)
		t_us = chiaki_time_now_monotonic_us();
	std::lock_guard<std::mutex> lock(records_mutex);
	Record *r = FindLocked(pts);
	if(!r)
		return;
	uint64_t *slot = nullptr;
	switch(stage)
	{
		case Stage::Prepared:     slot = &r->prepared_us; break;
		case Stage::PresentEntry: slot = &r->present_entry_us; break;
		case Stage::RenderEntry:  slot = &r->render_entry_us; break;
		case Stage::RenderReady:  slot = &r->render_ready_us; break;
		case Stage::SwapBegin:    slot = &r->swap_begin_us; break;
	}
	if(slot && !*slot)
		*slot = t_us;
}

void SwapDone(int64_t pts, uint64_t t_us)
{
	if(!Enabled())
		return;
	if(!t_us)
		t_us = chiaki_time_now_monotonic_us();
	Record r;
	{
		std::lock_guard<std::mutex> lock(records_mutex);
		Record *found = FindLocked(pts);
		if(!found || found->logged)
			return;
		found->logged = true;
		r = *found;
	}
	// Durations are consecutive stages, so they add up to total_us (ready to swap return).
	// first/ready/swap are absolute monotonic times: the gaps between frames at each end of
	// the path come from those.
	qCDebug(chiakiGui).nospace()
		<< "[frame] idx=" << r.frame_index
		<< " pkts=" << r.packets
		<< " first=" << r.first_packet_us
		<< " ready=" << r.ready_us
		<< " swap=" << t_us
		<< " assemble_us=" << Delta(r.first_packet_us, r.ready_us)
		<< " to_decoder_us=" << Delta(r.ready_us, r.sample_us)
		<< " decode_us=" << Delta(r.sample_us, r.decoded_us)
		<< " deliver_us=" << Delta(r.decoded_us, r.delivered_us)
		<< " prepare_us=" << Delta(r.delivered_us, r.prepared_us)
		<< " handoff_us=" << Delta(r.prepared_us, r.present_entry_us)
		<< " queue_us=" << Delta(r.present_entry_us, r.render_entry_us)
		<< " render_us=" << Delta(r.render_entry_us, r.render_ready_us)
		<< " hold_us=" << Delta(r.render_ready_us, r.swap_begin_us)
		<< " swap_us=" << Delta(r.swap_begin_us, t_us)
		<< " total_us=" << Delta(r.ready_us, t_us)
		<< " total_first_us=" << Delta(r.first_packet_us, t_us);
}

} // namespace FrameTiming
