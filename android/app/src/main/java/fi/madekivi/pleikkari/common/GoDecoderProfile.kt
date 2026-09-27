// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.common

import android.os.Build

/**
 * PLE-635: the Oculus Go's (Snapdragon 821, Android 7.1.1) decoder low-latency switch. The Go
 * decodes with `OMX.qcom.video.decoder.avc`/`.hevc` through Android 7.1's ACodec, which has no
 * `low-latency` key and no `vendor.*` extension mapping. Qualcomm's ExtendedACodec
 * (`libavenhancements.so`) does read `vt-low-latency`, its video-telephony switch, and sets the
 * decoder's `OMX.QTI.index.param.video.LowLatency`: decode-order output with timestamp
 * reordering off. The key means nothing on other devices, so it is applied only on the Go.
 */
object GoDecoderProfile
{
	const val DEVICE = "pacific"

	fun eligible(device: String = Build.DEVICE) = device == DEVICE

	/** The value for [fi.madekivi.pleikkari.lib.ConnectInfo.decoderQcomVtLowLatency]. */
	fun vtLowLatency(preferences: Preferences, device: String = Build.DEVICE) =
		enabled(preferences.decoderQcomVtLowLatency, device)

	internal fun enabled(setting: Boolean, device: String) = setting && eligible(device)
}
