// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.stream.vrui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test

/** PLE-730: VR Home's console cards and status sheets (docs/design/vr-ui.md §10.5, §10.8). */
class VrHomeTest
{
	private val actions = mutableListOf<VrHomeAction>()
	private val settings = VrHomeButton("Settings", VrHomeAction.Settings)
	private val exit = VrHomeButton("Exit", VrHomeAction.Exit, ButtonStyle.DESTRUCTIVE)

	private fun card(i: Int, detail: String = "Ready", tone: VrCardTone = VrCardTone.READY) =
		VrConsoleCard("00:00:00:00:00:1$i", "PS5-46$i", detail, tone)

	private fun consoles(count: Int, focus: String? = null, detail: String = "Ready") = VrHomeState.Consoles(
		"Choose a PlayStation", (0 until count).map { card(it, detail) }, focus, "Play",
		"Link a console or change settings (Oculus TV screen)", listOf(settings, exit))

	private fun finding(message: String = "Looking for PS5-466 on this network…") = VrHomeState.Status("PS5-466", message,
		listOf("It must be on, or in rest mode, on the same network as this headset."), busy = true,
		buttons = listOf(VrHomeButton("Cancel", VrHomeAction.Cancel)), back = VrHomeAction.Cancel)

	private val notFound = VrHomeState.Status("PS5-466", "PS5-466 did not answer on this network.",
		listOf("Check that it is on or in rest mode, on the same network as this headset."), busy = false,
		buttons = listOf(VrHomeButton("Consoles", VrHomeAction.Consoles), VrHomeButton("Try again", VrHomeAction.Retry, ButtonStyle.PRIMARY)),
		back = VrHomeAction.Consoles)

	private fun build(state: VrHomeState) = VrHomePage.build(state) { actions += it }
	private fun VrScreen.button(label: String) = all.filterIsInstance<VrButton>().single { it.text == label }
	private fun VrScreen.cards() = all.filterIsInstance<VrCard>()

	private fun assertFitsWithoutOverlap(screen: VrScreen)
	{
		assertEquals(VrMenu.WIDTH, screen.width)
		assertEquals(VrMenu.HEIGHT, screen.height)
		val inner = Box(VrUi.BORDER, VrUi.BORDER, screen.width - VrUi.BORDER, screen.height - VrUi.BORDER)
		for(w in screen.all)
		{
			assertTrue("$w outside the panel", w.box.left >= inner.left && w.box.right <= inner.right && w.box.top >= VrUi.HEADER_HEIGHT)
			// A list child may lie below the viewport (it scrolls), never beside it.
			w.list?.let { assertTrue(w.box.left >= it.viewport.left && w.box.right <= it.viewport.right) }
				?: assertTrue("$w below the panel", w.box.bottom <= inner.bottom)
		}
		// Rows below a list's viewport are clipped, so the list is checked as its viewport.
		for(a in screen.all) for(b in screen.all)
			if(a !== b && a.list === b.list) assertFalse("$a overlaps $b", a.box.intersects(b.box))
		for(list in screen.lists) for(w in screen.widgets)
			assertFalse("$w overlaps the list", w.box.intersects(list.viewport))
	}

	@Test fun consoleCardsFitTheMenuPanelAndMeetTheSpec()
	{
		for(count in listOf(1, 2, 3, 8))
		{
			val screen = build(consoles(count)).screen
			assertFitsWithoutOverlap(screen)
			assertEquals(count, screen.cards().size)
			for(w in screen.all.filter { it.interactive })
				assertTrue("${w.javaClass.simpleName} ${w.box.height} tall", w.box.height >= VrUi.ROW_HEIGHT - 0.01f)
			for(b in screen.all.filterIsInstance<VrButton>())
				assertTrue(b.box.width >= VrUi.deg(8f))
			// The Play pill lies inside its card.
			for(c in screen.cards())
				assertTrue(c.box.contains(c.pillBox.left, c.pillBox.top) && c.box.contains(c.pillBox.right, c.pillBox.bottom))
			// The Oculus TV screen row is the list's last row.
			assertEquals("Link a console or change settings (Oculus TV screen)", (screen.lists.single().children.last() as VrButton).text)
		}
		assertEquals("two consoles and the TV row fit without scrolling", 0f, build(consoles(2)).screen.lists.single().maxScroll)
		assertTrue("eight consoles scroll", build(consoles(8)).screen.lists.single().maxScroll > 0f)
	}

	@Test fun everyControlSendsItsAction()
	{
		val screen = build(consoles(3)).screen
		screen.cards()[1].activate()
		screen.button("Link a console or change settings (Oculus TV screen)").activate()
		screen.button("Settings").activate()
		screen.button("Exit").activate()
		assertEquals(listOf(VrHomeAction.Play("00:00:00:00:00:11"), VrHomeAction.OculusTv, VrHomeAction.Settings, VrHomeAction.Exit), actions)
	}

	@Test fun theWholeCardIsTheTargetForThePointer()
	{
		val screen = build(consoles(2)).screen
		val pointer = VrPointer()
		var time = 0L
		fun at(x: Float, y: Float, trigger: Boolean = false) =
			pointer.sample(screen, PointerSample(true, x, y, trigger, false, false, 0.5f, 0.5f, 14_000_000L * ++time))
		val second = screen.cards()[1]
		// The name at the card's left, then the pill at its right.
		for(x in listOf(second.box.left + VrUi.deg(3f), second.pillBox.centerX))
		{
			at(x, second.box.centerY)
			assertSame(second, pointer.hovered)
			at(x, second.box.centerY, trigger = true)
			assertTrue(second.pressed)
			at(x, second.box.centerY)
		}
		assertEquals(List(2) { VrHomeAction.Play(second.id) }, actions)
	}

