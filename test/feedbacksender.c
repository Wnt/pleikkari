// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <munit.h>

#include <chiaki/ecdh.h>
#include <chiaki/feedbacksender.h>
#include <chiaki/gkcrypt.h>
#include <chiaki/takion.h>
#include <chiaki/thread.h>
#include <chiaki/time.h>

#include <string.h>

#include "fake_console.h"
#include "test_log.h"

#if defined(__linux__)

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#define FEEDBACK_TEST_PACKET_HISTORY 1 // TAKION_PACKET_TYPE_FEEDBACK_HISTORY
#define FEEDBACK_TEST_PACKET_STATE 6 // TAKION_PACKET_TYPE_FEEDBACK_STATE
#define FEEDBACK_TEST_STATE_MIN_MS 8 // the sender's default state_min_interval_ms
#define FEEDBACK_TEST_KEEPALIVE_MS 200 // FEEDBACK_STATE_TIMEOUT_MAX_MS
#define FEEDBACK_TEST_AT_ONCE_US 1000
// The sender reads its clock in whole ms, so it sends a throttled state packet up to 1 ms after
// the minimum; the rest is the host's scheduling.
#define FEEDBACK_TEST_AT_MINIMUM_SLACK_US 2000
#define FEEDBACK_TEST_STATE_STICKS_OFFSET (0xc + 0x11) // left_x in a v12 state packet
// A loaded host can delay a thread's wake-up past FEEDBACK_TEST_AT_ONCE_US now and then, so a
// press gets this many tries. The defect is not noise: it held every press it hit for up to
// the keepalive deadline, and no try may come near that.
#define FEEDBACK_TEST_ATTEMPTS 10
// Every try must stay under this, however the host scheduled it. A stale deadline held a change
// inside the state minimum until the keepalive, at least FEEDBACK_TEST_KEEPALIVE_MS -
// FEEDBACK_TEST_STATE_MIN_MS after it; a host at load 34 once delayed a try by 69 ms (PLE-823).
#define FEEDBACK_TEST_NEVER_US (FEEDBACK_TEST_KEEPALIVE_MS * 1000 * 3 / 4)

typedef struct feedback_test_rig_t
{
	FakeConsole console;
	ChiakiMutex mutex;
	ChiakiCond cond;
	bool connected;
	ChiakiGKCrypt gkcrypt;
	ChiakiGKCrypt console_gkcrypt; // the same keys, so the console can read a state packet's sticks
	ChiakiTakion takion;
	ChiakiFeedbackSender sender;
	ChiakiControllerState state;
	uint16_t state_seq_num_next;
	uint16_t history_seq_num_next;
} FeedbackTestRig;

typedef struct feedback_test_packet_t
{
	uint8_t type;
	uint16_t seq_num;
	uint64_t at_us; // when the console received it
	int16_t left_x; // state packets only
} FeedbackTestPacket;

static void feedback_test_takion_cb(ChiakiTakionEvent *event, void *user)
{
	FeedbackTestRig *rig = user;
	chiaki_mutex_lock(&rig->mutex);
	if(event->type == CHIAKI_TAKION_EVENT_TYPE_CONNECTED)
		rig->connected = true;
	chiaki_cond_signal(&rig->cond);
	chiaki_mutex_unlock(&rig->mutex);
}

static bool feedback_test_connected(void *user)
{
	return ((FeedbackTestRig *)user)->connected;
}

/**
 * A real ChiakiFeedbackSender on a real takion, connected to PLE-490's loopback console, so
 * the console sees each packet the moment it leaves.
 */
