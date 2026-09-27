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
		"trace,late,budget=4000,sleep=1500,sweep=500/144/13000,stall=600:20000;1200:30000,bogus"), ==, 6);
	munit_assert_true(config.trace);
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
	// Nothing to key on before the first release.
	munit_assert_int64(pleikkari_vr_pacing_wake_ns(&pacing, t), ==, 0);
	// VrApi held the submit 11.9 ms: it returned at its release, so the next frame starts the
	// budget before the release after it.
	frame(&pacing, t, 2 * MS, 11900000LL, t + 46 * MS);
	const int64_t returned = t + 2 * MS + 11900000LL;
	munit_assert_int64(pleikkari_vr_pacing_wake_ns(&pacing, returned + 100000LL), ==,
		returned + PERIOD_72 - PLEIKKARI_VR_PACING_DEFAULT_BUDGET_NS);
	// A start already past that point goes at once.
	munit_assert_int64(pleikkari_vr_pacing_wake_ns(&pacing, returned + PERIOD_72), ==, 0);
	// A frame that came late (VrApi returned at once) gives no release to key on: the next one
	// starts at once, and VrApi's throttle anchors it again.
	const int64_t late = returned + PERIOD_72 + 3 * MS;
	frame(&pacing, late, 11 * MS, 200000LL, late + 40 * MS);
	munit_assert_false(pacing.throttled);
	munit_assert_int64(pleikkari_vr_pacing_wake_ns(&pacing, late + 11200000LL + 1000), ==, 0);
	PleikkariVrPacingWindow window;
	pleikkari_vr_pacing_take_window(&pacing, &window);
	munit_assert_uint32(window.frames, ==, 2);
	munit_assert_uint32(window.throttled, ==, 1);
	munit_assert_uint32(window.late, ==, 1);
	munit_assert_int64(window.wait_min_ns, ==, 200000LL);
	munit_assert_int64(window.wait_max_ns, ==, 11900000LL);
	munit_assert_int64(window.work_max_ns, ==, 11 * MS);
	munit_assert_int64(window.ahead_min_ns, ==, 29 * MS);
	munit_assert_int64(window.ahead_max_ns, ==, 44 * MS);
	munit_assert_int64(window.lead_min_ns, ==, 28800000LL);
	munit_assert_int64(window.lead_max_ns, ==, 32100000LL);
	// The next window starts empty.
	pleikkari_vr_pacing_take_window(&pacing, &window);
	munit_assert_uint32(window.frames, ==, 0);
	munit_assert_int64(window.wait_min_ns, ==, 0);
	munit_assert_string_equal(pleikkari_vr_pacing_mode_name(PLEIKKARI_VR_PACING_LATE), "late start");
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

MunitTest tests_vr_frame_pacing[] = {
	{ "/config", test_config, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/vrapi_never_sleeps", test_vrapi_never_sleeps, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/late_start", test_late_start, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/experiments", test_experiments, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
