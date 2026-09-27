// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.stream.vrui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** PLE-761: the `debug.pleikkari.vr_ui_layers` spec. */
class VrUiLayerDebugTest
{
	@Test
	fun emptyIsTheShippedPanels()
	{
		assertTrue(VrUiLayerDebug.parse("").isDefault)
		assertTrue(VrUiLayerDebug.parse(" , nonsense, tpd=x").isDefault)
	}

	@Test
	fun parsesEverySwitch()
	{
		val debug = VrUiLayerDebug.parse("quad, overlay,tpd=24,filter,panels=1")
		assertEquals(VrUiLayerDebug(quad = true, overlay = true, texelsPerDegree = 24f, filterExpensive = true, maxPanels = 1), debug)
		assertEquals(1.5f, debug.texelScale, 1e-6f)
	}

	@Test
	fun clampsRanges()
	{
		assertEquals(32f, VrUiLayerDebug.parse("tpd=100").texelsPerDegree, 0f)
		assertEquals(1, VrUiLayerDebug.parse("panels=0").maxPanels)
		assertEquals(2, VrUiLayerDebug.parse("panels=5").maxPanels)
	}
}
