// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "video-decoder-operating-rate.h"

AndroidChiakiDecoderOperatingRate android_chiaki_video_decoder_select_operating_rate(
		int32_t explicit_rate, bool default_path_enabled, int32_t default_path_rate,
		bool real_pts_enabled, bool auto_enabled, int32_t auto_rate)
{
	if(explicit_rate > 0)
		return (AndroidChiakiDecoderOperatingRate) {
			.rate = explicit_rate,
			.source = ANDROID_CHIAKI_DECODER_OPERATING_RATE_EXPLICIT,
		};
	if(default_path_enabled)
		return (AndroidChiakiDecoderOperatingRate) {
			.rate = default_path_rate,
			.source = ANDROID_CHIAKI_DECODER_OPERATING_RATE_DEFAULT_PATH,
		};
	if(real_pts_enabled && auto_enabled)
		return (AndroidChiakiDecoderOperatingRate) {
			.rate = auto_rate,
			.source = ANDROID_CHIAKI_DECODER_OPERATING_RATE_AUTO,
		};
	return (AndroidChiakiDecoderOperatingRate) {
		.rate = 0,
		.source = ANDROID_CHIAKI_DECODER_OPERATING_RATE_NONE,
	};
}
