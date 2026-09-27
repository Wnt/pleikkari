// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_JNI_VIDEO_FRAME_LATENCY_H
#define CHIAKI_JNI_VIDEO_FRAME_LATENCY_H

// PLE-698: per-frame latency through the Oculus Go's native cinema, in software, on one clock
// (CLOCK_MONOTONIC). Each frame is timestamped at
//   (a) arrival: chiaki completed the frame (its last needed packet, frame_ready_time_us),
//   (b) decoded: the presenter dequeued the decoder's output buffer,
//   (c) latched: the cinema's updateTexImage put it in the eye texture,
//   (d) submitted: the cinema called vrapi_SubmitFrame2 with it, together with that submit's
//       predicted display time (vrapi_GetPredictedDisplayTime, VrApi's clock is System.nanoTime).
// The presenter records (a) and (b) for every frame it renders to the SurfaceTexture, keyed by the
// buffer timestamp the framework gives that buffer (the PTS in ns for an immediate release, the
// release time for a timed one); the cinema reads the same value back with getTimestamp() after
// the latch. A frame rendered to the surface but passed over by a later latch was replaced before
// it was ever shown.

#include <chiaki/common.h>
#include <chiaki/thread.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Frames rendered to the surface and not latched yet; 64 is about a second at 60 fps.
#define ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_PENDING 64
// Samples kept per figure per window for the percentiles (the mean and max see every frame).
#define ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_SAMPLES 256

typedef enum android_chiaki_video_frame_latency_figure_t
{
	ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_ARRIVAL_DECODED = 0,
	ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_DECODED_LATCHED,
	ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_LATCHED_SUBMITTED,
	ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_SUBMITTED_PHOTON,
	ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_TOTAL, // arrival to predicted photon
	ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_FIGURES
} AndroidChiakiVideoFrameLatencyFigure;

typedef struct android_chiaki_video_frame_latency_stat_t
{
	uint32_t count;
	int64_t p50_ns;
	int64_t p95_ns;
	int64_t max_ns;
	int64_t mean_ns;
} AndroidChiakiVideoFrameLatencyStat;

typedef struct android_chiaki_video_frame_latency_window_t
{
	int64_t window_ns;
	uint32_t decoded;   // rendered to the surface
	uint32_t latched;   // updateTexImage calls reported
	uint32_t shown;     // latched and submitted with the video visible
	uint32_t replaced;  // rendered to the surface, never latched: a later frame was latched first
	uint32_t unmatched; // latched with a buffer timestamp the presenter never rendered
	AndroidChiakiVideoFrameLatencyStat figures[ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_FIGURES];
} AndroidChiakiVideoFrameLatencyWindow;

typedef struct android_chiaki_video_frame_latency_pending_t
{
	int64_t buffer_timestamp_ns;
	int64_t arrival_ns; // 0 when the frame's input metadata was not found
	int64_t decoded_ns;
} AndroidChiakiVideoFrameLatencyPending;

typedef struct android_chiaki_video_frame_latency_t
{
	ChiakiMutex mutex;
	bool enabled;
	AndroidChiakiVideoFrameLatencyPending pending[ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_PENDING];
	uint32_t pending_head;
	uint32_t pending_size;
	int64_t window_start_ns;
	uint32_t decoded;
	uint32_t latched;
	uint32_t shown;
	uint32_t replaced;
	uint32_t unmatched;
	int64_t samples[ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_FIGURES][ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_SAMPLES];
	uint32_t sample_count[ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_FIGURES];
	int64_t sample_sum[ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_FIGURES];
	int64_t sample_max[ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_FIGURES];
} AndroidChiakiVideoFrameLatency;

ChiakiErrorCode android_chiaki_video_frame_latency_init(AndroidChiakiVideoFrameLatency *latency);
void android_chiaki_video_frame_latency_fini(AndroidChiakiVideoFrameLatency *latency);
/** Turns recording on or off; either way drops every pending frame and starts a new window. */
void android_chiaki_video_frame_latency_set_enabled(AndroidChiakiVideoFrameLatency *latency, bool enabled,
		int64_t now_ns);
/** A frame rendered to the surface. arrival_ns is 0 when unknown. A no-op while disabled. */
void android_chiaki_video_frame_latency_record_decoded(AndroidChiakiVideoFrameLatency *latency,
		int64_t buffer_timestamp_ns, int64_t arrival_ns, int64_t decoded_ns);
/**
 * A frame latched into the texture. submitted_ns and predicted_display_ns are 0 when the submit
 * that followed the latch did not show the video (a message or the menu was up).
 */
void android_chiaki_video_frame_latency_record_latched(AndroidChiakiVideoFrameLatency *latency,
		int64_t buffer_timestamp_ns, int64_t latched_ns, int64_t submitted_ns, int64_t predicted_display_ns);
/** Hands out the window since the last call (or since enabling) and starts a new one. False while disabled. */
bool android_chiaki_video_frame_latency_take_window(AndroidChiakiVideoFrameLatency *latency, int64_t now_ns,
		AndroidChiakiVideoFrameLatencyWindow *window);
/** The window as the one-line "Cinema latency:" summary scripts/dev/go-latency/go_stats.py parses. */
void android_chiaki_video_frame_latency_format(const AndroidChiakiVideoFrameLatencyWindow *window,
		char *buf, size_t buf_size);

#endif
