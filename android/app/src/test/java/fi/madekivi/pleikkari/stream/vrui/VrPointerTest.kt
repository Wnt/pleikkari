// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.stream.vrui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test

/** PLE-722: the pointer's widget states (docs/design/vr-ui.md §10.6). */
class VrPointerTest
{
	private var clicks = mutableListOf<String>()
	private var sliderValue = 50
	private val button = VrButton(Box(0f, 0f, 200f, 58f), "Resume") { clicks += "Resume" }
	private val slider = VrSlider(Box(300f, 0f, 800f, 58f), "Distance", 0, 100, 5, { sliderValue }, { "$it" }) { sliderValue = it }
	private val rows = (0 until 10).map { i ->
		VrButton(Box(300f, 100f + i * 64f, 800f, 158f + i * 64f), "Row $i") { clicks += "Row $i" }
	}
	private val list = VrList(Box(300f, 100f, 800f, 400f), rows)
	private val screen = VrScreen(800, 448, "Menu", listOf(button, slider), listOf(list))
	private val pointer = VrPointer()
	private var time = 0L

	private fun at(x: Float, y: Float, trigger: Boolean = false, touchpad: Boolean = false,
		touching: Boolean = false, touchY: Float = 0.5f, onPanel: Boolean = true): Boolean
	{
		time += 14_000_000L
		return pointer.sample(screen, PointerSample(onPanel, x, y, trigger, touchpad, touching, 0.5f, touchY, time))
	}

	@Test fun hoverHoldsAgainstJitterJustOutsideTheWidget()
	{
		assertTrue(at(100f, 30f))
		assertSame(button, pointer.hovered)
		assertTrue(button.hovered)
		// 0.3 degrees is 4.8 texels: 3 texels past the edge keeps the hover, 8 loses it.
		assertFalse(at(203f, 30f))
		assertSame(button, pointer.hovered)
		assertTrue(at(208f, 30f))
		assertNull(pointer.hovered)
		assertFalse(button.hovered)
	}

	@Test fun activatesOnReleaseInsideOnly()
	{
		at(100f, 30f)
		at(100f, 30f, trigger = true)
		assertTrue(button.pressed)
		assertTrue(clicks.isEmpty())
		at(102f, 31f)
		assertEquals(listOf("Resume"), clicks)
		assertFalse(button.pressed)
		// Released after leaving the button: cancelled.
		at(100f, 30f, trigger = true)
		at(260f, 30f, trigger = true)
		at(260f, 30f)
		assertEquals(listOf("Resume"), clicks)
	}

	@Test fun aPressThatMovesThreeDegreesIsADragNotAClick()
	{
		at(100f, 20f)
		at(100f, 20f, touchpad = true)
		at(100f, 20f + VrUi.DRAG_THRESHOLD + 1f, touchpad = true)
		assertFalse(button.pressed)
		at(100f, 30f)
		assertTrue(clicks.isEmpty())
	}

	@Test fun bareSelectOverNothingActivatesNothing()
	{
		at(250f, 30f)
		at(250f, 30f, trigger = true)
		assertTrue(pointer.pressedNothing)
		at(250f, 30f)
		assertTrue(clicks.isEmpty())
	}

	@Test fun sliderStepsFromItsButtonsAndFollowsATrackDrag()
	{
		val plus = slider.plusBox
		at(plus.centerX, plus.centerY)
		at(plus.centerX, plus.centerY, trigger = true)
		at(plus.centerX, plus.centerY)
		assertEquals(55, sliderValue)
		val minus = slider.minusBox
		at(minus.centerX, minus.centerY, trigger = true)
		at(minus.centerX, minus.centerY)
		assertEquals(50, sliderValue)
		val track = slider.trackBox
		at(track.left + 1f, track.centerY, trigger = true)
		assertEquals(0, sliderValue)
		// The track takes the drag however far it goes; it snaps to steps.
		at(track.left + track.width * 0.62f, track.centerY + 5f, trigger = true)
		assertEquals(60, sliderValue)
		at(track.right + 40f, track.centerY, trigger = true)
		assertEquals(100, sliderValue)
		at(track.right + 40f, track.centerY)
		assertEquals(100, sliderValue)
	}

