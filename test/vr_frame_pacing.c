// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <munit.h>

#include "../android/app/src/main/cpp/vr-frame-pacing.h"

#define MS 1000000LL
#define PERIOD_72 13888889LL

// One frame the way the cinema records it: started at start, submitted work later, returned
// after wait, displayed at predicted.
static void frame(PleikkariVrPacing *pacing, int64_t start, int64_t work, int64_t wait, int64_t predicted)
{
	pleikkari_vr_pacing_frame(pacing, start, 0, start + work, start + work + wait, predicted);
}

static MunitResult test_config(const MunitParameter params[], void *user)
{
	(void)params;
	(void)user;
	PleikkariVrPacingConfig config;
	pleikkari_vr_pacing_config_default(&config, 72.0f);
	munit_assert_int(config.mode, ==, PLEIKKARI_VR_PACING_VRAPI);
	munit_assert_int64(config.period_ns, ==, PERIOD_72);
	munit_assert_int64(config.budget_ns, ==, PLEIKKARI_VR_PACING_DEFAULT_BUDGET_NS);
	munit_assert_false(config.trace);
	pleikkari_vr_pacing_config_default(&config, 60.0f);
	munit_assert_int64(config.period_ns, ==, 16666667LL);

	munit_assert_int(pleikkari_vr_pacing_config_parse(&config,
		"trace,late,budget=4000,sleep=1500,sweep=500/144/13000,stall=600:20000;1200:30000,signal,bogus"), ==, 7);
	munit_assert_true(config.trace);
	munit_assert_true(config.latch_on_signal);
	munit_assert_int(config.mode, ==, PLEIKKARI_VR_PACING_LATE);
	munit_assert_int64(config.budget_ns, ==, 4 * MS);
	munit_assert_int64(config.sleep_ns, ==, 1500000LL);
	munit_assert_int64(config.sweep_step_ns, ==, 500000LL);
	munit_assert_uint32(config.sweep_frames, ==, 144);
	munit_assert_int64(config.sweep_max_ns, ==, 13 * MS);
	munit_assert_uint32(config.stalls, ==, 2);
	munit_assert_uint64(config.stall_frame[1], ==, 1200);
	munit_assert_int64(config.stall_ns[1], ==, 30 * MS);

	// Empty, missing or malformed items leave the defaults.
	pleikkari_vr_pacing_config_default(&config, 72.0f);
	munit_assert_int(pleikkari_vr_pacing_config_parse(&config, ""), ==, 0);
	munit_assert_int(pleikkari_vr_pacing_config_parse(&config, NULL), ==, 0);
	munit_assert_int(pleikkari_vr_pacing_config_parse(&config, "budget=0,sweep=500,stall=x"), ==, 1);
	munit_assert_int64(config.budget_ns, ==, PLEIKKARI_VR_PACING_DEFAULT_BUDGET_NS);
	munit_assert_uint32(config.sweep_frames, ==, 0);
	munit_assert_uint32(config.stalls, ==, 0);
	munit_assert_int(config.mode, ==, PLEIKKARI_VR_PACING_VRAPI);
	return MUNIT_OK;
}

static MunitResult test_vrapi_never_sleeps(const MunitParameter params[], void *user)
{
	(void)params;
	(void)user;
	// The default is today's loop: every frame starts as soon as the last submit returned.
	PleikkariVrPacingConfig config;
	pleikkari_vr_pacing_config_default(&config, 72.0f);
	PleikkariVrPacing pacing;
	pleikkari_vr_pacing_init(&pacing, &config);
	int64_t t = 1000 * MS;
	for(int i = 0; i < 100; i++)
	{
		munit_assert_int64(pleikkari_vr_pacing_wake_ns(&pacing, t), ==, 0);
		frame(&pacing, t, 2 * MS, PERIOD_72 - 2 * MS, t + 46 * MS);
		t += PERIOD_72;
	}
	return MUNIT_OK;
}

// The predicted display time sits (lead + 1.38) refreshes after the release it was submitted for.
static int64_t lead0(int64_t period)
{
	return (int64_t)(PLEIKKARI_VR_PACING_LEAD0_REFRESHES * (double)period + 0.5);
}

