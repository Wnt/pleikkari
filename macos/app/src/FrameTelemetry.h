// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Per-frame latency telemetry: one "[frame]" line per presented frame, in the format of
// the Qt app's FrameTiming (gui/src/frametiming.cpp) that build/mac/profile/frames.py
// reads.
//
//   [frame] idx=<frame index> pkts=<video packets> first=<first packet> ready=<frame complete>
//           swap=<presented> assemble_us= to_decoder_us= decode_us= queue_us= render_us=
//           swap_us= total_us= total_first_us= gpu_wait_us= gpu_us= scanout_us= key=
//
// Clock: every absolute time (first, ready, swap) is mach_absolute_time in microseconds,
// the base of MTLDrawable.presentedTime. The lib stamps frame_ready_us with
// CLOCK_MONOTONIC, which on macOS includes sleep; the video pipeline translates it at
// submit (plk::video::monotonicToHostUs), and the packet times below are taken on the
// mach clock directly, so all of them can be subtracted from each other.
//
// Stages are consecutive, so they add up to total_us (frame complete -> on glass):
//   assemble_us     first video packet -> frame complete (the lib reassembled it)
//   to_decoder_us   frame complete -> VideoPipeline::submitFrame()
//   decode_us       submitFrame() -> VideoToolbox output
//   queue_us        decoded -> render thread took it
//   render_us       render thread took it -> command buffer committed (CPU encode)
//   swap_us         committed -> presentedTime (GPU, compositor, scanout)
// and outside the sum:
//   gpu_wait_us     committed -> GPU start
//   gpu_us          GPU start -> GPU end
//   scanout_us      GPU end -> presentedTime
// A stage that was not measured is -1, as frames.py expects.
//
// An offscreen (headless) frame is never presented: its line has offscreen=1 and swap is
// the GPU end time, so the same scripts can read a headless replay.

#ifndef PLK_APP_FRAMETELEMETRY_H
#define PLK_APP_FRAMETELEMETRY_H

#include <plk/VideoPipeline.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace plk::app {

struct PacketSpan
{
	uint32_t frame_index = UINT32_MAX;
	double first_us = 0;   // mach microseconds
	double last_us = 0;
	uint32_t packets = 0;
};

/** What the lib's receive thread saw of each frame's packets (CHIAKI_TRACE_EVENT_VIDEO_PACKET). */
class PacketSpans
{
public:
	/** Receive thread: one video packet of `frame_index` arrived at `now_us`. */
	void onPacket(uint16_t frame_index, double now_us);
	/** Any thread: the span of `frame_index`, if it is still in the ring. */
	bool lookup(uint16_t frame_index, PacketSpan *out) const;

private:
	mutable std::mutex mutex_;
	std::array<PacketSpan, 256> spans_;
};

/**
 * Writes one "[frame]" line per presented (or, headless, rendered) frame to the app log.
 * onFrame() is the pipeline's FrameCallback: it runs on a Metal callback thread and only
 * formats a line and queues it for the log writer.
 */
class FrameLogger
{
public:
	explicit FrameLogger(bool enabled) : enabled_(enabled) {}
	PacketSpans &spans() { return spans_; }
	void onFrame(const video::FrameTiming &t);
	uint64_t logged() const { return logged_.load(); }
	uint64_t notPresented() const { return not_presented_.load(); }

private:
	const bool enabled_;
	PacketSpans spans_;
	std::atomic<uint64_t> logged_{0};
	std::atomic<uint64_t> not_presented_{0};
};

/** The "[frame] ..." line (without date stamp) for one frame; span may be null. */
std::string formatFrameLine(const video::FrameTiming &t, const PacketSpan *span);

/**
 * Installs the lib's trace callback that feeds `spans` (once per process, before the first
 * session; nullptr removes it). Every video packet then costs one clock read and a short lock.
 */
void installPacketTrace(PacketSpans *spans);

} // namespace plk::app

#endif // PLK_APP_FRAMETELEMETRY_H
