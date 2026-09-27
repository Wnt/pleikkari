// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <munit.h>

#include "../android/app/src/main/cpp/video-frame-latency.h"

#include <string.h>

#define MS(x) ((int64_t)(x) * 1000000LL)

static AndroidChiakiVideoFrameLatency *new_latency(bool enabled)
{
	AndroidChiakiVideoFrameLatency *latency = munit_new(AndroidChiakiVideoFrameLatency);
	munit_assert_int(android_chiaki_video_frame_latency_init(latency), ==, CHIAKI_ERR_SUCCESS);
	if(enabled)
		android_chiaki_video_frame_latency_set_enabled(latency, true, MS(1000));
	return latency;
}

static void free_latency(AndroidChiakiVideoFrameLatency *latency)
{
	android_chiaki_video_frame_latency_fini(latency);
	free(latency);
}

static const AndroidChiakiVideoFrameLatencyStat *figure(const AndroidChiakiVideoFrameLatencyWindow *window,
		AndroidChiakiVideoFrameLatencyFigure which)
{
	return &window->figures[which];
}

static MunitResult test_stages_of_one_frame(const MunitParameter params[], void *user)
{
	(void)params;
	(void)user;
	AndroidChiakiVideoFrameLatency *latency = new_latency(true);
	// arrival 1000, decoded 1009, latched 1015, submitted 1018, predicted photon 1048 (ms)
	android_chiaki_video_frame_latency_record_decoded(latency, 17000, MS(1000), MS(1009));
	android_chiaki_video_frame_latency_record_latched(latency, 17000, MS(1015), MS(1018), MS(1048));

	AndroidChiakiVideoFrameLatencyWindow window;
	munit_assert_true(android_chiaki_video_frame_latency_take_window(latency, MS(2000), &window));
	munit_assert_int64(window.window_ns, ==, MS(1000));
	munit_assert_uint32(window.decoded, ==, 1);
	munit_assert_uint32(window.latched, ==, 1);
	munit_assert_uint32(window.shown, ==, 1);
	munit_assert_uint32(window.replaced, ==, 0);
	munit_assert_uint32(window.unmatched, ==, 0);
	munit_assert_int64(figure(&window, ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_ARRIVAL_DECODED)->p50_ns, ==, MS(9));
	munit_assert_int64(figure(&window, ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_DECODED_LATCHED)->p50_ns, ==, MS(6));
	munit_assert_int64(figure(&window, ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_LATCHED_SUBMITTED)->p50_ns, ==, MS(3));
	munit_assert_int64(figure(&window, ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_SUBMITTED_PHOTON)->p50_ns, ==, MS(30));
	munit_assert_int64(figure(&window, ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_TOTAL)->p50_ns, ==, MS(48));
	for(int i = 0; i < ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_FIGURES; i++)
	{
		munit_assert_uint32(window.figures[i].count, ==, 1);
		munit_assert_int64(window.figures[i].p50_ns, ==, window.figures[i].max_ns);
		munit_assert_int64(window.figures[i].p50_ns, ==, window.figures[i].mean_ns);
	}

	// The next window starts empty.
	munit_assert_true(android_chiaki_video_frame_latency_take_window(latency, MS(3000), &window));
	munit_assert_uint32(window.decoded, ==, 0);
	munit_assert_uint32(window.figures[ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_TOTAL].count, ==, 0);
	free_latency(latency);
	return MUNIT_OK;
}

static MunitResult test_replaced_before_latch(const MunitParameter params[], void *user)
{
	(void)params;
	(void)user;
	AndroidChiakiVideoFrameLatency *latency = new_latency(true);
	android_chiaki_video_frame_latency_record_decoded(latency, 1000, MS(1000), MS(1008));
	android_chiaki_video_frame_latency_record_decoded(latency, 2000, MS(1016), MS(1024));
	android_chiaki_video_frame_latency_record_decoded(latency, 3000, MS(1033), MS(1041));
	// The third frame is latched first: the two ahead of it were never shown.
	android_chiaki_video_frame_latency_record_latched(latency, 3000, MS(1043), MS(1045), MS(1075));
	// A late latch of an already passed-over frame no longer matches.
	android_chiaki_video_frame_latency_record_latched(latency, 1000, MS(1057), MS(1059), MS(1089));

	AndroidChiakiVideoFrameLatencyWindow window;
	munit_assert_true(android_chiaki_video_frame_latency_take_window(latency, MS(2000), &window));
	munit_assert_uint32(window.decoded, ==, 3);
	munit_assert_uint32(window.latched, ==, 2);
	munit_assert_uint32(window.shown, ==, 2);
	munit_assert_uint32(window.replaced, ==, 2);
	munit_assert_uint32(window.unmatched, ==, 1);
	// Decode-side figures only for the matched frame; the cinema-side ones for both latches.
	munit_assert_uint32(figure(&window, ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_ARRIVAL_DECODED)->count, ==, 1);
	munit_assert_uint32(figure(&window, ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_DECODED_LATCHED)->count, ==, 1);
	munit_assert_int64(figure(&window, ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_DECODED_LATCHED)->p50_ns, ==, MS(2));
	munit_assert_uint32(figure(&window, ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_LATCHED_SUBMITTED)->count, ==, 2);
	munit_assert_uint32(figure(&window, ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_SUBMITTED_PHOTON)->count, ==, 2);
	munit_assert_uint32(figure(&window, ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_TOTAL)->count, ==, 1);
	munit_assert_int64(figure(&window, ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_TOTAL)->p50_ns, ==, MS(42));
	free_latency(latency);
	return MUNIT_OK;
}