static void feedback_test_rig_start(FeedbackTestRig *rig)
{
	memset(rig, 0, sizeof(*rig));
	fake_console_open(&rig->console);
	munit_assert_int(chiaki_mutex_init(&rig->mutex, false), ==, CHIAKI_ERR_SUCCESS);
	munit_assert_int(chiaki_cond_init(&rig->cond), ==, CHIAKI_ERR_SUCCESS);

	ChiakiTakionConnectInfo info = { 0 };
	info.log = get_test_log();
	info.close_socket = false;
	info.ip_dontfrag = false;
	info.enable_crypt = false;
	info.protocol_version = 12;
	info.cb = feedback_test_takion_cb;
	info.cb_user = rig;
	chiaki_key_state_init(&rig->takion.key_state);
	munit_assert_int(chiaki_takion_connect(&rig->takion, &info, &rig->console.client_sock), ==, CHIAKI_ERR_SUCCESS);
	fake_console_handshake(&rig->console);
	chiaki_mutex_lock(&rig->mutex);
	chiaki_cond_timedwait_pred(&rig->cond, &rig->mutex, 5000, feedback_test_connected, rig);
	munit_assert_true(rig->connected);
	chiaki_mutex_unlock(&rig->mutex);

	// Feedback packets are encrypted whatever enable_crypt says. The stream connection sets
	// the keys before it starts the sender; any key does here, the console never decrypts.
	static const uint8_t handshake_key[0x10] = { 0 };
	static const uint8_t ecdh_secret[CHIAKI_ECDH_SECRET_SIZE] = { 0 };
	munit_assert_int(chiaki_gkcrypt_init(&rig->gkcrypt, get_test_log(), 0, 2, handshake_key, ecdh_secret), ==, CHIAKI_ERR_SUCCESS);
	munit_assert_int(chiaki_gkcrypt_init(&rig->console_gkcrypt, get_test_log(), 0, 2, handshake_key, ecdh_secret), ==, CHIAKI_ERR_SUCCESS);
	chiaki_takion_set_crypt(&rig->takion, &rig->gkcrypt, NULL);

	chiaki_controller_state_set_idle(&rig->state);
	munit_assert_int(chiaki_feedback_sender_init(&rig->sender, &rig->takion, 0, 0), ==, CHIAKI_ERR_SUCCESS);
	// The sender counts its start as a state packet, so a stick move inside the state minimum
	// would wait out the rest of it. The first move comes after it.
	usleep(2 * FEEDBACK_TEST_STATE_MIN_MS * 1000);
}

static void feedback_test_rig_stop(FeedbackTestRig *rig)
{
	chiaki_feedback_sender_fini(&rig->sender);
	chiaki_takion_close(&rig->takion);
	chiaki_gkcrypt_fini(&rig->console_gkcrypt);
	chiaki_gkcrypt_fini(&rig->gkcrypt);
	fake_console_fini(&rig->console);
	chiaki_cond_fini(&rig->cond);
	chiaki_mutex_fini(&rig->mutex);
}

/**
 * The next feedback packet at the console, or false if none comes within timeout_ms. Asserts
 * each stream's sequence numbers run on without a gap, a repeat or a reordering.
 */
static bool feedback_test_recv(FeedbackTestRig *rig, int timeout_ms, FeedbackTestPacket *packet)
{
	while(true)
	{
		struct pollfd pfd = { .fd = rig->console.sock, .events = POLLIN };
		int polled = poll(&pfd, 1, timeout_ms);
		munit_assert_int(polled, >=, 0);
		if(polled == 0)
			return false;
		uint8_t buf[1500];
		ssize_t r = recv(rig->console.sock, buf, sizeof(buf), 0);
		uint64_t at_us = chiaki_time_now_monotonic_us();
		munit_assert_int((int)r, >=, 3);
		if(buf[0] != FEEDBACK_TEST_PACKET_HISTORY && buf[0] != FEEDBACK_TEST_PACKET_STATE)
			continue; // takion's own control traffic
		packet->type = buf[0];
		packet->seq_num = ntohs(*((chiaki_unaligned_uint16_t *)(buf + 1)));
		packet->at_us = at_us;
		packet->left_x = 0;
		if(packet->type == FEEDBACK_TEST_PACKET_STATE)
		{
			munit_assert_uint16(packet->seq_num, ==, rig->state_seq_num_next++);
			munit_assert_int((int)r, >=, FEEDBACK_TEST_STATE_STICKS_OFFSET + 2);
			uint32_t key_pos = ntohl(*((chiaki_unaligned_uint32_t *)(buf + 4)));
			munit_assert_int(chiaki_gkcrypt_decrypt(&rig->console_gkcrypt, key_pos + CHIAKI_GKCRYPT_BLOCK_SIZE, buf + 0xc, (size_t)r - 0xc), ==, CHIAKI_ERR_SUCCESS);
			packet->left_x = (int16_t)ntohs(*((chiaki_unaligned_uint16_t *)(buf + FEEDBACK_TEST_STATE_STICKS_OFFSET)));
		}
		else
			munit_assert_uint16(packet->seq_num, ==, rig->history_seq_num_next++);
		return true;
	}
}

