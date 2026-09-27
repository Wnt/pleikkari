// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "video-frame-latency.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const figure_names[ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_FIGURES] = {
	"arrival_decoded",
	"decoded_latched",
	"latched_submitted",
	"submitted_photon",
	"total",
};

/** Called with the mutex held. */
static void reset_window_locked(AndroidChiakiVideoFrameLatency *latency, int64_t now_ns)
{
	latency->window_start_ns = now_ns;
	latency->decoded = 0;
	latency->latched = 0;
	latency->shown = 0;
	latency->replaced = 0;
	latency->unmatched = 0;
	for(int i = 0; i < ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_FIGURES; i++)
	{
		latency->sample_count[i] = 0;
		latency->sample_sum[i] = 0;
		latency->sample_max[i] = 0;
	}
}

ChiakiErrorCode android_chiaki_video_frame_latency_init(AndroidChiakiVideoFrameLatency *latency)
{
	memset(latency, 0, sizeof(*latency));
	return chiaki_mutex_init(&latency->mutex, false);
}

void android_chiaki_video_frame_latency_fini(AndroidChiakiVideoFrameLatency *latency)
{
	chiaki_mutex_fini(&latency->mutex);
}

void android_chiaki_video_frame_latency_set_enabled(AndroidChiakiVideoFrameLatency *latency, bool enabled,
		int64_t now_ns)
{
	chiaki_mutex_lock(&latency->mutex);
	latency->enabled = enabled;
	latency->pending_head = 0;
	latency->pending_size = 0;
	reset_window_locked(latency, now_ns);
	chiaki_mutex_unlock(&latency->mutex);
}

void android_chiaki_video_frame_latency_set_row_cb(AndroidChiakiVideoFrameLatency *latency,
		AndroidChiakiVideoFrameLatencyRowCallback cb, void *user)
{
	chiaki_mutex_lock(&latency->mutex);
	latency->row_cb = cb;
	latency->row_cb_user = user;
	chiaki_mutex_unlock(&latency->mutex);
}

/** Called with the mutex held. */
static void emit_row_locked(AndroidChiakiVideoFrameLatency *latency, AndroidChiakiVideoFrameLatencyRowKind kind,
		const AndroidChiakiVideoFrameLatencyPending *frame, int64_t latched_ns, int64_t submitted_ns,
		int64_t predicted_display_ns, int32_t luma)
{
	if(!latency->row_cb)
		return;
	AndroidChiakiVideoFrameLatencyRow row = {
		.kind = kind,
		.buffer_timestamp_ns = frame->buffer_timestamp_ns,
		.frame_index = frame->frame_index,
		.arrival_ns = frame->arrival_ns,
		.queued_ns = frame->queued_ns,
		.decoded_ns = frame->decoded_ns,
		.latched_ns = latched_ns,
		.submitted_ns = submitted_ns,
		.predicted_display_ns = predicted_display_ns,
		.luma = luma,
	};
	latency->row_cb(latency->row_cb_user, &row);
}

/** Called with the mutex held. */
static void add_sample_locked(AndroidChiakiVideoFrameLatency *latency, AndroidChiakiVideoFrameLatencyFigure figure,
		int64_t value_ns)
{
	uint32_t count = latency->sample_count[figure];
	if(count < ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_SAMPLES)
		latency->samples[figure][count] = value_ns;
	if(count == 0 || value_ns > latency->sample_max[figure])
		latency->sample_max[figure] = value_ns;
	latency->sample_sum[figure] += value_ns;
	latency->sample_count[figure] = count + 1;
}

void android_chiaki_video_frame_latency_record_decoded(AndroidChiakiVideoFrameLatency *latency,
		int64_t buffer_timestamp_ns, int32_t frame_index, int64_t arrival_ns, int64_t queued_ns, int64_t decoded_ns)
{
	chiaki_mutex_lock(&latency->mutex);
	if(!latency->enabled)
	{
		chiaki_mutex_unlock(&latency->mutex);
		return;
	}
	latency->decoded++;
	if(latency->pending_size == ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_PENDING)
	{
		// A second of frames with no latch: the oldest was never shown.
		emit_row_locked(latency, ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_ROW_REPLACED,
				&latency->pending[latency->pending_head], 0, 0, 0, -1);
		latency->pending_head = (latency->pending_head + 1) % ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_PENDING;
		latency->pending_size--;
		latency->replaced++;
	}
	uint32_t tail = (latency->pending_head + latency->pending_size) % ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_PENDING;
	latency->pending[tail].buffer_timestamp_ns = buffer_timestamp_ns;
	latency->pending[tail].frame_index = frame_index;
	latency->pending[tail].arrival_ns = arrival_ns;
	latency->pending[tail].queued_ns = queued_ns;
	latency->pending[tail].decoded_ns = decoded_ns;
	latency->pending_size++;
	chiaki_mutex_unlock(&latency->mutex);
}

