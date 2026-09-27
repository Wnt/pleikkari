// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_JNI_VIDEO_DECODER_OPERATING_RATE_H
#define CHIAKI_JNI_VIDEO_DECODER_OPERATING_RATE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum android_chiaki_decoder_operating_rate_source_t
{
	ANDROID_CHIAKI_DECODER_OPERATING_RATE_NONE,
	ANDROID_CHIAKI_DECODER_OPERATING_RATE_EXPLICIT,
	ANDROID_CHIAKI_DECODER_OPERATING_RATE_DEFAULT_PATH,
	ANDROID_CHIAKI_DECODER_OPERATING_RATE_AUTO,
} AndroidChiakiDecoderOperatingRateSource;

typedef struct android_chiaki_decoder_operating_rate_t
{
	int32_t rate;
	AndroidChiakiDecoderOperatingRateSource source;
} AndroidChiakiDecoderOperatingRate;

AndroidChiakiDecoderOperatingRate android_chiaki_video_decoder_select_operating_rate(
		int32_t explicit_rate, bool default_path_enabled, int32_t default_path_rate,
		bool real_pts_enabled, bool auto_enabled, int32_t auto_rate);

#endif