static MunitResult test_hidden_and_unknown_arrival(const MunitParameter params[], void *user)
{
	(void)params;
	(void)user;
	AndroidChiakiVideoFrameLatency *latency = new_latency(true);
	// Latched while the menu was up: counted as latched, not shown, no figures.
	android_chiaki_video_frame_latency_record_decoded(latency, 1000, MS(1000), MS(1008));
	android_chiaki_video_frame_latency_record_latched(latency, 1000, MS(1010), 0, 0);
	// No input metadata for this one: no arrival-based figures.
	android_chiaki_video_frame_latency_record_decoded(latency, 2000, 0, MS(1024));
	android_chiaki_video_frame_latency_record_latched(latency, 2000, MS(1027), MS(1030), MS(1060));

	AndroidChiakiVideoFrameLatencyWindow window;
	munit_assert_true(android_chiaki_video_frame_latency_take_window(latency, MS(2000), &window));
	munit_assert_uint32(window.latched, ==, 2);
	munit_assert_uint32(window.shown, ==, 1);
	munit_assert_uint32(window.replaced, ==, 0);
	munit_assert_uint32(figure(&window, ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_ARRIVAL_DECODED)->count, ==, 0);
	munit_assert_uint32(figure(&window, ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_TOTAL)->count, ==, 0);
	munit_assert_int64(figure(&window, ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_DECODED_LATCHED)->p50_ns, ==, MS(3));
	free_latency(latency);
	return MUNIT_OK;
}

static MunitResult test_disabled_and_reenabled(const MunitParameter params[], void *user)
{
	(void)params;
	(void)user;
	AndroidChiakiVideoFrameLatency *latency = new_latency(false);
	AndroidChiakiVideoFrameLatencyWindow window;
	android_chiaki_video_frame_latency_record_decoded(latency, 1000, MS(1000), MS(1008));
	android_chiaki_video_frame_latency_record_latched(latency, 1000, MS(1010), MS(1012), MS(1040));
	munit_assert_false(android_chiaki_video_frame_latency_take_window(latency, MS(2000), &window));

	android_chiaki_video_frame_latency_set_enabled(latency, true, MS(3000));
	android_chiaki_video_frame_latency_record_decoded(latency, 2000, MS(3000), MS(3008));
	// A new cinema starts over: its first latch cannot match the previous one's pending frames.
	android_chiaki_video_frame_latency_set_enabled(latency, true, MS(4000));
	android_chiaki_video_frame_latency_record_latched(latency, 2000, MS(4010), MS(4012), MS(4040));
	munit_assert_true(android_chiaki_video_frame_latency_take_window(latency, MS(5000), &window));
	munit_assert_int64(window.window_ns, ==, MS(1000));
	munit_assert_uint32(window.decoded, ==, 0);
	munit_assert_uint32(window.unmatched, ==, 1);

	android_chiaki_video_frame_latency_set_enabled(latency, false, MS(6000));
	munit_assert_false(android_chiaki_video_frame_latency_take_window(latency, MS(7000), &window));
	free_latency(latency);
	return MUNIT_OK;
}

static MunitResult test_pending_overflow_counts_replaced(const MunitParameter params[], void *user)
{
	(void)params;
	(void)user;
	AndroidChiakiVideoFrameLatency *latency = new_latency(true);
	for(int i = 0; i < ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_PENDING + 3; i++)
		android_chiaki_video_frame_latency_record_decoded(latency, 1000 * (i + 1), MS(1000 + i), MS(1008 + i));
	// The newest frame is still pending and matches; everything else was never shown.
	android_chiaki_video_frame_latency_record_latched(latency, 1000 * (ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_PENDING + 3),
			MS(1100), MS(1102), MS(1130));
	AndroidChiakiVideoFrameLatencyWindow window;
	munit_assert_true(android_chiaki_video_frame_latency_take_window(latency, MS(2000), &window));
	munit_assert_uint32(window.decoded, ==, ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_PENDING + 3);
	munit_assert_uint32(window.replaced, ==, ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_PENDING + 2);
	munit_assert_uint32(window.unmatched, ==, 0);
	free_latency(latency);
	return MUNIT_OK;
}

