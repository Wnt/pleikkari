// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_JNI_LATENCY_PROBE_H
#define CHIAKI_JNI_LATENCY_PROBE_H

// PLE-746: the Oculus Go's input-to-photon probe. With stream_go_vr_latency_probe on, it writes two
// CSV files for scripts/dev/go-latency/input_to_photon.py, all times CLOCK_MONOTONIC ns:
//   presses.csv: one row per Cross press, from the Android KeyEvent to the feedback history packet
//                that carried the press to the console;
//   frames.csv:  one row per video frame PLE-698's join settles (video-frame-latency.h), with the
//                latched picture's mean luma, so a press can be matched to the first frame whose
//                brightness flipped (PS5 Settings > Accessibility > Invert Color).

#include "video-frame-latency.h"

#include <chiaki/common.h>
#include <chiaki/thread.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct android_chiaki_latency_probe_t
{
	ChiakiMutex mutex;
	bool enabled;
	FILE *presses;
	FILE *frames;
	uint32_t presses_written;
	uint32_t frames_written;
	uint32_t buttons_prev; // the last controller state's buttons
	uint32_t press_buttons; // PLE-803: the buttons that open a press (Cross by default)
	// The press on its way: KeyEvent -> controller state -> history packet sent.
	bool pending;
	uint32_t press_id;
	int64_t event_ns; // KeyEvent.getEventTime(), ms resolution; 0 when the press did not come from a KeyEvent
	int64_t received_ns; // the app's key dispatch saw it
	int64_t state_ns; // the controller state with Cross down was handed to chiaki
} AndroidChiakiLatencyProbe;

#define ANDROID_CHIAKI_LATENCY_PROBE_PRESSES_HEADER \
	"press,input_event_ns,app_received_ns,state_set_ns,sent_ns,history_seq\n"
#define ANDROID_CHIAKI_LATENCY_PROBE_FRAMES_HEADER \
	"frame_index,buffer_ts_ns,kind,ready_ns,queued_ns,decoded_ns,latched_ns,submitted_ns,predicted_ns,luma\n"

ChiakiErrorCode android_chiaki_latency_probe_init(AndroidChiakiLatencyProbe *probe);
void android_chiaki_latency_probe_fini(AndroidChiakiLatencyProbe *probe);
/**
 * Takes ownership of both files, writes their headers and starts recording; a previous run is
 * stopped first. False, with nothing recorded, when either file is NULL (both are closed).
 */
bool android_chiaki_latency_probe_start(AndroidChiakiLatencyProbe *probe, FILE *presses, FILE *frames);
/**
 * Writes a press still on its way (sent 0), then closes both files. False when not started;
 * else the rows written go to presses and frames (either may be NULL).
 */
bool android_chiaki_latency_probe_stop(AndroidChiakiLatencyProbe *probe, uint32_t *presses, uint32_t *frames);
/** PLE-803: presses are these buttons going down (0 keeps the current mask); default Cross. */
void android_chiaki_latency_probe_set_press_buttons(AndroidChiakiLatencyProbe *probe, uint32_t buttons);
bool android_chiaki_latency_probe_enabled(AndroidChiakiLatencyProbe *probe);
/**
 * A KeyEvent putting [buttons] down reached the app. It opens a press only when they include a press
 * button (PLE-829: the mask decides, not the caller); a press still on its way is written unsent first.
 * True when it opened one.
 */
bool android_chiaki_latency_probe_press(AndroidChiakiLatencyProbe *probe, uint32_t buttons, int64_t event_ns,
		int64_t received_ns);
/**
 * The app handed chiaki a controller state. Cross going down stamps the pending press, or opens
 * one with no KeyEvent (a pad the probe did not see, or the debug pad broadcast).
 */
void android_chiaki_latency_probe_controller_state(AndroidChiakiLatencyProbe *probe, uint32_t buttons, int64_t now_ns);
/** A feedback history packet left; the first one with Cross down completes the pending press. */
void android_chiaki_latency_probe_history_sent(AndroidChiakiLatencyProbe *probe, uint32_t buttons, uint32_t history_seq,
		int64_t now_ns);
/** An AndroidChiakiVideoFrameLatencyRowCallback; user is the probe. */
void android_chiaki_latency_probe_frame_row(void *user, const AndroidChiakiVideoFrameLatencyRow *row);

#endif
