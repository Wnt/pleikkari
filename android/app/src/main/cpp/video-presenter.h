// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_JNI_VIDEO_PRESENTER_H
#define CHIAKI_JNI_VIDEO_PRESENTER_H

#include <android/choreographer.h>
#include <android/looper.h>
#include <media/NdkMediaCodec.h>

#include "video-presenter-recovery.h"

#include <chiaki/log.h>
#include <chiaki/seqnum.h>
#include <chiaki/thread.h>

#include <stdbool.h>
#include <stdint.h>

#include "video-presenter-cadence.h"
#include "video-frame-latency.h"

#define ANDROID_CHIAKI_VIDEO_PRESENTER_QUEUE_CAPACITY 5
#define ANDROID_CHIAKI_VIDEO_PRESENTER_JITTER_WINDOW 300
#define ANDROID_CHIAKI_VIDEO_INPUT_METADATA_CAPACITY 256
#define ANDROID_CHIAKI_VIDEO_DIAGNOSTICS_CAPACITY 256
// PLE-205's clean stage-0 repeat observed a minimum D_target of 10.8-11.0 ms;
// round that calibrated floor up so ordinary clean-LAN variance rests below it.
#define ANDROID_CHIAKI_VIDEO_DEJITTER_DEPTH_FLOOR_DEFAULT_MS 12
#define ANDROID_CHIAKI_VIDEO_DEJITTER_DEPTH_CAP_DEFAULT_MS 32
#define ANDROID_CHIAKI_VIDEO_DEJITTER_QUEUE_AGE_DEFAULT_FRAMES 2
#define ANDROID_CHIAKI_VIDEO_DEJITTER_DEPTH_MAX_MS 256

typedef enum android_chiaki_video_pacing_mode_t
{
	ANDROID_CHIAKI_VIDEO_PACING_DISABLED = 0,
	ANDROID_CHIAKI_VIDEO_PACING_LOWEST_LATENCY = 1,
	ANDROID_CHIAKI_VIDEO_PACING_BALANCED = 2,
	ANDROID_CHIAKI_VIDEO_PACING_SMOOTHEST = 3,
} AndroidChiakiVideoPacingMode;

typedef enum android_chiaki_video_presenter_lead_t
{
	ANDROID_CHIAKI_VIDEO_PRESENTER_LEAD_2MS = 0,
	ANDROID_CHIAKI_VIDEO_PRESENTER_LEAD_HALF_VSYNC = 1,
} AndroidChiakiVideoPresenterLead;

typedef struct android_chiaki_video_presenter_config_t
{
	bool pacing_enabled;
	bool pacing_high_refresh_enabled;
	AndroidChiakiVideoPacingMode pacing_mode;
	AndroidChiakiVideoPresenterLead presenter_lead;
	bool bounded_age_enabled;
	uint32_t max_frame_age_periods;
	bool nonblocking_producer;
	AndroidChiakiVideoRecoveryStrategy recovery_strategy;
	bool dejitter_enabled;
	uint32_t dejitter_floor_ms;
	uint32_t dejitter_cap_ms;
	uint32_t dejitter_queue_age_frames;
	bool dejitter_half_rate_enabled;
} AndroidChiakiVideoPresenterConfig;

#define ANDROID_CHIAKI_VIDEO_PRESENTER_CONFIG_DEFAULT { \
	.pacing_enabled = false, \
	.pacing_high_refresh_enabled = false, \
	.pacing_mode = ANDROID_CHIAKI_VIDEO_PACING_BALANCED, \
	.presenter_lead = ANDROID_CHIAKI_VIDEO_PRESENTER_LEAD_2MS, \
	.bounded_age_enabled = false, \
	.max_frame_age_periods = 2, \
	.nonblocking_producer = false, \
	.recovery_strategy = ANDROID_CHIAKI_VIDEO_RECOVERY_TIMELINE_SHIFT, \
	.dejitter_enabled = false, \
	.dejitter_floor_ms = ANDROID_CHIAKI_VIDEO_DEJITTER_DEPTH_FLOOR_DEFAULT_MS, \
	.dejitter_cap_ms = ANDROID_CHIAKI_VIDEO_DEJITTER_DEPTH_CAP_DEFAULT_MS, \
	.dejitter_queue_age_frames = ANDROID_CHIAKI_VIDEO_DEJITTER_QUEUE_AGE_DEFAULT_FRAMES, \
	.dejitter_half_rate_enabled = false, \
}

