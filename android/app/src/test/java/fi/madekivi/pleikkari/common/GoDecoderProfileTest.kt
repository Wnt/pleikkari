// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.common

import fi.madekivi.pleikkari.lib.Codec
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class GoDecoderProfileTest
{
	@Test
	fun onlyTheGoIsEligible()
	{
		assertTrue(GoDecoderProfile.eligible("pacific"))
		assertFalse(GoDecoderProfile.eligible("b0s"))
		assertFalse(GoDecoderProfile.eligible(""))
	}

	@Test
	fun rateIsTheDecoderCapacityAtTheStreamSize()
	{
		// The Go's OMX.qcom decoders: 1,958,400 blocks/s over 8160 blocks at 1080p, capped by
		// their 1-240 frame-rate range.
		assertEquals(240, GoDecoderProfile.rate(60, 240.0))
		assertEquals(240, GoDecoderProfile.rate(60, 240.9))
	}

	@Test
	fun rateNeverDropsBelowTheStreamFps()
	{
		assertEquals(60, GoDecoderProfile.rate(60, null))
		assertEquals(60, GoDecoderProfile.rate(60, 30.0))
		assertEquals(1, GoDecoderProfile.rate(0, null))
	}

	@Test
	fun mimeFollowsTheStreamCodec()
	{
		assertEquals("video/avc", GoDecoderProfile.mime(Codec.CODEC_H264))
		assertEquals("video/hevc", GoDecoderProfile.mime(Codec.CODEC_H265))
		assertEquals("video/hevc", GoDecoderProfile.mime(Codec.CODEC_H265_HDR))
	}
}
