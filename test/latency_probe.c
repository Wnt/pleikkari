// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <munit.h>

#include "../android/app/src/main/cpp/latency-probe.h"

#include <chiaki/controller.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MS(x) ((int64_t)(x) * 1000000LL)
#define CROSS CHIAKI_CONTROLLER_BUTTON_CROSS

typedef struct probe_files_t
{
	char *presses;
	size_t presses_size;
	char *frames;
	size_t frames_size;
} ProbeFiles;

static AndroidChiakiLatencyProbe *start_probe(ProbeFiles *files)
{
	memset(files, 0, sizeof(*files));
	AndroidChiakiLatencyProbe *probe = munit_new(AndroidChiakiLatencyProbe);
	munit_assert_int(android_chiaki_latency_probe_init(probe), ==, CHIAKI_ERR_SUCCESS);
	FILE *presses = open_memstream(&files->presses, &files->presses_size);
	FILE *frames = open_memstream(&files->frames, &files->frames_size);
	munit_assert_true(android_chiaki_latency_probe_start(probe, presses, frames));
	munit_assert_true(android_chiaki_latency_probe_enabled(probe));
	return probe;
}

/** Stops the probe (closing both streams, so their buffers are final) and frees it. */
static void stop_probe(AndroidChiakiLatencyProbe *probe, uint32_t expect_presses, uint32_t expect_frames)
{
	uint32_t presses = 0, frames = 0;
	munit_assert_true(android_chiaki_latency_probe_stop(probe, &presses, &frames));
	munit_assert_uint32(presses, ==, expect_presses);
	munit_assert_uint32(frames, ==, expect_frames);
	munit_assert_false(android_chiaki_latency_probe_stop(probe, NULL, NULL));
	android_chiaki_latency_probe_fini(probe);
	free(probe);
}

static void free_files(ProbeFiles *files)
{
	free(files->presses);
	free(files->frames);
}

static MunitResult test_press_from_key_event_to_packet(const MunitParameter params[], void *user)
{
	(void)params;
	(void)user;
	ProbeFiles files;
	AndroidChiakiLatencyProbe *probe = start_probe(&files);
	// Sticks moving before the press send packets without Cross: none completes anything.
	android_chiaki_latency_probe_history_sent(probe, 0, 3, MS(990));
	android_chiaki_latency_probe_press(probe, CROSS, MS(1000), MS(1002));
	android_chiaki_latency_probe_history_sent(probe, 0, 4, MS(1003)); // still the old state
	android_chiaki_latency_probe_controller_state(probe, CROSS, MS(1004));
	android_chiaki_latency_probe_history_sent(probe, CROSS, 5, MS(1006));
	// The history ring resends the edge in later packets, and the release follows: no second row.
	android_chiaki_latency_probe_history_sent(probe, CROSS, 6, MS(1010));
	android_chiaki_latency_probe_controller_state(probe, 0, MS(1060));
	android_chiaki_latency_probe_history_sent(probe, 0, 7, MS(1062));
	stop_probe(probe, 1, 0);
	munit_assert_string_equal(files.presses, ANDROID_CHIAKI_LATENCY_PROBE_PRESSES_HEADER
			"1,1000000000,1002000000,1004000000,1006000000,5\n");
	munit_assert_string_equal(files.frames, ANDROID_CHIAKI_LATENCY_PROBE_FRAMES_HEADER);
	free_files(&files);
	return MUNIT_OK;
}

