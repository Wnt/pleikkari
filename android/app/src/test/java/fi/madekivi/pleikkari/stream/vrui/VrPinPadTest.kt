// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.stream.vrui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** PLE-731: the console login PIN pad's layout and typing (docs/design/vr-ui.md §10.7). */
class VrPinPadTest
{
	private val calls = mutableListOf<String>()
	private val model = object: VrPinPadModel
	{
		override fun submit(pin: String) { calls += "submit $pin" }
		override fun quit() { calls += "quit" }
	}
	private val text = VrPinPadText("Login PIN", "Wrong PIN", "Connect", "Quit", "Clear", "<")
	private val pad = VrPinPad(model, text, incorrect = false)
	private fun button(label: String) = pad.screen.all.filterIsInstance<VrButton>().single { it.text == label }
	private fun press(label: String) { button(label).activate(); pad.refresh() }
	private val slots get() = pad.screen.all.filterIsInstance<VrLabel>().single().text

	@Test fun everyWidgetFitsThePanelAndNothingOverlaps()
	{
		val inner = Box(VrUi.BORDER, VrUi.BORDER, VrPinPad.WIDTH - VrUi.BORDER, VrPinPad.HEIGHT - VrUi.BORDER)
		val widgets = pad.screen.all
		for(w in widgets)
			assertTrue("$w outside the panel", w.box.left >= inner.left && w.box.right <= inner.right &&
				w.box.top >= VrUi.HEADER_HEIGHT && w.box.bottom <= inner.bottom)
		for(a in widgets) for(b in widgets)
			if(a !== b) assertFalse("$a overlaps $b", a.box.intersects(b.box))
		assertEquals(14, widgets.count { it is VrButton })
	}

	@Test fun typesUpToFourDigitsThenConnects()
	{
		assertFalse(button("Connect").enabled)
		assertFalse(button("Clear").enabled)
		assertFalse(button("<").enabled)
		listOf("1", "2", "3", "0", "9").forEach { press(it) }
		assertEquals("1230", pad.digits)
		assertEquals("•  •  •  •", slots)
		assertTrue(button("Connect").enabled)
		press("Connect")
		assertEquals(listOf("submit 1230"), calls)
	}

	@Test fun backspaceClearAndQuit()
	{
		listOf("5", "6", "7").forEach { press(it) }
		press("<")
		assertEquals("56", pad.digits)
		assertEquals("•  •  _  _", slots)
		assertFalse(button("Connect").enabled)
		press("Connect")
		assertTrue(calls.isEmpty())
		press("Clear")
		assertEquals("", pad.digits)
		press("Quit")
		assertEquals(listOf("quit"), calls)
	}

	@Test fun aWrongPinChangesTheTitle()
	{
		assertEquals("Login PIN", pad.screen.title)
		assertEquals("Wrong PIN", VrPinPad(model, text, incorrect = true).screen.title)
	}

	/** PLE-757: a wrong PIN turns the title `stream_quit` and shakes it 0.3 degrees for 0.4 s (§10.7). */
	@Test fun wrongPinAlertsAndShakesTheTitle()
	{
		assertFalse(pad.screen.titleAlert)
		assertTrue(VrPinPad(model, text, incorrect = true).screen.titleAlert)
		val amp = VrUi.deg(0.3f)
		assertEquals(0f, VrPinPad.shakeShift(0), 0f)
		assertEquals(0f, VrPinPad.shakeShift(VrPinPad.SHAKE_NS), 0f)
		val samples = (1 until 400).map { VrPinPad.shakeShift(it * 1_000_000L) }
		assertTrue(samples.all { kotlin.math.abs(it) <= amp })
		assertTrue(samples.any { it > amp / 2 } && samples.any { it < -amp / 2 })
	}
}
