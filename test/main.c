// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <munit.h>
#include <chiaki/config.h>

extern MunitTest tests_seq_num[];
extern MunitTest tests_key_state[];
extern MunitTest tests_reorder_queue[];
extern MunitTest tests_http[];
extern MunitTest tests_rpcrypt[];
extern MunitTest tests_gkcrypt[];
extern MunitTest tests_takion[];
extern MunitTest tests_senkusha[];
extern MunitTest tests_link_watchdog[];
extern MunitTest tests_frame_loss[];
extern MunitTest tests_video_receiver[];
extern MunitTest tests_fec[];
extern MunitTest tests_regist[];
extern MunitTest tests_session_mtu_fallback[];
#ifndef CHIAKI_LIB_ENABLE_MBEDTLS
extern MunitTest tests_aia[];
#endif
extern MunitTest tests_bitstream[];
extern MunitTest tests_video_presenter[];
extern MunitTest tests_video_frame_latency[];
extern MunitTest tests_video_decoder_operating_rate[];
extern MunitTest tests_video_decoder_codec_header[];
extern MunitTest tests_vr_screen_placement[];
extern MunitTest tests_vr_frame_pacing[];
#if CHIAKI_LIB_ENABLE_FFMPEG_DECODER
extern MunitTest tests_ffmpegdecoder[];
#endif

static MunitSuite suites[] = {
	{
		"/seq_num",
		tests_seq_num,
		NULL,
		1,
		MUNIT_SUITE_OPTION_NONE
	},
	{
		"/key_state",
		tests_key_state,
		NULL,
		1,
		MUNIT_SUITE_OPTION_NONE
	},
	{
		"/reorder_queue",
		tests_reorder_queue,
		NULL,
		1,
		MUNIT_SUITE_OPTION_NONE
	},
	{
		"/http",
		tests_http,
		NULL,
		1,
		MUNIT_SUITE_OPTION_NONE
	},
	{
		"/rpcrypt",
		tests_rpcrypt,
		NULL,
		1,
		MUNIT_SUITE_OPTION_NONE
	},
	{
		"/gkcrypt",
		tests_gkcrypt,
		NULL,
		1,
		MUNIT_SUITE_OPTION_NONE
	},
	{
		"/takion",
		tests_takion,
		NULL,
		1,
		MUNIT_SUITE_OPTION_NONE
	},
	{
		"/senkusha",
		tests_senkusha,
		NULL,
		1,
		MUNIT_SUITE_OPTION_NONE
	},
	{
		"/link_watchdog",
		tests_link_watchdog,
		NULL,
		1,
		MUNIT_SUITE_OPTION_NONE
	},
	{
		"/frame_loss",
		tests_frame_loss,
		NULL,
		1,
		MUNIT_SUITE_OPTION_NONE
	},
	{
		"/video_receiver",
		tests_video_receiver,
		NULL,
		1,
		MUNIT_SUITE_OPTION_NONE
	},
	{
		"/fec",
		tests_fec,
		NULL,
		1,
		MUNIT_SUITE_OPTION_NONE
	},
	{
		"/regist",
		tests_regist,
		NULL,
		1,
		MUNIT_SUITE_OPTION_NONE
	},
	{
		"/session_mtu_fallback",
		tests_session_mtu_fallback,
		NULL,
		1,
		MUNIT_SUITE_OPTION_NONE
	},
#ifndef CHIAKI_LIB_ENABLE_MBEDTLS
	{
		"/aia",
		tests_aia,
		NULL,
		1,
		MUNIT_SUITE_OPTION_NONE
	},
#endif
	{
		"/bitstream",
		tests_bitstream,
		NULL,
		1,
		MUNIT_SUITE_OPTION_NONE
	},
	{
		"/video_presenter",
		tests_video_presenter,
		NULL,
		1,
		MUNIT_SUITE_OPTION_NONE
	},
	{
		"/video_frame_latency",
		tests_video_frame_latency,
		NULL,
		1,
		MUNIT_SUITE_OPTION_NONE
	},
	{
		"/video_decoder_operating_rate",
		tests_video_decoder_operating_rate,
		NULL,
		1,
		MUNIT_SUITE_OPTION_NONE
	},
	{
		"/video_decoder_codec_header",
		tests_video_decoder_codec_header,
		NULL,
		1,
		MUNIT_SUITE_OPTION_NONE
	},
	{
		"/vr_screen_placement",
		tests_vr_screen_placement,
		NULL,
		1,
		MUNIT_SUITE_OPTION_NONE
	},
	{
		"/vr_frame_pacing",
		tests_vr_frame_pacing,
		NULL,
		1,
		MUNIT_SUITE_OPTION_NONE
	},
#if CHIAKI_LIB_ENABLE_FFMPEG_DECODER
	{
		"/ffmpegdecoder",
		tests_ffmpegdecoder,
		NULL,
		1,
		MUNIT_SUITE_OPTION_NONE
	},
#endif
	{ NULL, NULL, NULL, 0, MUNIT_SUITE_OPTION_NONE }
};

static const MunitSuite suite_main = {
	"/chiaki",
	NULL,
	suites,
	1,
	MUNIT_SUITE_OPTION_NONE
};

int main(int argc, char *argv[])
{
	return munit_suite_main(&suite_main, NULL, argc, argv);
}
