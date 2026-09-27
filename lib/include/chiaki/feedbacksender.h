// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_FEEDBACKSENDER_H
#define CHIAKI_FEEDBACKSENDER_H

#include "controller.h"
#include "takion.h"
#include "thread.h"
#include "common.h"

#define CHIAKI_FEEDBACK_HISTORY_PACKET_BUF_SIZE 0x300
#define CHIAKI_FEEDBACK_HISTORY_PACKET_QUEUE_SIZE 0x40
#define CHIAKI_FEEDBACK_GAP_WARN_MS 50

#ifdef __cplusplus
extern "C" {
#endif

typedef struct chiaki_feedback_sender_t
{
	ChiakiLog *log;
	ChiakiTakion *takion;
	ChiakiThread thread;

	ChiakiSeqNum16 state_seq_num;

	ChiakiSeqNum16 history_seq_num;
	ChiakiFeedbackHistoryBuffer history_buf;
	uint8_t history_packets[CHIAKI_FEEDBACK_HISTORY_PACKET_QUEUE_SIZE][CHIAKI_FEEDBACK_HISTORY_PACKET_BUF_SIZE];
	size_t history_packet_sizes[CHIAKI_FEEDBACK_HISTORY_PACKET_QUEUE_SIZE];
	// PLE-746: the controller buttons as of the change that queued each packet, for chiaki_trace_event
	uint32_t history_packet_buttons[CHIAKI_FEEDBACK_HISTORY_PACKET_QUEUE_SIZE];
	size_t history_packet_begin;
	size_t history_packet_len;

	bool should_stop;
	ChiakiControllerState controller_state_prev;
	ChiakiControllerState controller_state_history_prev;
	ChiakiControllerState controller_state;
	bool controller_state_changed;
	bool history_dirty;
	ChiakiMutex state_mutex;
	ChiakiCond state_cond;
	uint32_t state_min_interval_ms;
	uint64_t last_feedback_state_ms;

	// Retained for ABI compatibility; periodic logging now uses the shared stream-stats event.
	uint32_t stats_log_interval_ms;
	uint64_t stats_window_start_ms;
	uint64_t stats_state_packets;
	uint64_t stats_history_packets;
	uint64_t stats_packets_total;
	uint64_t stats_last_send_ms;
	uint64_t stats_gap_sum_ms;
	uint64_t stats_gap_count;
	uint64_t stats_gap_max_ms;
	uint64_t stats_gaps_over_50_ms;
} ChiakiFeedbackSender;

typedef struct chiaki_feedback_sender_stats_t
{
	uint64_t packets_total;
	uint64_t gap_sum_ms;
	uint64_t gap_count;
	uint64_t gap_max_ms;
	uint64_t gaps_over_50_ms;
} ChiakiFeedbackSenderStats;

/**
 * @param state_min_interval_ms minimum time in ms to wait between sending 2 controller state
 *        packets, or 0 to use the default (8ms)
 * @param stats_log_interval_ms retained for API compatibility; the stream connection now owns
 *        the general periodic stats event and log cadence
 */
CHIAKI_EXPORT ChiakiErrorCode chiaki_feedback_sender_init(ChiakiFeedbackSender *feedback_sender, ChiakiTakion *takion, uint32_t state_min_interval_ms, uint32_t stats_log_interval_ms);
CHIAKI_EXPORT void chiaki_feedback_sender_fini(ChiakiFeedbackSender *feedback_sender);
CHIAKI_EXPORT ChiakiErrorCode chiaki_feedback_sender_set_controller_state(ChiakiFeedbackSender *feedback_sender, ChiakiControllerState *state);
CHIAKI_EXPORT uint64_t chiaki_feedback_sender_get_packets_total(ChiakiFeedbackSender *feedback_sender);
CHIAKI_EXPORT void chiaki_feedback_sender_get_stats(ChiakiFeedbackSender *feedback_sender, ChiakiFeedbackSenderStats *stats, bool reset_gaps);
CHIAKI_EXPORT void chiaki_feedback_sender_record_send(ChiakiFeedbackSender *feedback_sender, uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif // CHIAKI_FEEDBACKSENDER_H