static MunitResult test_late_start(const MunitParameter params[], void *user)
{
	(void)params;
	(void)user;
	PleikkariVrPacingConfig config;
	pleikkari_vr_pacing_config_default(&config, 72.0f);
	config.mode = PLEIKKARI_VR_PACING_LATE;
	PleikkariVrPacing pacing;
	pleikkari_vr_pacing_init(&pacing, &config);
	const int64_t t = 1000 * MS;
	const int64_t budget = PLEIKKARI_VR_PACING_DEFAULT_BUDGET_NS;
	// Nothing to key on before the first frame.
	munit_assert_int64(pleikkari_vr_pacing_next_release_ns(&pacing, t), ==, 0);
	munit_assert_int64(pleikkari_vr_pacing_wake_ns(&pacing, t), ==, 0);
	// VrApi held the submit 10.7 ms, to its release; the frame is shown 1.38 refreshes later
	// (lead 0), and the next frame starts the budget before the next release.
	const int64_t release = t + 13700000LL;
	frame(&pacing, t, 3 * MS, 10700000LL, release + lead0(PERIOD_72));
	munit_assert_true(pacing.throttled);
	munit_assert_int(pacing.lead, ==, 0);
	munit_assert_int64(pleikkari_vr_pacing_next_release_ns(&pacing, release + 100000LL), ==, release + PERIOD_72);
	munit_assert_int64(pleikkari_vr_pacing_wake_ns(&pacing, release + 100000LL), ==, release + PERIOD_72 - budget);
	// A start already inside the budget goes at once while 6 ms or more are left.
	munit_assert_int64(pleikkari_vr_pacing_wake_ns(&pacing, release + PERIOD_72 - 7 * MS), ==, 0);
	// A frame that came after its release was let through after VrApi's 2 ms minimum. Its
	// predicted display is still on the grid, so the next one aims at the release after it.
	const int64_t late = release + PERIOD_72 - 1 * MS;
	const int64_t late_predicted = release + 2 * PERIOD_72 + lead0(PERIOD_72);
	frame(&pacing, late, 3 * MS, 2 * MS, late_predicted);
	munit_assert_false(pacing.throttled);
	munit_assert_int(pacing.lead, ==, 0);
	munit_assert_int64(pleikkari_vr_pacing_wake_ns(&pacing, late + 5 * MS + 1000), ==, release + 2 * PERIOD_72 - budget);
	PleikkariVrPacingWindow window;
	pleikkari_vr_pacing_take_window(&pacing, &window);
	munit_assert_uint32(window.frames, ==, 2);
	munit_assert_uint32(window.throttled, ==, 1);
	munit_assert_uint32(window.late, ==, 1);
	munit_assert_uint32(window.leads[0], ==, 2);
	munit_assert_uint32(window.leads[1], ==, 0);
	munit_assert_uint32(window.drains, ==, 0);
	munit_assert_int64(window.wait_min_ns, ==, 2 * MS);
	munit_assert_int64(window.wait_max_ns, ==, 10700000LL);
	munit_assert_int64(window.work_max_ns, ==, 3 * MS);
	munit_assert_int64(window.ahead_min_ns, ==, release + lead0(PERIOD_72) - (t + 3 * MS));
	munit_assert_int64(window.ahead_max_ns, ==, late_predicted - (late + 3 * MS));
	munit_assert_int64(window.lead_min_ns, ==, lead0(PERIOD_72));
	munit_assert_int64(window.lead_max_ns, ==, late_predicted - (late + 5 * MS));
	// The next window starts empty.
	pleikkari_vr_pacing_take_window(&pacing, &window);
	munit_assert_uint32(window.frames, ==, 0);
	munit_assert_int64(window.wait_min_ns, ==, 0);
	munit_assert_string_equal(pleikkari_vr_pacing_mode_name(PLEIKKARI_VR_PACING_LATE), "late start");
	return MUNIT_OK;
}

