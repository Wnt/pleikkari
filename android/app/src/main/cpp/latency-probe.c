// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "latency-probe.h"

#include <chiaki/controller.h>

#include <inttypes.h>
#include <string.h>

static const char *const row_kinds[] = { "latched", "replaced", "unmatched" };

ChiakiErrorCode android_chiaki_latency_probe_init(AndroidChiakiLatencyProbe *probe)
{
	memset(probe, 0, sizeof(*probe));
	probe->press_buttons = CHIAKI_CONTROLLER_BUTTON_CROSS;
	return chiaki_mutex_init(&probe->mutex, false);
}

void android_chiaki_latency_probe_fini(AndroidChiakiLatencyProbe *probe)
{
	android_chiaki_latency_probe_stop(probe, NULL, NULL);
	chiaki_mutex_fini(&probe->mutex);
}

/** Called with the mutex held. */
static void write_press_locked(AndroidChiakiLatencyProbe *probe, int64_t sent_ns, int64_t history_seq)
{
	fprintf(probe->presses, "%" PRIu32 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 "\n",
			probe->press_id, probe->event_ns, probe->received_ns, probe->state_ns, sent_ns, history_seq);
	// A handful of rows a run: flush each, so a force-stopped app still leaves them on disk.
	fflush(probe->presses);
	probe->presses_written++;
	probe->pending = false;
}

/** Called with the mutex held. */
static void open_press_locked(AndroidChiakiLatencyProbe *probe, int64_t event_ns, int64_t received_ns)
{
	if(probe->pending)
		write_press_locked(probe, 0, -1);
	probe->pending = true;
	probe->press_id++;
	probe->event_ns = event_ns;
	probe->received_ns = received_ns;
	probe->state_ns = 0;
}

bool android_chiaki_latency_probe_start(AndroidChiakiLatencyProbe *probe, FILE *presses, FILE *frames)
{
	android_chiaki_latency_probe_stop(probe, NULL, NULL);
	if(!presses || !frames)
	{
		if(presses)
			fclose(presses);
		if(frames)
			fclose(frames);
		return false;
	}
	chiaki_mutex_lock(&probe->mutex);
	probe->presses = presses;
	probe->frames = frames;
	fputs(ANDROID_CHIAKI_LATENCY_PROBE_PRESSES_HEADER, presses);
	fputs(ANDROID_CHIAKI_LATENCY_PROBE_FRAMES_HEADER, frames);
	fflush(presses);
	probe->presses_written = 0;
	probe->frames_written = 0;
	probe->buttons_prev = 0;
	probe->pending = false;
	probe->press_id = 0;
	probe->enabled = true;
	chiaki_mutex_unlock(&probe->mutex);
	return true;
}

bool android_chiaki_latency_probe_stop(AndroidChiakiLatencyProbe *probe, uint32_t *presses, uint32_t *frames)
{
	chiaki_mutex_lock(&probe->mutex);
	bool was_enabled = probe->enabled;
	if(was_enabled)
	{
		if(probe->pending)
			write_press_locked(probe, 0, -1);
		if(presses)
			*presses = probe->presses_written;
		if(frames)
			*frames = probe->frames_written;
		fclose(probe->presses);
		fclose(probe->frames);
		probe->presses = NULL;
		probe->frames = NULL;
		probe->enabled = false;
	}
	chiaki_mutex_unlock(&probe->mutex);
	return was_enabled;
}

void android_chiaki_latency_probe_set_press_buttons(AndroidChiakiLatencyProbe *probe, uint32_t buttons)
{
	if(!buttons)
		return;
	chiaki_mutex_lock(&probe->mutex);
	probe->press_buttons = buttons;
	chiaki_mutex_unlock(&probe->mutex);
}

bool android_chiaki_latency_probe_enabled(AndroidChiakiLatencyProbe *probe)
{
	chiaki_mutex_lock(&probe->mutex);
	bool enabled = probe->enabled;
	chiaki_mutex_unlock(&probe->mutex);
	return enabled;
}

void android_chiaki_latency_probe_press(AndroidChiakiLatencyProbe *probe, int64_t event_ns, int64_t received_ns)
{
	chiaki_mutex_lock(&probe->mutex);
	if(probe->enabled)
		open_press_locked(probe, event_ns, received_ns);
	chiaki_mutex_unlock(&probe->mutex);
}

void android_chiaki_latency_probe_controller_state(AndroidChiakiLatencyProbe *probe, uint32_t buttons, int64_t now_ns)
{
	chiaki_mutex_lock(&probe->mutex);
	if(probe->enabled)
	{
		bool press_down = (buttons & probe->press_buttons) && !(probe->buttons_prev & probe->press_buttons);
		if(press_down)
		{
			if(!probe->pending || probe->state_ns != 0)
				open_press_locked(probe, 0, 0);
			probe->state_ns = now_ns;
		}
		probe->buttons_prev = buttons;
	}
	chiaki_mutex_unlock(&probe->mutex);
}

void android_chiaki_latency_probe_history_sent(AndroidChiakiLatencyProbe *probe, uint32_t buttons, uint32_t history_seq,
		int64_t now_ns)
{
	chiaki_mutex_lock(&probe->mutex);
	if(probe->enabled && probe->pending && probe->state_ns != 0 && (buttons & probe->press_buttons))
		write_press_locked(probe, now_ns, history_seq);
	chiaki_mutex_unlock(&probe->mutex);
}

void android_chiaki_latency_probe_frame_row(void *user, const AndroidChiakiVideoFrameLatencyRow *row)
{
	AndroidChiakiLatencyProbe *probe = user;
	chiaki_mutex_lock(&probe->mutex);
	if(probe->enabled)
	{
		// About 60 rows a second; flushed about once a second, so a force-stopped app loses under a second.
		fprintf(probe->frames, "%" PRId32 ",%" PRId64 ",%s,%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId32 "\n",
				row->frame_index, row->buffer_timestamp_ns,
				(unsigned)row->kind < sizeof(row_kinds) / sizeof(row_kinds[0]) ? row_kinds[row->kind] : "unknown",
				row->arrival_ns, row->queued_ns, row->decoded_ns, row->latched_ns, row->submitted_ns,
				row->predicted_display_ns, row->luma);
		if(++probe->frames_written % 64 == 0)
			fflush(probe->frames);
	}
	chiaki_mutex_unlock(&probe->mutex);
}