/**
 * Moves the left stick, which only a state packet carries, and waits for that packet. The
 * sender stamps the packet no earlier than *move_us.
 */
static FeedbackTestPacket feedback_test_state_packet(FeedbackTestRig *rig, uint64_t *move_us)
{
	rig->state.left_x = rig->state.left_x == 0x1000 ? -0x1000 : 0x1000;
	uint64_t moved_us = chiaki_time_now_monotonic_us();
	if(move_us)
		*move_us = moved_us;
	munit_assert_int(chiaki_feedback_sender_set_controller_state(&rig->sender, &rig->state), ==, CHIAKI_ERR_SUCCESS);
	FeedbackTestPacket packet;
	munit_assert_true(feedback_test_recv(rig, 1000, &packet));
	munit_assert_uint8(packet.type, ==, FEEDBACK_TEST_PACKET_STATE);
	munit_assert_int16(packet.left_x, ==, rig->state.left_x);
	return packet;
}

static void feedback_test_sleep_until(uint64_t at_us)
{
	uint64_t now_us = chiaki_time_now_monotonic_us();
	if(now_us < at_us)
		usleep((useconds_t)(at_us - now_us));
}

/**
 * Moves the left stick alone to left_x at at_us (a state packet only). Returns when it moved.
 */
static uint64_t feedback_test_move_stick_at(FeedbackTestRig *rig, uint64_t at_us, int16_t left_x)
{
	feedback_test_sleep_until(at_us);
	rig->state.left_x = left_x;
	uint64_t moved_us = chiaki_time_now_monotonic_us();
	munit_assert_int(chiaki_feedback_sender_set_controller_state(&rig->sender, &rig->state), ==, CHIAKI_ERR_SUCCESS);
	return moved_us;
}

/**
 * Presses Cross delay_ms after a state packet (or releases it, turn about), stick still: a
 * history packet only. Returns true if the press came inside the state minimum and its history
 * packet reached the console within FEEDBACK_TEST_AT_ONCE_US; false for a try the host's
 * scheduling spoilt.
 */
static bool feedback_test_press_after_state(FeedbackTestRig *rig, unsigned int delay_ms, unsigned int attempt)
{
	FeedbackTestPacket state = feedback_test_state_packet(rig, NULL);
	feedback_test_sleep_until(state.at_us + delay_ms * 1000);

	rig->state.buttons ^= CHIAKI_CONTROLLER_BUTTON_CROSS;
	uint64_t press_us = chiaki_time_now_monotonic_us();
	munit_assert_int(chiaki_feedback_sender_set_controller_state(&rig->sender, &rig->state), ==, CHIAKI_ERR_SUCCESS);

	FeedbackTestPacket history;
	munit_assert_true(feedback_test_recv(rig, 1000, &history));
	uint64_t after_state_us = press_us - state.at_us;
	uint64_t latency_us = history.at_us - press_us;
	munit_logf(MUNIT_LOG_INFO, "press %u ms after a state packet (try %u): %llu us after it, history packet out %llu us after the press",
		delay_ms, attempt, (unsigned long long)after_state_us, (unsigned long long)latency_us);
	// The press moved no stick, so nothing may come ahead of its history packet.
	munit_assert_uint8(history.type, ==, FEEDBACK_TEST_PACKET_HISTORY);
	// Before PLE-800 the sender slept until the keepalive deadline, FEEDBACK_TEST_KEEPALIVE_MS
	// after the state packet, and only then sent the history packet.
	munit_assert_uint64(latency_us, <, FEEDBACK_TEST_NEVER_US);

	if(after_state_us >= FEEDBACK_TEST_STATE_MIN_MS * 1000)
		return false; // the sleep overshot past the state minimum: the press proves nothing
	return latency_us < FEEDBACK_TEST_AT_ONCE_US;
}