// The Go's real refresh at "72 Hz" is 13.924 ms; the grid follows the predicted display times.
#define REAL_72 13924000LL

// Frames VrApi throttles, released one refresh apart from [release], each shown [lead] refreshes
// early; returns the last release.
static int64_t throttled_frames(PleikkariVrPacing *pacing, int64_t release, int count, int lead)
{
	for(int i = 0; i < count; i++)
	{
		const int64_t start = release + 50000LL;
		release += REAL_72;
		frame(pacing, start, 3 * MS, release - start - 3 * MS, release + lead * REAL_72 + lead0(REAL_72));
	}
	return release;
}

static MunitResult test_hold_and_period(const MunitParameter params[], void *user)
{
	(void)params;
	(void)user;
	const int64_t budget = PLEIKKARI_VR_PACING_DEFAULT_BUDGET_NS;
	PleikkariVrPacingConfig config;
	pleikkari_vr_pacing_config_default(&config, 72.0f);
	munit_assert_int(pleikkari_vr_pacing_config_parse(&config, "hold"), ==, 1);
	munit_assert_true(config.hold);
	PleikkariVrPacing pacing;
	pleikkari_vr_pacing_init(&pacing, &config);
	// VRAPI mode with hold: throttled frames start at once, and the period is learnt.
	munit_assert_int64(pleikkari_vr_pacing_wake_ns(&pacing, 2000 * MS), ==, 0);
	const int64_t release = throttled_frames(&pacing, 2000 * MS, 400, 1);
	munit_assert_int64(pleikkari_vr_pacing_wake_ns(&pacing, release + 50000LL), ==, 0);
	munit_assert_int64(pacing.period_ns, >, REAL_72 - 2000LL);
	munit_assert_int64(pacing.period_ns, <, REAL_72 + 2000LL);
	PleikkariVrPacingWindow window;
	pleikkari_vr_pacing_take_window(&pacing, &window);
	munit_assert_uint32(window.leads[1], ==, 400); // every frame one refresh early
	munit_assert_uint32(window.drains, ==, 0);     // VRAPI mode never drains
	// A stall makes the next frame late; the hold keeps the one after it out of that refresh.
	// Its release comes 3.8 ms after the late return, too close, so it aims at the one after.
	const int64_t late = release + REAL_72 + 5 * MS;
	frame(&pacing, late, 3 * MS, 2 * MS, release + 3 * REAL_72 + lead0(REAL_72));
	const int64_t wake = pleikkari_vr_pacing_wake_ns(&pacing, late + 5 * MS + 100000LL);
	munit_assert_int64(wake, >=, release + 3 * REAL_72 - budget - 20000LL);
	munit_assert_int64(wake, <=, release + 3 * REAL_72 - budget + 20000LL);
	// A late frame back just after a release: the next frame starts the budget before the next
	// release, and a loop top already past that point but 7 ms before it goes at once.
	const int64_t r4 = release + 4 * REAL_72;
	const int64_t r5 = r4 + REAL_72;
	frame(&pacing, r4 - 4 * MS, 3 * MS, 2 * MS, r5 + REAL_72 + lead0(REAL_72));
	const int64_t hold = pleikkari_vr_pacing_wake_ns(&pacing, r4 + 1100000LL);
	munit_assert_int64(hold, >=, r5 - budget - 20000LL);
	munit_assert_int64(hold, <=, r5 - budget + 20000LL);
	munit_assert_int64(pleikkari_vr_pacing_wake_ns(&pacing, r5 - 7 * MS), ==, 0);
	return MUNIT_OK;
}