static MunitResult test_press_without_key_event_and_unsent(const MunitParameter params[], void *user)
{
	(void)params;
	(void)user;
	ProbeFiles files;
	AndroidChiakiLatencyProbe *probe = start_probe(&files);
	// A Cross the probe saw no KeyEvent for (the debug pad broadcast) still times state to packet.
	android_chiaki_latency_probe_controller_state(probe, CROSS, MS(2000));
	android_chiaki_latency_probe_history_sent(probe, CROSS, 9, MS(2001));
	android_chiaki_latency_probe_controller_state(probe, 0, MS(2100));
	// A KeyEvent whose state never came is written unsent when the next press arrives.
	android_chiaki_latency_probe_press(probe, CROSS, MS(3000), MS(3001));
	android_chiaki_latency_probe_press(probe, CROSS, MS(4000), MS(4002));
	android_chiaki_latency_probe_controller_state(probe, CROSS, MS(4003));
	// Cross held while another button changes is not a new press.
	android_chiaki_latency_probe_controller_state(probe, CROSS | CHIAKI_CONTROLLER_BUTTON_MOON, MS(4004));
	android_chiaki_latency_probe_history_sent(probe, CROSS | CHIAKI_CONTROLLER_BUTTON_MOON, 10, MS(4005));
	// A press still on its way when the probe stops is written unsent.
	android_chiaki_latency_probe_controller_state(probe, 0, MS(4100));
	android_chiaki_latency_probe_press(probe, CROSS, MS(5000), MS(5001));
	stop_probe(probe, 4, 0);
	munit_assert_string_equal(files.presses, ANDROID_CHIAKI_LATENCY_PROBE_PRESSES_HEADER
			"1,0,0,2000000000,2001000000,9\n"
			"2,3000000000,3001000000,0,0,-1\n"
			"3,4000000000,4002000000,4003000000,4005000000,10\n"
			"4,5000000000,5001000000,0,0,-1\n");
	free_files(&files);
	return MUNIT_OK;
}

static MunitResult test_frame_rows_from_the_join(const MunitParameter params[], void *user)
{
	(void)params;
	(void)user;
	ProbeFiles files;
	AndroidChiakiLatencyProbe *probe = start_probe(&files);
	AndroidChiakiVideoFrameLatency *latency = munit_new(AndroidChiakiVideoFrameLatency);
	munit_assert_int(android_chiaki_video_frame_latency_init(latency), ==, CHIAKI_ERR_SUCCESS);
	android_chiaki_video_frame_latency_set_enabled(latency, true, MS(1000));
	android_chiaki_video_frame_latency_set_row_cb(latency, android_chiaki_latency_probe_frame_row, probe);
	// frame 40: ready 1000, queued 1001, decoded 1009; 41 is replaced by 42 before any latch.
	android_chiaki_video_frame_latency_record_decoded(latency, 1000, 40, MS(1000), MS(1001), MS(1009));
	android_chiaki_video_frame_latency_record_latched(latency, 1000, MS(1015), MS(1018), MS(1048), 17);
	android_chiaki_video_frame_latency_record_decoded(latency, 2000, 41, MS(1016), MS(1017), MS(1025));
	android_chiaki_video_frame_latency_record_decoded(latency, 3000, 42, MS(1033), MS(1034), MS(1041));
	// Latched while the menu hid the video: no submit, luma still measured.
	android_chiaki_video_frame_latency_record_latched(latency, 3000, MS(1043), 0, 0, 230);
	// A latch the presenter never rendered.
	android_chiaki_video_frame_latency_record_latched(latency, 9000, MS(1060), MS(1062), MS(1092), -1);
	// Removing the callback stops the rows; the stats keep counting.
	android_chiaki_video_frame_latency_set_row_cb(latency, NULL, NULL);
	android_chiaki_video_frame_latency_record_decoded(latency, 4000, 43, MS(1050), MS(1051), MS(1058));
	android_chiaki_video_frame_latency_record_latched(latency, 4000, MS(1070), MS(1072), MS(1100), 20);
	AndroidChiakiVideoFrameLatencyWindow window;
	munit_assert_true(android_chiaki_video_frame_latency_take_window(latency, MS(2000), &window));
	munit_assert_uint32(window.latched, ==, 4);
	munit_assert_uint32(window.replaced, ==, 1);
	munit_assert_uint32(window.unmatched, ==, 1);
	android_chiaki_video_frame_latency_fini(latency);
	free(latency);
	stop_probe(probe, 0, 4);
	munit_assert_string_equal(files.frames, ANDROID_CHIAKI_LATENCY_PROBE_FRAMES_HEADER
			"40,1000,latched,1000000000,1001000000,1009000000,1015000000,1018000000,1048000000,17\n"
			"41,2000,replaced,1016000000,1017000000,1025000000,0,0,0,-1\n"
			"42,3000,latched,1033000000,1034000000,1041000000,1043000000,0,0,230\n"
			"-1,9000,unmatched,0,0,0,1060000000,1062000000,1092000000,-1\n");
	free_files(&files);
	return MUNIT_OK;
}