typedef struct android_chiaki_video_presenter_stats_t
{
	uint64_t missed_vsyncs;
	uint64_t dropped_frames;
	uint64_t dejitter_buffer_ns;
	uint32_t queue_depth;
} AndroidChiakiVideoPresenterStats;

typedef struct android_chiaki_video_presenter_diagnostics_t
{
	uint64_t output_frames;
	uint64_t decode_mean_us;
	uint64_t decode_p95_us;
	uint64_t missed_vsyncs;
	uint64_t dropped_frames;
	uint64_t bounded_age_dropped_frames;
	uint64_t recovery_flushes;
	uint64_t recovery_flushed_frames;
	uint64_t dejitter_buffer_ns;
	uint64_t cadence_depth_ns;
	uint64_t cadence_target_ns;
	uint64_t cadence_err_p50_ns;
	uint64_t cadence_err_p99_ns;
	uint64_t decode_ewma_ns;
	uint64_t cadence_window_dropped_frames;
	bool cadence_half_rate_detected;
	uint64_t vsync_period_ns;
	uint32_t queue_depth;
} AndroidChiakiVideoPresenterDiagnostics;

typedef struct android_chiaki_video_input_metadata_t
{
	int64_t presentation_time_us;
	int64_t queued_ns;
	ChiakiSeqNum16 frame_index;
	uint64_t frame_ready_time_us;
	bool valid;
} AndroidChiakiVideoInputMetadata;

typedef void (*AndroidChiakiVideoPresenterReleaseCallback)(void *user, bool dropped);
typedef void (*AndroidChiakiPerformanceHintThreadCallback)(void *user, ChiakiThreadName role);
typedef void (*AndroidChiakiPerformanceHintReportCallback)(void *user, ChiakiThreadName role,
		uint64_t actual_duration_ns);

typedef struct android_chiaki_video_presenter_frame_t
{
	size_t index;
	AMediaCodecBufferInfo info;
	int64_t arrival_ns;
	ChiakiSeqNum16 frame_index;
	uint64_t frame_ready_time_us;
	bool input_metadata_valid;
} AndroidChiakiVideoPresenterFrame;

typedef struct android_chiaki_video_presenter_t
{
	ChiakiLog *log;
	ChiakiMutex mutex;
	ChiakiCond queue_cond;
	AMediaCodec *codec;
	ChiakiThread output_thread;
	ChiakiThread vsync_thread;
	bool output_thread_started;
	bool vsync_thread_started;
	bool shutdown;
	ALooper *looper;
	AChoreographer *choreographer;

	AndroidChiakiVideoPresenterFrame queue[ANDROID_CHIAKI_VIDEO_PRESENTER_QUEUE_CAPACITY];
	uint32_t queue_head;
	uint32_t queue_size;

	AndroidChiakiVideoPresenterConfig config;
	AndroidChiakiVideoPacingMode mode;
	AndroidChiakiVideoPresenterLead lead_mode;
	AndroidChiakiVideoRecoveryStrategy recovery_strategy;
	bool timestamped_release_enabled;
	bool late_frame_recovery_enabled;
	bool real_pts_enabled;
	bool nonblocking_producer;
	bool dejitter_enabled;
	unsigned int stream_fps;
	double refresh_hz;
	int64_t app_vsync_offset_ns;
	int64_t vsync_period_ns;
	bool vsync_period_waiting_for_choreographer;
	int64_t last_vsync_ns;

	bool timeline_valid;
	int64_t timeline_offset_ns;
	uint64_t dejitter_buffer_ns;
	int64_t arrival_offsets[ANDROID_CHIAKI_VIDEO_PRESENTER_JITTER_WINDOW];
	int64_t last_arrival_ns;
	uint32_t arrival_offset_count;
	uint32_t arrival_offset_next;
	uint32_t samples_since_adjustment;
	uint32_t decrease_hysteresis;

	uint64_t missed_vsyncs;
	uint64_t dropped_frames;
	uint64_t bounded_age_dropped_frames;
	uint64_t recovery_flushes;
	uint64_t recovery_flushed_frames;
	uint32_t max_queue_age_periods;
	bool diagnostics_enabled;
	bool stats_log_enabled;
	AndroidChiakiVideoInputMetadata input_metadata[ANDROID_CHIAKI_VIDEO_INPUT_METADATA_CAPACITY];
	uint32_t input_metadata_next;
	uint64_t diagnostics_decode_ns[ANDROID_CHIAKI_VIDEO_DIAGNOSTICS_CAPACITY];
	uint32_t diagnostics_decode_count;
	uint32_t diagnostics_decode_next;
	uint64_t diagnostics_output_frames;
	AndroidChiakiVideoCadence cadence;
	uint64_t cadence_last_dropped_frames;
	uint64_t cadence_window_dropped_frames;
	AndroidChiakiVideoPresenterReleaseCallback release_cb;
	void *release_cb_user;
	AndroidChiakiPerformanceHintThreadCallback performance_hint_thread_start_cb;
	AndroidChiakiPerformanceHintReportCallback performance_hint_report_cb;
	AndroidChiakiPerformanceHintThreadCallback performance_hint_thread_stop_cb;
	void *performance_hint_cb_user;
	// PLE-698: the Go cinema's per-frame latency; NULL unless the stats log is on.
	AndroidChiakiVideoFrameLatency *frame_latency;
} AndroidChiakiVideoPresenter;