static MunitResult test_drain(const MunitParameter params[], void *user)
{
	(void)params;
	(void)user;
	const int64_t budget = PLEIKKARI_VR_PACING_DEFAULT_BUDGET_NS;
	PleikkariVrPacingConfig config;
	pleikkari_vr_pacing_config_default(&config, 72.0f);
	config.mode = PLEIKKARI_VR_PACING_LATE;
	PleikkariVrPacing pacing;
	pleikkari_vr_pacing_init(&pacing, &config);
	// Not yet: 35 frames in a row one refresh early.
	int64_t release = throttled_frames(&pacing, 3000 * MS, PLEIKKARI_VR_PACING_DRAIN_FRAMES - 1, 1);
	int64_t wake = pleikkari_vr_pacing_wake_ns(&pacing, release + 50000LL);
	munit_assert_false(pacing.draining);
	munit_assert_int64(wake - (release + REAL_72 - budget), <, 20000LL);
	munit_assert_int64(wake - (release + REAL_72 - budget), >, -20000LL);
	// The 36th: the next frame skips one release.
	release = throttled_frames(&pacing, release, 1, 1);
	wake = pleikkari_vr_pacing_wake_ns(&pacing, release + 50000LL);
	munit_assert_true(pacing.draining);
	munit_assert_int64(wake - (release + 2 * REAL_72 - budget), <, 20000LL);
	munit_assert_int64(wake - (release + 2 * REAL_72 - budget), >, -20000LL);
	// Still at lead 1 afterwards: no second drain within 2 s.
	release = throttled_frames(&pacing, release + REAL_72, 100, 1);
	pleikkari_vr_pacing_wake_ns(&pacing, release + 50000LL);
	munit_assert_false(pacing.draining);
	// After 2 s (144 frames) another is allowed.
	release = throttled_frames(&pacing, release, 60, 1);
	pleikkari_vr_pacing_wake_ns(&pacing, release + 50000LL);
	munit_assert_true(pacing.draining);
	PleikkariVrPacingWindow window;
	pleikkari_vr_pacing_take_window(&pacing, &window);
	munit_assert_uint32(window.drains, ==, 2);
	// At lead 0 nothing drains, and drain=0 turns it off.
	pleikkari_vr_pacing_init(&pacing, &config);
	release = throttled_frames(&pacing, 9000 * MS, 200, 0);
	pleikkari_vr_pacing_wake_ns(&pacing, release + 50000LL);
	munit_assert_false(pacing.draining);
	munit_assert_int(pleikkari_vr_pacing_config_parse(&config, "drain=0"), ==, 1);
	pleikkari_vr_pacing_init(&pacing, &config);
	release = throttled_frames(&pacing, 9000 * MS, 200, 1);
	pleikkari_vr_pacing_wake_ns(&pacing, release + 50000LL);
	munit_assert_false(pacing.draining);
	return MUNIT_OK;
}

