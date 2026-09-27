// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.stream.vrui

import fi.madekivi.pleikkari.lib.StreamStatsEvent
import fi.madekivi.pleikkari.stream.VrEnvironmentConfig
import fi.madekivi.pleikkari.stream.VrEnvironmentKind
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test

/** PLE-722: the in-stream VR menu's layout, bindings and pad navigation (docs/design/vr-ui.md §10.5-§10.8). */
class VrMenuTest
{
	private class Model: VrMenuModel
	{
		val calls = mutableListOf<String>()
		override var leaveIsExit = false
		override fun resume() { calls += "resume" }
		override fun recentre() { calls += "recentre" }
		override fun leave() { calls += "leave" }
		override var room = VrEnvironmentKind.PLAIN
		override var screenDistanceCm = 300
		override var screenWidthCm = 419
		override var match60Hz = false
		override var statsOverlay = false
	}

	private val text = VrMenuText("Pleikkari cinema", "Resume", "Recentre", "Disconnect", "Exit", "Room",
		VrEnvironmentKind.values().associateWith { it.value.replaceFirstChar(Char::uppercase) },
		"Distance", "Size", "Rooms only", "Match 60 Hz", "From the next stream", "Stats overlay")
	private val model = Model()
	private val menu = VrMenu.build(model, text)
	private fun button(label: String) = menu.all.filterIsInstance<VrButton>().single { it.text == label }
	private fun segment(label: String) = menu.all.filterIsInstance<VrSegment>().single { it.text == label }
	private fun toggle(label: String) = menu.all.filterIsInstance<VrToggle>().single { it.label == label }
	private fun slider(label: String) = menu.all.filterIsInstance<VrSlider>().single { it.label == label }

	@Test fun everyWidgetFitsThePanelInsideItsBorderAndNothingOverlaps()
	{
		val inner = Box(VrUi.BORDER, VrUi.BORDER, VrMenu.WIDTH - VrUi.BORDER, VrMenu.HEIGHT - VrUi.BORDER)
		val widgets = menu.all
		for(w in widgets)
		{
			assertTrue("$w outside the panel", w.box.left >= inner.left && w.box.right <= inner.right && w.box.top >= inner.top)
			// A list child may lie below the viewport (it scrolls), never beside it.
			w.list?.let { assertTrue(w.box.left >= it.viewport.left && w.box.right <= it.viewport.right) }
				?: assertTrue(w.box.bottom <= inner.bottom)
		}
		for(a in widgets) for(b in widgets)
			if(a !== b) assertFalse("$a overlaps $b", a.box.intersects(b.box))
	}

	@Test fun sizesMeetTheSpec()
	{
		for(w in menu.all.filter { it.interactive })
			assertTrue("${w.javaClass.simpleName} ${w.box.height} tall", w.box.height >= VrUi.deg(3.6f) - 0.01f)
		for(b in menu.all.filterIsInstance<VrButton>())
			assertTrue(b.box.width >= VrUi.deg(8f))
		// Choice segments at least 7 degrees wide, every room offered.
		val segments = menu.all.filterIsInstance<VrSegment>()
		assertEquals(VrEnvironmentKind.values().size, segments.size)
		for(s in segments)
			assertTrue(s.box.width >= VrUi.deg(7f))
		assertEquals(1, menu.lists.size)
		assertTrue("the settings column scrolls", menu.lists[0].maxScroll > 0f)
	}

	@Test fun actionsCallTheModel()
	{
		button("Resume").activate()
		button("Recentre").activate()
		button("Disconnect").activate()
		assertEquals(listOf("resume", "recentre", "leave"), model.calls)
		model.leaveIsExit = true
		assertEquals("Exit", VrMenu.build(model, text).all.filterIsInstance<VrButton>().last().text)
	}

	@Test fun roomsSlidersAndToggles()
	{
		// The plain screen has a fixed size, so its sliders are off until a room is picked.
		assertFalse(slider("Distance").enabled)
		assertEquals("Rooms only", slider("Distance").format(slider("Distance").current))
		assertTrue(segment("Plain").isSelected)
		segment("Cinema").activate()
		VrMenu.refresh(menu, model)
		assertEquals(VrEnvironmentKind.CINEMA, model.room)
		assertTrue(segment("Cinema").isSelected)
		assertFalse(segment("Plain").isSelected)
		assertTrue(slider("Distance").enabled)
		assertEquals("3.00 m", slider("Distance").format(model.screenDistanceCm))
		slider("Distance").step(1)
		assertEquals(325, model.screenDistanceCm)
		slider("Size").step(-1)
		assertEquals(400, model.screenWidthCm) // snaps into the 25 cm steps
		slider("Distance").dragTo(slider("Distance").trackBox.right)
		assertEquals(VrEnvironmentConfig.SCREEN_DISTANCE_CM_MAX, model.screenDistanceCm)
		toggle("Match 60 Hz").activate()
		toggle("Stats overlay").activate()
		assertTrue(model.match60Hz)
		assertTrue(model.statsOverlay)
		assertTrue(toggle("Match 60 Hz").checked)
		assertEquals("From the next stream", toggle("Match 60 Hz").detail)
	}

