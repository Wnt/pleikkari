// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_GUI_FRAMETIMING_H
#define CHIAKI_GUI_FRAMETIMING_H

#include <cstdint>

/**
 * Per-frame latency trace of the stream path, from the first video packet of a frame to the
 * return of the swap that presents it.
 *
 * It is diagnostic only and off unless CHIAKI_VERBOSE_LOG_NOISE=1 is set and the chiaki.gui
 * debug category is enabled, the same switch as the renderer's other "[latency]" telemetry.
 * Enabled, it logs one "[frame]" line per presented frame; scripts can rebuild every stage
 * time from it. Disabled, every call returns after one branch.
 *
 * A frame is identified by the pts the decoder gives its packet, which FFmpeg copies to the
 * decoded AVFrame and av_frame_copy_props() carries over a hardware-to-software transfer.
 */
namespace FrameTiming
{

enum class Stage
{
	Prepared,     // frame thread finished preparing it (hardware-to-software transfer, if any)
	PresentEntry, // GUI thread entered presentFrame()
	RenderEntry,  // render thread entered the render() cycle that first picked the frame
	RenderReady,  // rendered, before the pacing wait and the submit
	SwapBegin,    // about to swap
};

bool Enabled();

/** Call once before a session starts. Hooks the lib's trace points when Enabled(). */
void Install();

/**
 * Frame thread, right after pulling a decoded frame: starts the frame's record from what the
 * decoder reports about its sample (ChiakiFfmpegFrame) and marks it delivered.
 */
void Begin(int64_t pts, uint16_t frame_index, uint64_t frame_ready_us, uint64_t decode_begin_us, uint64_t decode_end_us);

/** Any thread. Only the first mark of a stage counts; t_us = 0 means now. */
void Mark(int64_t pts, Stage stage, uint64_t t_us = 0);

/** Render thread, after the swap returned. Logs the frame's line once. */
void SwapDone(int64_t pts, uint64_t t_us = 0);

} // namespace FrameTiming

#endif // CHIAKI_GUI_FRAMETIMING_H