ChiakiErrorCode android_chiaki_video_presenter_init(AndroidChiakiVideoPresenter *presenter, ChiakiLog *log,
		bool late_frame_recovery_enabled, bool real_pts_enabled, bool diagnostics_enabled, bool stats_log_enabled,
		const AndroidChiakiVideoPresenterConfig *config,
		AndroidChiakiVideoPresenterReleaseCallback release_cb, void *release_cb_user);
void android_chiaki_video_presenter_fini(AndroidChiakiVideoPresenter *presenter);
void android_chiaki_video_presenter_set_performance_hint_callbacks(AndroidChiakiVideoPresenter *presenter,
		AndroidChiakiPerformanceHintThreadCallback start_cb,
		AndroidChiakiPerformanceHintReportCallback report_cb,
		AndroidChiakiPerformanceHintThreadCallback stop_cb, void *user);
/** PLE-698: set before the presenter starts; every frame rendered to the surface is recorded there. */
void android_chiaki_video_presenter_set_frame_latency(AndroidChiakiVideoPresenter *presenter,
		AndroidChiakiVideoFrameLatency *frame_latency);
ChiakiErrorCode android_chiaki_video_presenter_start(AndroidChiakiVideoPresenter *presenter, AMediaCodec *codec,
		unsigned int stream_fps, double refresh_hz, int64_t app_vsync_offset_ns);
void android_chiaki_video_presenter_request_stop(AndroidChiakiVideoPresenter *presenter);
void android_chiaki_video_presenter_join(AndroidChiakiVideoPresenter *presenter);
void android_chiaki_video_presenter_set_mode(AndroidChiakiVideoPresenter *presenter,
		AndroidChiakiVideoPacingMode mode);
void android_chiaki_video_presenter_set_timing(AndroidChiakiVideoPresenter *presenter,
		unsigned int stream_fps, double refresh_hz, int64_t app_vsync_offset_ns);
void android_chiaki_video_presenter_get_stats(AndroidChiakiVideoPresenter *presenter,
		AndroidChiakiVideoPresenterStats *stats);
void android_chiaki_video_presenter_record_input_queued(AndroidChiakiVideoPresenter *presenter,
		int64_t presentation_time_us, int64_t queued_ns, ChiakiSeqNum16 frame_index,
		uint64_t frame_ready_time_us);
void android_chiaki_video_presenter_get_diagnostics(AndroidChiakiVideoPresenter *presenter,
		AndroidChiakiVideoPresenterDiagnostics *diagnostics);

#endif