	@Test fun padFocusStartsOnResumeAndWalksBothColumns()
	{
		val focus = VrFocus()
		assertTrue(focus.move(menu, 0, 1))
		assertSame(button("Resume"), focus.focused)
		assertTrue(focus.ringVisible)
		focus.move(menu, 0, 1)
		assertSame(button("Recentre"), focus.focused)
		focus.move(menu, 0, 1)
		assertSame(button("Disconnect"), focus.focused)
		// Right from Resume reaches the room choice beside it.
		focus.set(button("Resume"))
		focus.move(menu, 1, 0)
		assertTrue(focus.focused is VrSegment)
		assertTrue(focus.activate())
		assertEquals(VrEnvironmentKind.values().first { text.rooms[it] == (focus.focused as VrSegment).text }, model.room)
	}

	@Test fun padLeftAndRightStepAFocusedSliderAndDownScrollsToTheLastRow()
	{
		model.room = VrEnvironmentKind.VOID
		VrMenu.refresh(menu, model)
		val focus = VrFocus()
		focus.set(slider("Distance"))
		assertTrue(focus.move(menu, 1, 0))
		assertEquals(325, model.screenDistanceCm)
		assertSame(slider("Distance"), focus.focused)
		val list = menu.lists[0]
		repeat(6) { focus.move(menu, 0, 1) }
		assertSame(toggle("Stats overlay"), focus.focused)
		assertTrue("focus scrolls its row into view", list.viewport.contains(0f + list.viewport.left, toggle("Stats overlay").screenBox.bottom - 1f))
		assertTrue(list.scroll > 0f)
		// A disabled slider is skipped.
		model.room = VrEnvironmentKind.PLAIN
		VrMenu.refresh(menu, model)
		focus.set(toggle("Match 60 Hz"))
		repeat(2) { focus.move(menu, 0, -1) }
		assertSame(segment("Plain"), focus.focused)
		// Past the top of the column up stays put; left reaches the actions.
		focus.move(menu, 0, -1)
		assertSame(segment("Plain"), focus.focused)
		focus.move(menu, -1, 0)
		assertSame(button("Resume"), focus.focused)
	}

	@Test fun focusRingHidesForThePointer()
	{
		val focus = VrFocus()
		focus.move(menu, 0, 1)
		assertTrue(focus.hideRing())
		assertFalse(focus.ringVisible)
		assertNotNull(focus.focused)
	}

	@Test fun stickRepeatsAfter400msThenEvery150ms()
	{
		val stick = VrStickRepeat()
		assertNull(stick.update(0.2f, 0f, 0L))
		assertEquals(0 to 1, stick.update(0.1f, 0.9f, 0L))
		assertNull(stick.update(0f, 0.9f, 399_000_000L))
		assertEquals(0 to 1, stick.update(0f, 0.9f, 400_000_000L))
		assertNull(stick.update(0f, 0.9f, 500_000_000L))
		assertEquals(0 to 1, stick.update(0f, 0.9f, 550_000_000L))
		assertNull(stick.update(0f, 0f, 560_000_000L))
		assertFalse(stick.held)
		assertEquals(-1 to 0, stick.update(-0.7f, 0.2f, 600_000_000L))
	}

	@Test fun statsPanelSitsInsideThePlainPicturesTopLeftCorner()
	{
		val (x, y) = VrStatsPanel.anchor(VrEnvironmentConfig())
		assertEquals(-(40.1f - 12.5f - 1f), x, 0.01f)
		assertEquals(21.5f - 3.5f - 1f, y, 0.01f)
		// A small room screen puts it no further out than the middle.
		val (sx, sy) = VrStatsPanel.anchor(VrEnvironmentConfig(environment = VrEnvironmentKind.VOID, screenWidthCm = 50, screenDistanceCm = 1200))
		assertEquals(0f, sx, 0.01f)
		assertEquals(0f, sy, 0.01f)
		assertEquals(2f, VrUi.panelRadius(VrEnvironmentConfig()), 0f)
		assertEquals(0.8f, VrUi.panelRadius(VrEnvironmentConfig(environment = VrEnvironmentKind.CINEMA, screenDistanceCm = 100)), 0.001f)
		// The nearest screen the settings allow is 0.8 m; the panel radius never goes under 0.6 m.
		assertEquals(0.64f, VrUi.panelRadius(VrEnvironmentConfig(environment = VrEnvironmentKind.CINEMA, screenDistanceCm = 50)), 0.001f)
		assertEquals(0.6f, VrUi.panelRadius(0.5f), 0.001f)
	}

	@Test fun statsLines()
	{
		assertEquals(listOf("panel 72 Hz", "preview: no console", ""), VrStatsPanel.lines(null, 72f, true))
		val stats = StreamStatsEvent(
			1000, 10_000, 60, 59, 4_200, 0, 1, 2,
			0, 3, 0, 1_000, 100, 0, 0,
			0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
		)
		val lines = VrStatsPanel.lines(stats, 60f, false)
		assertEquals("stream 60.0 fps · decoder 59.0 fps · panel 60 Hz", lines[0])
		assertEquals("decode 4.2 ms · this stream: lost 3, dropped 3", lines[1])
		assertEquals(3, VrStatsPanel.screen(lines).widgets.size)
	}
}