// PLE-753: the hold and drain, with no late latch.
static MunitResult test_hold_drain(const MunitParameter params[], void *user)
{
	(void)params;
	(void)user;
	PleikkariVrPacingConfig config;
	pleikkari_vr_pacing_config_default(&config, 72.0f);
	munit_assert_false(config.flush_eyes);
	munit_assert_int(pleikkari_vr_pacing_config_parse(&config, "holddrain,flusheyes"), ==, 2);
	munit_assert_int(config.mode, ==, PLEIKKARI_VR_PACING_HOLD);
	munit_assert_true(config.flush_eyes);
	munit_assert_string_equal(pleikkari_vr_pacing_mode_name(PLEIKKARI_VR_PACING_HOLD), "hold and drain");
	PleikkariVrPacing pacing;
	pleikkari_vr_pacing_init(&pacing, &config);
	// Throttled frames at lead 0 start as soon as the submit returns, as VrApi's own loop does: a
	// room keeps the whole refresh.
	munit_assert_int64(pleikkari_vr_pacing_wake_ns(&pacing, 2000 * MS), ==, 0);
	int64_t release = throttled_frames(&pacing, 2000 * MS, 400, 0);
	munit_assert_int64(pleikkari_vr_pacing_wake_ns(&pacing, release + 50000LL), ==, 0);
	munit_assert_false(pacing.draining);
	// A frame came 5 ms after its release and was let through at once: the next one starts at
	// the next release, not in the late one's refresh.
	const int64_t late = release + REAL_72 + 5 * MS;
	frame(&pacing, late - 3 * MS, 3 * MS, 2 * MS, release + 2 * REAL_72 + lead0(REAL_72));
	munit_assert_false(pacing.throttled);
	int64_t wake = pleikkari_vr_pacing_wake_ns(&pacing, late + 2 * MS);
	munit_assert_int64(wake, >=, release + 2 * REAL_72 - 20000LL);
	munit_assert_int64(wake, <=, release + 2 * REAL_72 + 20000LL);
	// Back on time: no sleep.
	release = throttled_frames(&pacing, release + 2 * REAL_72, 1, 0);
	munit_assert_int64(pleikkari_vr_pacing_wake_ns(&pacing, release + 50000LL), ==, 0);
	// A late return 1 ms before a release may share that release's refresh with its submit
	// call: the next frame waits for the release after it.
	const int64_t r = release + 2 * REAL_72;
	frame(&pacing, r - 6 * MS, 3 * MS, 2 * MS, r + REAL_72 + lead0(REAL_72));
	wake = pleikkari_vr_pacing_wake_ns(&pacing, r - 1 * MS);
	munit_assert_int64(wake, >=, r + REAL_72 - 20000LL);
	munit_assert_int64(wake, <=, r + REAL_72 + 20000LL);
	PleikkariVrPacingWindow window;
	pleikkari_vr_pacing_take_window(&pacing, &window);
	munit_assert_uint32(window.drains, ==, 0);

	// At lead 1, the 36th frame in a row drains: the next frame skips the release it would start
	// at and starts at the one after, once within 2 s.
	pleikkari_vr_pacing_init(&pacing, &config);
	release = throttled_frames(&pacing, 3000 * MS, PLEIKKARI_VR_PACING_DRAIN_FRAMES - 1, 1);
	munit_assert_int64(pleikkari_vr_pacing_wake_ns(&pacing, release + 50000LL), ==, 0);
	release = throttled_frames(&pacing, release, 1, 1);
	wake = pleikkari_vr_pacing_wake_ns(&pacing, release + 50000LL);
	munit_assert_true(pacing.draining);
	munit_assert_int64(wake, >=, release + REAL_72 - 20000LL);
	munit_assert_int64(wake, <=, release + REAL_72 + 20000LL);
	release = throttled_frames(&pacing, release + REAL_72, 100, 1);
	munit_assert_int64(pleikkari_vr_pacing_wake_ns(&pacing, release + 50000LL), ==, 0);
	munit_assert_false(pacing.draining);
	pleikkari_vr_pacing_take_window(&pacing, &window);
	munit_assert_uint32(window.drains, ==, 1);
	munit_assert_uint32(window.leads[1], ==, PLEIKKARI_VR_PACING_DRAIN_FRAMES + 100);
	// drain=2 skips one release more; drain=0 turns the drain off and leaves the hold.
	munit_assert_int(pleikkari_vr_pacing_config_parse(&config, "drain=2"), ==, 1);
	pleikkari_vr_pacing_init(&pacing, &config);
	release = throttled_frames(&pacing, 9000 * MS, PLEIKKARI_VR_PACING_DRAIN_FRAMES, 1);
	wake = pleikkari_vr_pacing_wake_ns(&pacing, release + 50000LL);
	munit_assert_int64(wake, >=, release + 2 * REAL_72 - 20000LL);
	munit_assert_int64(wake, <=, release + 2 * REAL_72 + 20000LL);
	munit_assert_int(pleikkari_vr_pacing_config_parse(&config, "drain=0"), ==, 1);
	pleikkari_vr_pacing_init(&pacing, &config);
	release = throttled_frames(&pacing, 9000 * MS, 200, 1);
	munit_assert_int64(pleikkari_vr_pacing_wake_ns(&pacing, release + 50000LL), ==, 0);
	munit_assert_false(pacing.draining);
	return MUNIT_OK;
}

