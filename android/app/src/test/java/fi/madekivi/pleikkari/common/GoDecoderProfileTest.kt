// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.common

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
	fun vtLowLatencyNeedsTheSettingAndTheGo()
	{
		assertTrue(GoDecoderProfile.enabled(true, "pacific"))
		assertFalse(GoDecoderProfile.enabled(false, "pacific"))
		assertFalse(GoDecoderProfile.enabled(true, "b0s"))
	}
}