static MunitResult test_not_started(const MunitParameter params[], void *user)
{
	(void)params;
	(void)user;
	AndroidChiakiLatencyProbe probe;
	munit_assert_int(android_chiaki_latency_probe_init(&probe), ==, CHIAKI_ERR_SUCCESS);
	// Everything is a no-op before a start, and a start without both files records nothing.
	android_chiaki_latency_probe_press(&probe, CROSS, MS(1), MS(2));
	android_chiaki_latency_probe_controller_state(&probe, CROSS, MS(3));
	android_chiaki_latency_probe_history_sent(&probe, CROSS, 1, MS(4));
	char *buf = NULL;
	size_t size = 0;
	munit_assert_false(android_chiaki_latency_probe_start(&probe, open_memstream(&buf, &size), NULL));
	munit_assert_false(android_chiaki_latency_probe_enabled(&probe));
	munit_assert_size(size, ==, 0);
	free(buf);
	munit_assert_false(android_chiaki_latency_probe_stop(&probe, NULL, NULL));
	android_chiaki_latency_probe_fini(&probe);
	return MUNIT_OK;
}

static MunitResult test_press_buttons_mask(const MunitParameter params[], void *user)
{
	(void)params;
	(void)user;
	ProbeFiles files;
	AndroidChiakiLatencyProbe *probe = start_probe(&files);
	// PLE-803: another stimulus button (Create) opens presses; Cross then does not, and 0 changes nothing.
	android_chiaki_latency_probe_set_press_buttons(probe, CHIAKI_CONTROLLER_BUTTON_SHARE);
	android_chiaki_latency_probe_set_press_buttons(probe, 0);
	android_chiaki_latency_probe_controller_state(probe, CROSS, MS(1000));
	android_chiaki_latency_probe_history_sent(probe, CROSS, 1, MS(1001));
	android_chiaki_latency_probe_controller_state(probe, CHIAKI_CONTROLLER_BUTTON_SHARE, MS(2000));
	android_chiaki_latency_probe_history_sent(probe, CHIAKI_CONTROLLER_BUTTON_SHARE, 2, MS(2002));
	android_chiaki_latency_probe_controller_state(probe, 0, MS(2100));
	// PLE-829: the mask decides for KeyEvents too. R3 (the phone's stimulus) does not open a press here,
	// and nothing it could complete is left pending; Create's KeyEvent does.
	munit_assert_false(android_chiaki_latency_probe_press(probe, CHIAKI_CONTROLLER_BUTTON_R3, MS(2999), MS(3000)));
	android_chiaki_latency_probe_controller_state(probe, CHIAKI_CONTROLLER_BUTTON_R3, MS(3001));
	android_chiaki_latency_probe_history_sent(probe, CHIAKI_CONTROLLER_BUTTON_R3, 3, MS(3002));
	android_chiaki_latency_probe_controller_state(probe, 0, MS(3100));
	munit_assert_true(android_chiaki_latency_probe_press(probe, CHIAKI_CONTROLLER_BUTTON_SHARE, MS(4000), MS(4001)));
	android_chiaki_latency_probe_controller_state(probe, CHIAKI_CONTROLLER_BUTTON_SHARE, MS(4002));
	android_chiaki_latency_probe_history_sent(probe, CHIAKI_CONTROLLER_BUTTON_SHARE, 4, MS(4004));
	stop_probe(probe, 2, 0);
	munit_assert_string_equal(files.presses, ANDROID_CHIAKI_LATENCY_PROBE_PRESSES_HEADER
			"1,0,0,2000000000,2002000000,2\n"
			"2,4000000000,4001000000,4002000000,4004000000,4\n");
	free_files(&files);
	return MUNIT_OK;
}

MunitTest tests_latency_probe[] = {
	{ "/press_buttons_mask", test_press_buttons_mask, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/press_from_key_event_to_packet", test_press_from_key_event_to_packet, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/press_without_key_event_and_unsent", test_press_without_key_event_and_unsent, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/frame_rows_from_the_join", test_frame_rows_from_the_join, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/not_started", test_not_started, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
};