static MunitResult test_experiments(const MunitParameter params[], void *user)
{
	(void)params;
	(void)user;
	PleikkariVrPacingConfig config;
	pleikkari_vr_pacing_config_default(&config, 72.0f);
	munit_assert_int(pleikkari_vr_pacing_config_parse(&config, "sleep=1000,sweep=500/2/1500,stall=3:20000"), ==, 3);
	PleikkariVrPacing pacing;
	pleikkari_vr_pacing_init(&pacing, &config);
	const int64_t now = 5000 * MS;
	// Every frame sleeps the fixed 1 ms; frames 2, 4 and 6 add a 0.5 ms step each, up to the
	// 1.5 ms maximum, and frame 3 also stalls 20 ms.
	const int64_t expected[] = { 1000000LL, 1000000LL, 1500000LL, 21500000LL, 2000000LL, 2000000LL, 2500000LL,
		2500000LL, 2500000LL };
	for(int i = 0; i < (int)(sizeof(expected) / sizeof(expected[0])); i++)
	{
		munit_assert_int64(pleikkari_vr_pacing_wake_ns(&pacing, now) - now, ==, expected[i]);
		frame(&pacing, now, MS, MS, now + 40 * MS);
	}
	return MUNIT_OK;
}

static MunitResult test_latch_on_signal(const MunitParameter params[], void *user)
{
	(void)params;
	(void)user;
	const int64_t budget = PLEIKKARI_VR_PACING_DEFAULT_BUDGET_NS;
	PleikkariVrPacingConfig config;
	pleikkari_vr_pacing_config_default(&config, 72.0f);
	munit_assert_false(config.latch_on_signal);
	PleikkariVrPacing pacing;
	// Off (the default): the loop never waits for the frame signal.
	pleikkari_vr_pacing_init(&pacing, &config);
	int64_t release = throttled_frames(&pacing, 4000 * MS, 10, 1);
	munit_assert_int64(pleikkari_vr_pacing_latch_deadline_ns(&pacing, release + 50000LL), ==, 0);

	munit_assert_int(pleikkari_vr_pacing_config_parse(&config, "signal"), ==, 1);
	munit_assert_true(config.latch_on_signal);
	munit_assert_int(config.mode, ==, PLEIKKARI_VR_PACING_VRAPI);
	pleikkari_vr_pacing_init(&pacing, &config);
	// Nothing to key on before the first frame.
	munit_assert_int64(pleikkari_vr_pacing_latch_deadline_ns(&pacing, 4000 * MS), ==, 0);
	// After a submit VrApi held to its release: wait until the budget before the next release.
	// Today's loop never sleeps; pace() still starts at once. (400 frames learn the period.)
	release = throttled_frames(&pacing, 4000 * MS, 400, 1);
	munit_assert_int64(pleikkari_vr_pacing_wake_ns(&pacing, release + 50000LL), ==, 0);
	int64_t deadline = pleikkari_vr_pacing_latch_deadline_ns(&pacing, release + 50000LL);
	munit_assert_int64(deadline, >=, release + REAL_72 - budget - 20000LL);
	munit_assert_int64(deadline, <=, release + REAL_72 - budget + 20000LL);
	// A loop top already past that point latches at once, never aiming at a later release.
	munit_assert_int64(pleikkari_vr_pacing_latch_deadline_ns(&pacing, release + REAL_72 - budget + 100000LL), ==, 0);
	// VrApi's return a little either side of the grid still names the next release.
	for(int side = -1; side <= 1; side += 2)
	{
		const int64_t grid = release + REAL_72;
		const int64_t returned = grid + side * 400000LL;
		const int64_t start = release + 50000LL;
		pleikkari_vr_pacing_frame(&pacing, start, 0, start + 3 * MS, returned, grid + REAL_72 + lead0(REAL_72));
		munit_assert_true(pacing.throttled);
		deadline = pleikkari_vr_pacing_latch_deadline_ns(&pacing, returned + 100000LL);
		munit_assert_int64(deadline, >=, grid + REAL_72 - budget - 20000LL);
		munit_assert_int64(deadline, <=, grid + REAL_72 - budget + 20000LL);
		release = grid;
	}

	// A wait the frame signal ended, then a throttled submit: nothing is held.
	pleikkari_vr_pacing_latch_waited(&pacing, 3 * MS, true);
	release = throttled_frames(&pacing, release, 1, 1);
	munit_assert_false(pacing.latch_hold);
	munit_assert_int64(pleikkari_vr_pacing_wake_ns(&pacing, release + 50000LL), ==, 0);
	// A wait to the deadline, and the frame still came late: no wait after it, and the next frame
	// is held out of that refresh, to the budget before the release after it.
	pleikkari_vr_pacing_latch_waited(&pacing, 5 * MS, false);
	const int64_t late = release + REAL_72 + 1 * MS;
	frame(&pacing, late - 3 * MS, 3 * MS, 2 * MS, release + 2 * REAL_72 + REAL_72 + lead0(REAL_72));
	munit_assert_false(pacing.throttled);
	munit_assert_true(pacing.latch_hold);
	munit_assert_int64(pleikkari_vr_pacing_latch_deadline_ns(&pacing, late + 2 * MS + 50000LL), ==, 0);
	int64_t wake = pleikkari_vr_pacing_wake_ns(&pacing, late + 2 * MS + 50000LL);
	munit_assert_int64(wake, >=, release + 2 * REAL_72 - budget - 20000LL);
	munit_assert_int64(wake, <=, release + 2 * REAL_72 - budget + 20000LL);
	PleikkariVrPacingWindow window;
	pleikkari_vr_pacing_take_window(&pacing, &window);
	munit_assert_uint32(window.latch_waits, ==, 2);
	munit_assert_uint32(window.latch_signalled, ==, 1);
	munit_assert_int64(window.latch_wait_sum_ns, ==, 8 * MS);
	munit_assert_int64(window.latch_wait_max_ns, ==, 5 * MS);
	munit_assert_uint32(window.latch_late, ==, 1);
	// A late frame the loop did not make wait is today's: no hold.
	release = throttled_frames(&pacing, release + 2 * REAL_72, 10, 1);
	frame(&pacing, release + REAL_72 - 2 * MS, 3 * MS, 2 * MS, release + 2 * REAL_72 + REAL_72 + lead0(REAL_72));
	munit_assert_false(pacing.latch_hold);
	munit_assert_int64(pleikkari_vr_pacing_wake_ns(&pacing, release + REAL_72 + 3 * MS + 50000LL), ==, 0);
	pleikkari_vr_pacing_take_window(&pacing, &window);
	munit_assert_uint32(window.latch_waits, ==, 0);
	munit_assert_uint32(window.latch_late, ==, 0);
	munit_assert_int64(window.latch_wait_max_ns, ==, 0);

	// Under the late start the frame already starts at that point: the latch never waits.
	config.mode = PLEIKKARI_VR_PACING_LATE;
	pleikkari_vr_pacing_init(&pacing, &config);
	release = throttled_frames(&pacing, 6000 * MS, 10, 0);
	wake = pleikkari_vr_pacing_wake_ns(&pacing, release + 50000LL);
	munit_assert_int64(wake, >=, release + REAL_72 - budget - 20000LL);
	munit_assert_int64(pleikkari_vr_pacing_latch_deadline_ns(&pacing, release + 50000LL), ==, 0);
	munit_assert_int64(pleikkari_vr_pacing_latch_deadline_ns(&pacing, wake), ==, 0);

	// PLE-753: nor under the hold and drain, which is not VrApi's release either.
	config.mode = PLEIKKARI_VR_PACING_HOLD;
	pleikkari_vr_pacing_init(&pacing, &config);
	release = throttled_frames(&pacing, 8000 * MS, 400, 0);
	munit_assert_int64(pleikkari_vr_pacing_latch_deadline_ns(&pacing, release + 50000LL), ==, 0);
	return MUNIT_OK;
}

MunitTest tests_vr_frame_pacing[] = {
	{ "/config", test_config, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/vrapi_never_sleeps", test_vrapi_never_sleeps, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/late_start", test_late_start, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/hold_and_period", test_hold_and_period, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/drain", test_drain, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/hold_drain", test_hold_drain, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/experiments", test_experiments, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/latch_on_signal", test_latch_on_signal, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