// PLE-800: the sender's wait ended early only once the 8 ms state-packet minimum had passed
// since the last state packet. A press inside it queued its history packet and slept until
// the next controller change or the 200 ms keepalive deadline: 3 of 192 presses in PLE-746's
// Go baseline waited 199-200 ms. Presses 1-7 ms after a state packet must go out at once.
static MunitResult test_history_sent_at_once_inside_state_minimum(const MunitParameter params[], void *user)
{
	(void)params;
	(void)user;
	FeedbackTestRig rig;
	feedback_test_rig_start(&rig);

	for(unsigned int delay_ms = 1; delay_ms < FEEDBACK_TEST_STATE_MIN_MS; delay_ms++)
	{
		bool at_once = false;
		for(unsigned int attempt = 0; attempt < FEEDBACK_TEST_ATTEMPTS && !at_once; attempt++)
			at_once = feedback_test_press_after_state(&rig, delay_ms, attempt);
		munit_assert_true(at_once);
	}

	feedback_test_rig_stop(&rig);
	return MUNIT_OK;
}

// PLE-800: the fix wakes the sender for history packets only. A press that also moves a stick
// sends its history packet at once, while its state packet still waits out the state minimum
// since the last one; exactly one state packet follows. Sequence numbers stay per stream, as
// feedback_test_recv() asserts.
static MunitResult test_state_minimum_holds_for_a_stick_moved_with_a_press(const MunitParameter params[], void *user)
{
	(void)params;
	(void)user;
	FeedbackTestRig rig;
	feedback_test_rig_start(&rig);

	bool at_once = false;
	for(unsigned int attempt = 0; attempt < FEEDBACK_TEST_ATTEMPTS && !at_once; attempt++)
	{
		uint64_t move_us;
		FeedbackTestPacket state = feedback_test_state_packet(&rig, &move_us);
		feedback_test_sleep_until(state.at_us + 2000);

		rig.state.buttons ^= CHIAKI_CONTROLLER_BUTTON_CROSS;
		rig.state.right_y = rig.state.right_y == 0x2000 ? -0x2000 : 0x2000;
		uint64_t press_us = chiaki_time_now_monotonic_us();
		munit_assert_int(chiaki_feedback_sender_set_controller_state(&rig.sender, &rig.state), ==, CHIAKI_ERR_SUCCESS);

		FeedbackTestPacket first, second, extra;
		munit_assert_true(feedback_test_recv(&rig, 1000, &first));
		munit_assert_true(feedback_test_recv(&rig, 1000, &second));
		munit_assert_uint8(first.type, !=, second.type); // one history packet, one state packet
		munit_assert_false(feedback_test_recv(&rig, 2 * FEEDBACK_TEST_STATE_MIN_MS, &extra));
		FeedbackTestPacket *history = first.type == FEEDBACK_TEST_PACKET_HISTORY ? &first : &second;
		FeedbackTestPacket *next_state = first.type == FEEDBACK_TEST_PACKET_STATE ? &first : &second;

		uint64_t latency_us = history->at_us - press_us;
		uint64_t history_after_move_us = history->at_us - move_us;
		uint64_t state_gap_us = next_state->at_us - move_us;
		munit_logf(MUNIT_LOG_INFO, "press and stick %llu us after the last stick move (try %u): history packet out %llu us after the press, next state packet %llu us after the move",
			(unsigned long long)(press_us - move_us), attempt, (unsigned long long)latency_us, (unsigned long long)state_gap_us);
		munit_assert_uint64(latency_us, <, FEEDBACK_TEST_NEVER_US);
		// The sender stamped the last state packet after move_us and reads its clock in whole
		// ms, so it sees the minimum pass as much as 1 ms early, never more. A history packet
		// out less than that after move_us was sent from inside the minimum, however the
		// host scheduled the threads, so the state packet must not have come with it.
		if(history_after_move_us >= (FEEDBACK_TEST_STATE_MIN_MS - 1) * 1000)
			continue; // the host delayed the press or the sender past the minimum: both may go at once
		munit_assert_uint8(first.type, ==, FEEDBACK_TEST_PACKET_HISTORY);
		munit_assert_uint64(state_gap_us, >=, (FEEDBACK_TEST_STATE_MIN_MS - 1) * 1000);
		at_once = latency_us < FEEDBACK_TEST_AT_ONCE_US;
	}
	munit_assert_true(at_once);

	feedback_test_rig_stop(&rig);
	return MUNIT_OK;
}

