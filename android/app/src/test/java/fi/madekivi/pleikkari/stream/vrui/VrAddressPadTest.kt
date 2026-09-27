// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.stream.vrui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** PLE-733: the manual console address pad's layout, pages and typing (docs/design/vr-ui.md §10.7). */
class VrAddressPadTest
{
	private val calls = mutableListOf<String>()
	private val model = object: VrAddressPadModel
	{
		override fun save(address: String) { calls += "save $address" }
		override fun cancel() { calls += "cancel" }
	}
	private val text = VrAddressPadText("Console address", "Save", "Cancel", "Clear", "<", "abc", "123")
	private val pad = VrAddressPad(model, text)
	private fun button(label: String) = pad.screen.all.filterIsInstance<VrButton>().single { it.text == label }
	private fun press(label: String) { button(label).activate(); pad.refresh() }
	private val field get() = pad.screen.all.filterIsInstance<VrLabel>().single().text

	@Test fun everyWidgetFitsThePanelAndNothingOverlaps()
	{
		val inner = Box(VrUi.BORDER, VrUi.BORDER, VrAddressPad.WIDTH - VrUi.BORDER, VrAddressPad.HEIGHT - VrUi.BORDER)
		val widgets = pad.screen.all
		for(w in widgets)
			assertTrue("$w outside the panel", w.box.left >= inner.left && w.box.right <= inner.right &&
				w.box.top >= VrUi.HEADER_HEIGHT && w.box.bottom <= inner.bottom)
		for(a in widgets) for(b in widgets)
			if(a !== b) assertFalse("$a overlaps $b", a.box.intersects(b.box))
	}

	@Test fun typesAnIpAddressAndSaves()
	{
		assertFalse(button("Save").enabled)
		"192.168.1.164".forEach { press(it.toString()) }
		assertEquals("192.168.1.164_", field)
		press("Save")
		assertEquals(listOf("save 192.168.1.164"), calls)
	}

	@Test fun lettersPageTypesAHostName()
	{
		press("abc")
		"ps-5.lan".forEach { c -> if(c == '5') { press("123"); press("5"); press("abc") } else press(c.toString()) }
		assertEquals("ps-5.lan", pad.address)
		assertTrue(pad.lettersPage)
		assertEquals(26 + 2, pad.screen.all.count { it is VrButton && it.enabled && it.text.length == 1 && it.text != "<" })
	}

	@Test fun backspaceClearCancelAndAStartingAddress()
	{
		val edit = VrAddressPad(model, text, "10.0.0.2")
		assertEquals("10.0.0.2", edit.address)
		press("1"); press("2"); press("<")
		assertEquals("1", pad.address)
		press("Clear")
		assertEquals("", pad.address)
		assertFalse(button("<").enabled)
		press("Cancel")
		assertEquals(listOf("cancel"), calls)
	}

	@Test fun aLongAddressShowsItsTail()
	{
		press("abc")
		repeat(40) { press("a") }
		press("b")
		assertEquals(VrAddressPad.FIELD_CHARS + 1, field.length)
		assertTrue(field.startsWith("…") && field.endsWith("ab_"))
	}
}