void android_chiaki_video_frame_latency_record_latched(AndroidChiakiVideoFrameLatency *latency,
		int64_t buffer_timestamp_ns, int64_t latched_ns, int64_t submitted_ns, int64_t predicted_display_ns,
		int32_t luma)
{
	chiaki_mutex_lock(&latency->mutex);
	if(!latency->enabled)
	{
		chiaki_mutex_unlock(&latency->mutex);
		return;
	}
	latency->latched++;
	// Frames reach the surface in release order and the latch consumes them in that order, so
	// every pending frame ahead of the latched one was replaced before it could be shown.
	bool found = false;
	AndroidChiakiVideoFrameLatencyPending frame = { 0 };
	for(uint32_t offset = 0; offset < latency->pending_size; offset++)
	{
		uint32_t index = (latency->pending_head + offset) % ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_PENDING;
		if(latency->pending[index].buffer_timestamp_ns != buffer_timestamp_ns)
			continue;
		frame = latency->pending[index];
		found = true;
		for(uint32_t skipped = 0; skipped < offset; skipped++)
			emit_row_locked(latency, ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_ROW_REPLACED,
					&latency->pending[(latency->pending_head + skipped) % ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_PENDING],
					0, 0, 0, -1);
		latency->replaced += offset;
		latency->pending_head = (index + 1) % ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_PENDING;
		latency->pending_size -= offset + 1;
		break;
	}
	if(!found)
	{
		latency->unmatched++;
		frame.buffer_timestamp_ns = buffer_timestamp_ns;
		frame.frame_index = -1;
	}
	emit_row_locked(latency, found ? ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_ROW_LATCHED : ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_ROW_UNMATCHED,
			&frame, latched_ns, submitted_ns, predicted_display_ns, luma);
	if(submitted_ns > 0)
	{
		latency->shown++;
		if(found && frame.arrival_ns > 0)
			add_sample_locked(latency, ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_ARRIVAL_DECODED, frame.decoded_ns - frame.arrival_ns);
		if(found)
			add_sample_locked(latency, ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_DECODED_LATCHED, latched_ns - frame.decoded_ns);
		add_sample_locked(latency, ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_LATCHED_SUBMITTED, submitted_ns - latched_ns);
		add_sample_locked(latency, ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_SUBMITTED_PHOTON, predicted_display_ns - submitted_ns);
		if(found && frame.arrival_ns > 0)
			add_sample_locked(latency, ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_TOTAL, predicted_display_ns - frame.arrival_ns);
	}
	chiaki_mutex_unlock(&latency->mutex);
}

static int compare_i64(const void *left, const void *right)
{
	int64_t a = *(const int64_t *)left;
	int64_t b = *(const int64_t *)right;
	return a < b ? -1 : a > b ? 1 : 0;
}

/** Nearest rank on sorted values: the smallest value with at least percent % of them at or below it. */
static int64_t nearest_rank(const int64_t *sorted, uint32_t count, uint32_t percent)
{
	uint32_t rank = (percent * count + 99) / 100;
	return sorted[rank > 0 ? rank - 1 : 0];
}

bool android_chiaki_video_frame_latency_take_window(AndroidChiakiVideoFrameLatency *latency, int64_t now_ns,
		AndroidChiakiVideoFrameLatencyWindow *window)
{
	memset(window, 0, sizeof(*window));
	chiaki_mutex_lock(&latency->mutex);
	if(!latency->enabled)
	{
		chiaki_mutex_unlock(&latency->mutex);
		return false;
	}
	window->window_ns = now_ns - latency->window_start_ns;
	window->decoded = latency->decoded;
	window->latched = latency->latched;
	window->shown = latency->shown;
	window->replaced = latency->replaced;
	window->unmatched = latency->unmatched;
	for(int i = 0; i < ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_FIGURES; i++)
	{
		uint32_t count = latency->sample_count[i];
		AndroidChiakiVideoFrameLatencyStat *stat = &window->figures[i];
		stat->count = count;
		if(count == 0)
			continue;
		uint32_t kept = count < ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_SAMPLES ? count : ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_SAMPLES;
		qsort(latency->samples[i], kept, sizeof(latency->samples[i][0]), compare_i64);
		stat->p50_ns = nearest_rank(latency->samples[i], kept, 50);
		stat->p95_ns = nearest_rank(latency->samples[i], kept, 95);
		stat->max_ns = latency->sample_max[i];
		stat->mean_ns = latency->sample_sum[i] / (int64_t)count;
	}
	reset_window_locked(latency, now_ns);
	chiaki_mutex_unlock(&latency->mutex);
	return true;
}

void android_chiaki_video_frame_latency_format(const AndroidChiakiVideoFrameLatencyWindow *window,
		char *buf, size_t buf_size)
{
	if(buf_size == 0)
		return;
	int written = snprintf(buf, buf_size,
			"Cinema latency: window %lld ms decoded %u latched %u shown %u replaced %u unmatched %u"
			" | p50/p95/max/mean ms",
			(long long)(window->window_ns / 1000000), window->decoded, window->latched, window->shown,
			window->replaced, window->unmatched);
	for(int i = 0; i < ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_FIGURES && written >= 0 && (size_t)written < buf_size; i++)
	{
		const AndroidChiakiVideoFrameLatencyStat *stat = &window->figures[i];
		int n;
		if(stat->count == 0)
			n = snprintf(buf + written, buf_size - (size_t)written, " %s n/a", figure_names[i]);
		else
			n = snprintf(buf + written, buf_size - (size_t)written, " %s %.1f/%.1f/%.1f/%.1f", figure_names[i],
					(double)stat->p50_ns / 1e6, (double)stat->p95_ns / 1e6,
					(double)stat->max_ns / 1e6, (double)stat->mean_ns / 1e6);
		if(n < 0)
			break;
		written += n;
	}
}