/**
 * Moves the left stick alone delay_ms after a state packet: a state packet only. Returns true if
 * the move came inside the state minimum and its state packet reached the console as the minimum
 * ended; false for a try the host's scheduling spoilt.
 */
static bool feedback_test_stick_after_state(FeedbackTestRig *rig, unsigned int delay_ms, unsigned int attempt)
{
	uint64_t last_move_us;
	FeedbackTestPacket state = feedback_test_state_packet(rig, &last_move_us);
	int16_t left_x = rig->state.left_x / 2;
	uint64_t move_us = feedback_test_move_stick_at(rig, state.at_us + delay_ms * 1000, left_x);

	FeedbackTestPacket next;
	munit_assert_true(feedback_test_recv(rig, 1000, &next));
	uint64_t after_state_us = move_us - state.at_us;
	uint64_t latency_us = next.at_us - move_us;
	uint64_t gap_us = next.at_us - state.at_us;
	munit_logf(MUNIT_LOG_INFO, "stick %u ms after a state packet (try %u): %llu us after it, next state packet %llu us after the move, %llu us after the last one",
		delay_ms, attempt, (unsigned long long)after_state_us, (unsigned long long)latency_us, (unsigned long long)gap_us);
	munit_assert_uint8(next.type, ==, FEEDBACK_TEST_PACKET_STATE);
	munit_assert_int16(next.left_x, ==, left_x);
	// Before PLE-823 the sender slept until the keepalive deadline, FEEDBACK_TEST_KEEPALIVE_MS
	// after the last state packet, or until the next controller change.
	munit_assert_uint64(latency_us, <, FEEDBACK_TEST_NEVER_US);
	// The minimum still holds. The sender stamped the last state packet after last_move_us and
	// reads its clock in whole ms, so it sees the minimum pass as much as 1 ms early, never more.
	munit_assert_uint64(next.at_us - last_move_us, >=, (FEEDBACK_TEST_STATE_MIN_MS - 1) * 1000);

	if(after_state_us >= FEEDBACK_TEST_STATE_MIN_MS * 1000)
		return false; // the sleep overshot past the state minimum: the move proves nothing
	return gap_us < FEEDBACK_TEST_STATE_MIN_MS * 1000 + FEEDBACK_TEST_AT_MINIMUM_SLACK_US;
}

// PLE-823: a stick-only change inside the 8 ms state minimum woke the sender, but its wait kept
// the deadline computed before the change: the next controller change, or the 200 ms keepalive.
// The chiaki-unit rig saw 202 ms. Moves 1-7 ms after a state packet must go out as the minimum
// ends, and not before.
static MunitResult test_stick_change_inside_state_minimum_sent_at_minimum(const MunitParameter params[], void *user)
{
	(void)params;
	(void)user;
	FeedbackTestRig rig;
	feedback_test_rig_start(&rig);

	for(unsigned int delay_ms = 1; delay_ms < FEEDBACK_TEST_STATE_MIN_MS; delay_ms++)
	{
		bool at_minimum = false;
		for(unsigned int attempt = 0; attempt < FEEDBACK_TEST_ATTEMPTS && !at_minimum; attempt++)
			at_minimum = feedback_test_stick_after_state(&rig, delay_ms, attempt);
		munit_assert_true(at_minimum);
	}

	feedback_test_rig_stop(&rig);
	return MUNIT_OK;
}

