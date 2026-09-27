// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.common

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class StreamQualityPresetTest
{
	@Test
	fun defaultMatchesCurrentEffectiveStreamDefaults()
	{
		val preset = Preferences.streamQualityPresetDefault

		assertEquals(Preferences.StreamQualityPreset.LOW_LATENCY, preset)
		assertEquals(Preferences.resolutionDefault, preset.resolution)
		assertEquals(Preferences.fpsDefault, preset.fps)
		assertEquals(Preferences.codecDefault, preset.codec)
	}

	@Test
	fun namedPresetsMatchTheUxContract()
	{
		assertPreset(
			Preferences.StreamQualityPreset.BALANCED,
			Preferences.Resolution.RES_1080P,
			Preferences.FPS.FPS_60,
			Preferences.Codec.CODEC_H265
		)
		assertPreset(
			Preferences.StreamQualityPreset.LOW_LATENCY,
			Preferences.Resolution.RES_1080P,
			Preferences.FPS.FPS_60,
			Preferences.Codec.CODEC_H265
		)
		assertPreset(
			Preferences.StreamQualityPreset.DATA_SAVER,
			Preferences.Resolution.RES_540P,
			Preferences.FPS.FPS_30,
			Preferences.Codec.CODEC_H264
		)
	}

	@Test
	fun lowLatencyPinsAnExplicitBitrateSoItStaysDistinctFromBalancedAt1080p()
	{
		assertEquals(10000, Preferences.StreamQualityPreset.LOW_LATENCY.bitrate)
		assertNull(Preferences.StreamQualityPreset.BALANCED.bitrate)
	}

	@Test
	fun everyPresetUsesTheProvenDecoderDefaults()
	{
		Preferences.StreamQualityPreset.values().forEach { preset ->
			assertTrue(preset.decoderOperatingRateDefault)
			assertTrue(preset.decoderOperatingRateAuto)
			assertEquals(0, preset.decoderOperatingRate)
			assertTrue(preset.decoderInputThreadEnabled)
			assertFalse(preset.debandingEnabled)
		}
	}

	@Test
	fun eachPresetMatchesOnlyItsOwnValues()
	{
		Preferences.StreamQualityPreset.values().forEach { preset ->
			val matching = Preferences.StreamQualityPreset.values().filter { matchesValuesOf(it, preset) }
			assertEquals(listOf(preset), matching)
		}
	}

	@Test
	fun anyChangedFieldMakesThePresetCustom()
	{
		val p = Preferences.StreamQualityPreset.LOW_LATENCY
		fun m(
			resolution: Preferences.Resolution = p.resolution, fps: Preferences.FPS = p.fps,
			codec: Preferences.Codec = p.codec, bitrate: Int? = p.bitrate,
			rateDefault: Boolean = p.decoderOperatingRateDefault, rateAuto: Boolean = p.decoderOperatingRateAuto,
			rate: Int = p.decoderOperatingRate, inputThread: Boolean = p.decoderInputThreadEnabled,
			debanding: Boolean = p.debandingEnabled
		) = p.matches(resolution, fps, codec, bitrate, rateDefault, rateAuto, rate, inputThread, debanding)
		assertTrue(m())
		assertFalse(m(resolution = Preferences.Resolution.RES_720P))
		assertFalse(m(fps = Preferences.FPS.FPS_30))
		assertFalse(m(codec = Preferences.Codec.CODEC_H264))
		assertFalse(m(bitrate = 12000))
		assertFalse(m(rateDefault = false))
		assertFalse(m(rateAuto = false))
		assertFalse(m(rate = 120))
		assertFalse(m(inputThread = false))
		assertFalse(m(debanding = true))
	}

	private fun matchesValuesOf(candidate: Preferences.StreamQualityPreset, values: Preferences.StreamQualityPreset) =
		candidate.matches(values.resolution, values.fps, values.codec, values.bitrate,
			values.decoderOperatingRateDefault, values.decoderOperatingRateAuto, values.decoderOperatingRate,
			values.decoderInputThreadEnabled, values.debandingEnabled)

	private fun assertPreset(
		preset: Preferences.StreamQualityPreset,
		resolution: Preferences.Resolution,
		fps: Preferences.FPS,
		codec: Preferences.Codec
	)
	{
		assertEquals(resolution, preset.resolution)
		assertEquals(fps, preset.fps)
		assertEquals(codec, preset.codec)
	}
}