static MunitResult test_percentiles_nearest_rank(const MunitParameter params[], void *user)
{
	(void)params;
	(void)user;
	AndroidChiakiVideoFrameLatency *latency = new_latency(true);
	// 100 frames whose decoded->latched is 100, 99, ..., 1 ms; more than the kept samples would
	// not change mean and max, which see every frame.
	for(int i = 0; i < 100; i++)
	{
		int64_t ts = 1000 * (i + 1);
		android_chiaki_video_frame_latency_record_decoded(latency, ts, MS(1000), MS(1000));
		android_chiaki_video_frame_latency_record_latched(latency, ts, MS(1100 - i), MS(1101 - i), MS(1131 - i));
	}
	AndroidChiakiVideoFrameLatencyWindow window;
	munit_assert_true(android_chiaki_video_frame_latency_take_window(latency, MS(2000), &window));
	const AndroidChiakiVideoFrameLatencyStat *d2l = figure(&window, ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_DECODED_LATCHED);
	munit_assert_uint32(d2l->count, ==, 100);
	munit_assert_int64(d2l->p50_ns, ==, MS(50));
	munit_assert_int64(d2l->p95_ns, ==, MS(95));
	munit_assert_int64(d2l->max_ns, ==, MS(100));
	munit_assert_int64(d2l->mean_ns, ==, MS(50) + MS(1) / 2);

	for(int i = 0; i < ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_SAMPLES + 44; i++)
	{
		int64_t ts = 1000000 + i;
		android_chiaki_video_frame_latency_record_decoded(latency, ts, MS(3000), MS(3000));
		android_chiaki_video_frame_latency_record_latched(latency, ts, MS(3000) + (i == 280 ? MS(70) : MS(5)),
				MS(3080), MS(3110));
	}
	munit_assert_true(android_chiaki_video_frame_latency_take_window(latency, MS(4000), &window));
	d2l = figure(&window, ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_DECODED_LATCHED);
	munit_assert_uint32(d2l->count, ==, ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_SAMPLES + 44);
	munit_assert_int64(d2l->p95_ns, ==, MS(5));
	munit_assert_int64(d2l->max_ns, ==, MS(70)); // past the kept samples, still the max
	free_latency(latency);
	return MUNIT_OK;
}

static MunitResult test_format_line(const MunitParameter params[], void *user)
{
	(void)params;
	(void)user;
	AndroidChiakiVideoFrameLatencyWindow window;
	memset(&window, 0, sizeof(window));
	window.window_ns = MS(1001) + 400000;
	window.decoded = 60;
	window.latched = 58;
	window.shown = 58;
	window.replaced = 2;
	window.unmatched = 0;
	const int64_t values[ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_FIGURES][4] = {
		{ 9100000, 12040000, 14230000, 9420000 },
		{ 5200000, 13100000, 13900000, 6000000 },
		{ 3100000, 3500000, 4000000, 3200000 },
		{ 30100000, 31000000, 31500000, 30200000 },
		{ 47800000, 58200000, 60100000, 48900000 },
	};
	for(int i = 0; i < ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_FIGURES; i++)
	{
		window.figures[i].count = 58;
		window.figures[i].p50_ns = values[i][0];
		window.figures[i].p95_ns = values[i][1];
		window.figures[i].max_ns = values[i][2];
		window.figures[i].mean_ns = values[i][3];
	}
	window.figures[ANDROID_CHIAKI_VIDEO_FRAME_LATENCY_TOTAL].count = 0;
	char line[512];
	android_chiaki_video_frame_latency_format(&window, line, sizeof(line));
	// scripts/dev/go-latency/go_stats.py parses exactly this.
	munit_assert_string_equal(line,
			"Cinema latency: window 1001 ms decoded 60 latched 58 shown 58 replaced 2 unmatched 0"
			" | p50/p95/max/mean ms arrival_decoded 9.1/12.0/14.2/9.4 decoded_latched 5.2/13.1/13.9/6.0"
			" latched_submitted 3.1/3.5/4.0/3.2 submitted_photon 30.1/31.0/31.5/30.2 total n/a");

	// A short buffer truncates instead of overrunning.
	char small[40];
	memset(small, 'x', sizeof(small));
	android_chiaki_video_frame_latency_format(&window, small, 32);
	munit_assert_size(strlen(small), ==, 31);
	munit_assert_char(small[33], ==, 'x');
	return MUNIT_OK;
}

MunitTest tests_video_frame_latency[] = {
	{ "/stages_of_one_frame", test_stages_of_one_frame, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/replaced_before_latch", test_replaced_before_latch, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/hidden_and_unknown_arrival", test_hidden_and_unknown_arrival, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/disabled_and_reenabled", test_disabled_and_reenabled, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/pending_overflow_counts_replaced", test_pending_overflow_counts_replaced, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/percentiles_nearest_rank", test_percentiles_nearest_rank, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/format_line", test_format_line, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
};
