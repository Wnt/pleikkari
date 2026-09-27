// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <munit.h>

#include "../android/app/src/main/cpp/video-decoder-operating-rate.h"

static MunitResult test_precedence(const MunitParameter params[], void *user)
{
	(void)params;
	(void)user;

	AndroidChiakiDecoderOperatingRate selected =
			android_chiaki_video_decoder_select_operating_rate(1920, true, 960, true, true, 960);
	munit_assert_int(selected.rate, ==, 1920);
	munit_assert_int(selected.source, ==, ANDROID_CHIAKI_DECODER_OPERATING_RATE_EXPLICIT);

	selected = android_chiaki_video_decoder_select_operating_rate(0, true, 960, true, true, 960);
	munit_assert_int(selected.rate, ==, 960);
	munit_assert_int(selected.source, ==, ANDROID_CHIAKI_DECODER_OPERATING_RATE_DEFAULT_PATH);

	selected = android_chiaki_video_decoder_select_operating_rate(0, false, 960, true, true, 960);
	munit_assert_int(selected.rate, ==, 960);
	munit_assert_int(selected.source, ==, ANDROID_CHIAKI_DECODER_OPERATING_RATE_AUTO);

	selected = android_chiaki_video_decoder_select_operating_rate(0, false, 960, true, false, 960);
	munit_assert_int(selected.rate, ==, 0);
	munit_assert_int(selected.source, ==, ANDROID_CHIAKI_DECODER_OPERATING_RATE_NONE);

	selected = android_chiaki_video_decoder_select_operating_rate(0, false, 960, false, true, 960);
	munit_assert_int(selected.rate, ==, 0);
	munit_assert_int(selected.source, ==, ANDROID_CHIAKI_DECODER_OPERATING_RATE_NONE);

	return MUNIT_OK;
}

MunitTest tests_video_decoder_operating_rate[] = {
	{ "/precedence", test_precedence, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
