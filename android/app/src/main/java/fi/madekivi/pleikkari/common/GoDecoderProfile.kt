// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.common

import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Build
import android.util.Log
import fi.madekivi.pleikkari.lib.Codec

/**
 * PLE-635: the decoder profile for the Oculus Go (Snapdragon 821, Android 7.1.1). Its
 * `OMX.qcom.video.decoder.avc`/`.hevc` get their MediaFormat through Android 7.1's ACodec, which
 * reads only `frame-rate`, `operating-rate` and `priority` from it. It has no `low-latency` key and
 * no `vendor.*` extension mapping, so the decode-order and low-latency extensions the Go's
 * libOmxVdec implements cannot be reached from an app. The profile asks for the highest operating
 * rate the decoder advertises at the stream size, the only rate its msm_vidc driver takes above
 * the stream fps, plus realtime priority.
 */
object GoDecoderProfile
{
	private const val TAG = "GoDecoderProfile"
	const val DEVICE = "pacific"

	fun eligible(device: String = Build.DEVICE) = device == DEVICE

	/** The value for [fi.madekivi.pleikkari.lib.ConnectInfo.decoderQcomProfileOperatingRate]; 0 = profile off. */
	fun operatingRate(preferences: Preferences, device: String = Build.DEVICE): Int
	{
		if(!preferences.decoderQcomGoProfile || !eligible(device))
			return 0
		val profile = preferences.videoProfile
		val mime = mime(profile.codec)
		val capacity = decoderCapacity(mime, profile.width, profile.height)
		val rate = rate(profile.maxFPS, capacity?.maxFps)
		Log.i(TAG, "Qualcomm OMX decoder profile: ${capacity?.name ?: "no decoder found"} for $mime at " +
			"${profile.width}x${profile.height} supports up to ${capacity?.maxFps ?: "?"} fps; operating-rate $rate")
		return rate
	}

	internal fun mime(codec: Codec) =
		if(codec == Codec.CODEC_H264) MediaFormat.MIMETYPE_VIDEO_AVC else MediaFormat.MIMETYPE_VIDEO_HEVC

	/** Never below the stream's own rate, so the profile stays on when the capacity lookup fails. */
	internal fun rate(streamFps: Int, capacityFps: Double?): Int =
		maxOf(streamFps, capacityFps?.toInt() ?: 0, 1)

	private class Capacity(val name: String, val maxFps: Double)

	/** The decoder the framework's list picks for [mime], as createDecoderByType does, and its rate limit. */
	private fun decoderCapacity(mime: String, width: Int, height: Int): Capacity? = try
	{
		val codecs = MediaCodecList(MediaCodecList.REGULAR_CODECS)
		codecs.findDecoderForFormat(MediaFormat.createVideoFormat(mime, width, height))?.let { name ->
			val video = codecs.codecInfos.first { it.name == name }.getCapabilitiesForType(mime).videoCapabilities
			Capacity(name, video.getSupportedFrameRatesFor(width, height).upper)
		}
	}
	catch(e: IllegalArgumentException)
	{
		Log.w(TAG, "No decoder capacity for $mime at ${width}x$height", e)
		null
	}
}