	@Test fun thePadStartsOnTheLastPlayedCardThenWalksToTheTvRowAndTheButtons()
	{
		val screen = build(consoles(3, focus = "00:00:00:00:00:12")).screen
		val focus = VrFocus()
		assertTrue(focus.move(screen, 0, 1))
		assertSame(screen.cards()[2], focus.focused)
		assertTrue(focus.ringVisible)
		focus.move(screen, 0, -1)
		assertSame(screen.cards()[1], focus.focused)
		focus.move(screen, 0, 1)
		focus.move(screen, 0, 1)
		assertSame(screen.lists.single().children.last(), focus.focused)
		focus.move(screen, 0, 1)
		assertSame(screen.button("Settings"), focus.focused)
		focus.move(screen, 1, 0)
		assertSame(screen.button("Exit"), focus.focused)
		// Up from the buttons returns to the list, and A on a card plays it.
		focus.move(screen, 0, -1)
		focus.move(screen, 0, -1)
		assertSame(screen.cards()[2], focus.focused)
		assertTrue(focus.activate())
		assertEquals(listOf(VrHomeAction.Play("00:00:00:00:00:12")), actions)
		// With no card named, the pad starts on the first card, not on Settings.
		val first = VrFocus()
		val plain = build(consoles(2)).screen
		first.move(plain, 0, 1)
		assertSame(plain.cards()[0], first.focused)
	}

	@Test fun padFocusScrollsAManyConsoleListToTheTvRow()
	{
		val screen = build(consoles(8)).screen
		val list = screen.lists.single()
		val focus = VrFocus()
		focus.move(screen, 0, 1)
		repeat(8) { focus.move(screen, 0, 1) }
		assertSame(list.children.last(), focus.focused)
		assertTrue(list.scroll > 0f)
		assertTrue(list.viewport.contains(list.viewport.left, list.children.last().screenBox.bottom - 1f))
	}

	@Test fun newWordsForTheSameCardsChangeThemInPlace()
	{
		val page = build(consoles(2, focus = "00:00:00:00:00:10"))
		val cards = page.screen.cards()
		val next = consoles(2, detail = "In rest mode · Play wakes it").let {
			it.copy(cards = it.cards.map { c -> c.copy(tone = VrCardTone.ASLEEP) })
		}
		assertTrue(page.update(next))
		assertSame(next, page.state)
		assertTrue(page.screen.cards().zip(cards).all { (a, b) -> a === b })
		assertEquals("In rest mode · Play wakes it", cards[0].detail)
		assertEquals(VrCardTone.ASLEEP, cards[1].tone)
		// Another set of consoles, or other buttons, is another page.
		assertFalse(page.update(consoles(3)))
		assertFalse(page.update(consoles(2).copy(buttons = listOf(exit))))
		assertFalse(page.update(finding()))
		assertNull(page.back)
	}

	@Test fun theFindingSheetHasASpinnerItsWordsAndCancel()
	{
		val page = build(finding())
		val screen = page.screen
		assertFitsWithoutOverlap(screen)
		assertNotNull(page.spinner)
		assertEquals(VrHomeAction.Cancel, page.back)
		val labels = screen.all.filterIsInstance<VrLabel>()
		assertEquals(listOf("Looking for PS5-466 on this network…", "It must be on, or in rest mode, on the same network as this headset."),
			labels.map { it.text })
		assertTrue("the message sits right of the spinner", labels[0].box.left >= page.spinner!!.box.right)
		val focus = VrFocus()
		focus.move(screen, 0, 1)
		assertSame(screen.button("Cancel"), focus.focused)
		focus.activate()
		assertEquals(listOf(VrHomeAction.Cancel), actions)
		// Looking turns to waking in place: the same label, the pad's focus kept.
		val message = labels[0]
		assertTrue(page.update(finding("Waking PS5-466 from rest mode…")))
		assertSame(message, page.screen.all.filterIsInstance<VrLabel>()[0])
		assertEquals("Waking PS5-466 from rest mode…", message.text)
		assertSame(screen.button("Cancel"), focus.focused)
	}

	@Test fun theNotFoundSheetOffersTryAgainFirstAndBackReturnsToTheConsoles()
	{
		val page = build(notFound)
		val screen = page.screen
		assertFitsWithoutOverlap(screen)
		assertNull(page.spinner)
		assertEquals(VrHomeAction.Consoles, page.back)
		// Right-aligned in order: Try again is the rightmost, and the pad starts on it (the primary).
		val consoles = screen.button("Consoles")
		val retry = screen.button("Try again")
		assertEquals(VrMenu.WIDTH - VrUi.deg(1f), retry.box.right, 0.01f)
		assertTrue(consoles.box.right < retry.box.left)
		val focus = VrFocus()
		focus.move(screen, 0, 1)
		assertSame(retry, focus.focused)
		focus.move(screen, -1, 0)
		assertSame(consoles, focus.focused)
		// A spinner or not is another page.
		assertFalse(page.update(notFound.copy(busy = true)))
	}

	@Test fun detailPastTheSheetsRoomIsCutNotOverlapped()
	{
		val long = notFound.copy(detail = (1..10).map { "Line $it" })
		val page = build(long)
		assertFitsWithoutOverlap(page.screen)
		assertEquals(1 + VrHome.MAX_DETAIL_LINES, page.screen.all.filterIsInstance<VrLabel>().size)
		assertTrue(page.update(long.copy(detail = (1..10).map { "Other $it" })))
		assertEquals("Other 1", page.screen.all.filterIsInstance<VrLabel>()[1].text)
	}
}