// PLE-823: the shape the defect hurt: a stick movement whose last samples, the stick easing
// back to centre, come inside the state minimum. One state packet goes out as the minimum ends,
// carrying the last sample, and nothing follows it until the keepalive.
static MunitResult test_last_stick_sample_inside_state_minimum_sent_at_minimum(const MunitParameter params[], void *user)
{
	(void)params;
	(void)user;
	FeedbackTestRig rig;
	feedback_test_rig_start(&rig);

	static const int16_t samples[] = { 0x0c00, 0x0600, 0 };
	const size_t samples_count = sizeof(samples) / sizeof(samples[0]);
	bool at_minimum = false;
	for(unsigned int attempt = 0; attempt < FEEDBACK_TEST_ATTEMPTS && !at_minimum; attempt++)
	{
		uint64_t last_move_us;
		FeedbackTestPacket state = feedback_test_state_packet(&rig, &last_move_us);
		uint64_t move_us = 0;
		for(size_t i = 0; i < samples_count; i++)
			move_us = feedback_test_move_stick_at(&rig, state.at_us + (1 + 2 * i) * 1000, samples[i]);

		// If the host delayed a sample past the minimum, the sample before it went out as the
		// minimum ended and the last one follows a minimum later: at most one packet per sample.
		FeedbackTestPacket first, next;
		unsigned int packets = 0;
		do
		{
			munit_assert_true(feedback_test_recv(&rig, 1000, &next));
			munit_assert_uint8(next.type, ==, FEEDBACK_TEST_PACKET_STATE);
			if(!packets)
				first = next;
			packets++;
			munit_assert_uint(packets, <=, samples_count);
		} while(next.left_x != samples[samples_count - 1]);
		FeedbackTestPacket extra;
		munit_assert_false(feedback_test_recv(&rig, 2 * FEEDBACK_TEST_STATE_MIN_MS, &extra));

		uint64_t last_sample_us = move_us - state.at_us;
		uint64_t latency_us = next.at_us - move_us;
		uint64_t gap_us = next.at_us - state.at_us;
		munit_logf(MUNIT_LOG_INFO, "last of %zu stick samples %llu us after a state packet (try %u): %u state packets, the last %llu us after the sample, %llu us after the state packet",
			samples_count, (unsigned long long)last_sample_us, attempt, packets, (unsigned long long)latency_us, (unsigned long long)gap_us);
		// Before PLE-823 the last sample waited for the keepalive, FEEDBACK_TEST_KEEPALIVE_MS
		// after the state packet.
		munit_assert_uint64(latency_us, <, FEEDBACK_TEST_NEVER_US);
		// The minimum still holds; see feedback_test_stick_after_state().
		munit_assert_uint64(first.at_us - last_move_us, >=, (FEEDBACK_TEST_STATE_MIN_MS - 1) * 1000);

		if(packets > 1 || last_sample_us >= FEEDBACK_TEST_STATE_MIN_MS * 1000)
			continue; // the host delayed a sample past the minimum
		at_minimum = gap_us < FEEDBACK_TEST_STATE_MIN_MS * 1000 + FEEDBACK_TEST_AT_MINIMUM_SLACK_US;
	}
	munit_assert_true(at_minimum);

	feedback_test_rig_stop(&rig);
	return MUNIT_OK;
}
#endif

MunitTest tests_feedback_sender[] = {
#if defined(__linux__)
	{
		"/history_sent_at_once_inside_state_minimum",
		test_history_sent_at_once_inside_state_minimum,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{
		"/state_minimum_holds_for_a_stick_moved_with_a_press",
		test_state_minimum_holds_for_a_stick_moved_with_a_press,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{
		"/stick_change_inside_state_minimum_sent_at_minimum",
		test_stick_change_inside_state_minimum_sent_at_minimum,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
	{
		"/last_stick_sample_inside_state_minimum_sent_at_minimum",
		test_last_stick_sample_inside_state_minimum_sent_at_minimum,
		NULL,
		NULL,
		MUNIT_TEST_OPTION_NONE,
		NULL
	},
#endif
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
