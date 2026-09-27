// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_TRACE_H
#define CHIAKI_TRACE_H

#include "common.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * PLE-746: points on the input and video paths a platform profiler can mark (the Android client
 * turns them into atrace sections and its input-to-photon probe). The lib stays platform-free:
 * with no callback set, as in every build that never calls chiaki_trace_set_event_cb, each point
 * costs one load and one branch.
 */
typedef enum chiaki_trace_event_t
{
	/** A video AV packet reached the video receiver. a = frame index, b = unit index. */
	CHIAKI_TRACE_EVENT_VIDEO_PACKET = 0,
	/** The receiver completed a frame and hands it to the decoder. a = frame index, b = frame_ready_time_us. */
	CHIAKI_TRACE_EVENT_VIDEO_FRAME_READY,
	/**
	 * A feedback history packet (the one that carries button edges) left for the console.
	 * a = the controller buttons as of the change that queued it, b = its history sequence number.
	 */
	CHIAKI_TRACE_EVENT_FEEDBACK_HISTORY_SENT,
	/** A feedback state packet (sticks and motion) left for the console. a = its sequence number. */
	CHIAKI_TRACE_EVENT_FEEDBACK_STATE_SENT,
} ChiakiTraceEvent;

typedef void (*ChiakiTraceEventFunc)(ChiakiTraceEvent event, uint64_t a, uint64_t b, void *user);

/** Set once, before any session starts (like chiaki_thread_set_affinity_cb); NULL removes it. */
CHIAKI_EXPORT void chiaki_trace_set_event_cb(ChiakiTraceEventFunc func, void *user);
CHIAKI_EXPORT void chiaki_trace_event(ChiakiTraceEvent event, uint64_t a, uint64_t b);

#ifdef __cplusplus
}
#endif

#endif // CHIAKI_TRACE_H