	@Test fun draggingAListRowScrollsTheListAndCancelsTheRow()
	{
		at(500f, 300f)
		assertSame(rows[3], pointer.hovered)
		at(500f, 300f, trigger = true)
		at(500f, 240f, trigger = true)
		assertFalse(rows[3].pressed)
		assertEquals(60f - 0f, list.scroll, 0.01f)
		at(500f, 200f, trigger = true)
		assertEquals(100f, list.scroll, 0.01f)
		at(500f, 200f)
		assertTrue(clicks.isEmpty())
		// A row now under the ray lies where the scroll put it.
		assertSame(rows[3], screen.widgetAt(500f, 200f))
	}

	@Test fun listRowsOutsideTheViewportCannotBeHit()
	{
		assertNull(screen.widgetAt(500f, 430f))
		// Scrolled 30: row 0 spans 70..128, but above the viewport's top (100) it is clipped.
		list.scrollBy(30f)
		assertNull(screen.widgetAt(500f, 80f))
		assertSame(rows[0], screen.widgetAt(500f, 110f))
		// Row 0 now lies above the viewport (0..58); row 1 (64..122) is under y = 100.
		list.scrollBy(70f)
		assertSame(rows[1], screen.widgetAt(500f, 100f))
	}

	@Test fun touchpadScrollsTheListUnderTheRayAndFlings()
	{
		at(500f, 200f, touching = true, touchY = 0.5f)
		// Finger up by a fifth of the pad: the content follows it up by a fifth of the viewport.
		at(500f, 200f, touching = true, touchY = 0.3f)
		assertEquals(0.2f * list.viewport.height, list.scroll, 0.01f)
		at(500f, 200f, touching = true, touchY = 0.2f)
		at(500f, 200f, touching = false)
		assertTrue(list.velocity > 0f)
		val before = list.scroll
		repeat(10) { at(500f, 200f) }
		assertTrue(list.scroll > before)
		// Friction 5 per second brings it to rest.
		repeat(200) { at(500f, 200f) }
		assertEquals(0f, list.velocity, 0f)
	}

	@Test fun aTouchpadClickThatSwipesIsNotAClick()
	{
		at(100f, 30f, touching = true, touchY = 0.5f)
		at(100f, 30f, touchpad = true, touching = true, touchY = 0.5f)
		at(100f, 30f, touchpad = true, touching = true, touchY = 0.3f)
		at(100f, 30f, touching = true, touchY = 0.3f)
		assertTrue(clicks.isEmpty())
		// Within 15 % of the pad it still clicks.
		at(100f, 30f, touchpad = true, touching = true, touchY = 0.3f)
		at(100f, 30f, touchpad = true, touching = true, touchY = 0.35f)
		at(100f, 30f, touching = true, touchY = 0.35f)
		assertEquals(listOf("Resume"), clicks)
	}

	@Test fun theClickThatOpenedTheMenuActivatesNothingOnRelease()
	{
		pointer.reset(selectHeld = true)
		at(100f, 30f, trigger = true)
		assertFalse(button.pressed)
		at(100f, 30f)
		assertTrue(clicks.isEmpty())
	}

	@Test fun disabledWidgetsTakeHoverButNotPress()
	{
		slider.enabled = false
		val plus = slider.plusBox
		at(plus.centerX, plus.centerY)
		assertSame(slider, pointer.hovered)
		at(plus.centerX, plus.centerY, trigger = true)
		at(plus.centerX, plus.centerY)
		assertEquals(50, sliderValue)
	}

	@Test fun leavingThePanelDropsHover()
	{
		at(100f, 30f)
		at(-1f, -1f, onPanel = false)
		assertNull(pointer.hovered)
	}
}
