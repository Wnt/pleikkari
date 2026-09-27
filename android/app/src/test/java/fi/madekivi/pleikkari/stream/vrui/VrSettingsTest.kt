// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.stream.vrui

import fi.madekivi.pleikkari.common.Preferences
import fi.madekivi.pleikkari.stream.VrEnvironmentKind
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test

/** PLE-732: the Go Settings sheet's layout, bindings and pad navigation (docs/design/vr-ui.md §10.8). */
class VrSettingsTest
{
	private class Model: VrMenuModel, VrStreamProfileModel
	{
		val calls = mutableListOf<String>()
		override val leaveIsExit = false
		override fun resume() { calls += "resume" }
		override fun recentre() { calls += "recentre" }
		override fun leave() { calls += "leave" }
		override fun openSettings() { calls += "settings" }
		override var room = VrEnvironmentKind.PLAIN
		override var screenDistanceCm = 300
		override var screenWidthCm = 419
		override var match60Hz = false
		override var statsOverlay = false
		override var resolution = Preferences.Resolution.RES_720P
		override var fps = Preferences.FPS.FPS_60
		override var bitrateKbps = 0
		override val bitrateAutoKbps = 10000
		override var codec = Preferences.Codec.CODEC_H265
		override fun back() { calls += "back" }
		override fun openOculusTv() { calls += "oculus tv" }
	}

	private val menuText = VrMenuText("Pleikkari cinema", "Resume", "Recentre", "Disconnect", "Exit", "Settings", "Room",
		VrEnvironmentKind.values().associateWith { it.value.replaceFirstChar(Char::uppercase) },
		"Distance", "Size", "Rooms only", "Match 60 Hz", "From the next stream", "Stats overlay")
	private val text = VrSettingsText("Settings", "Back", "Oculus TV screen", "Stream", "From the next stream",
		Preferences.Resolution.values().associateWith { it.value }, Preferences.FPS.values().associateWith { "${it.value} fps" },
		"Bitrate", "Auto (%s)", Preferences.Codec.values().associateWith { it.value.uppercase() })
	private val model = Model()
	private val sheet = VrSettings.build(model, model, menuText, text)
	private fun button(label: String) = sheet.all.filterIsInstance<VrButton>().single { it.text == label }
	private fun segment(label: String) = sheet.all.filterIsInstance<VrSegment>().single { it.text == label }
	private fun slider(label: String) = sheet.all.filterIsInstance<VrSlider>().single { it.label == label }

	@Test fun everyWidgetFitsThePanelAndNothingOverlaps()
	{
		val inner = Box(VrUi.BORDER, VrUi.BORDER, VrMenu.WIDTH - VrUi.BORDER, VrMenu.HEIGHT - VrUi.BORDER)
		val widgets = sheet.all
		for(w in widgets)
		{
			assertTrue("$w outside the panel", w.box.left >= inner.left && w.box.right <= inner.right && w.box.top >= inner.top)
			w.list?.let { assertTrue(w.box.left >= it.viewport.left && w.box.right <= it.viewport.right) }
				?: assertTrue(w.box.bottom <= inner.bottom)
		}
		for(a in widgets) for(b in widgets)
			if(a !== b) assertFalse("$a overlaps $b", a.box.intersects(b.box))
		for(w in widgets.filter { it.interactive })
			assertTrue(w.box.height >= VrUi.deg(3.6f) - 0.01f)
		for(s in widgets.filterIsInstance<VrSegment>())
			assertTrue("${s.text} ${s.box.width} wide", s.box.width >= VrUi.deg(7f))
	}

	@Test fun roomScreenAndStatsRowsAreTheMenus()
	{
		val menu = VrMenu.build(model, menuText)
		fun labels(screen: VrScreen) = screen.lists[0].children.map {
			when(it) { is VrSegment -> it.text; is VrSlider -> it.label; is VrToggle -> it.label; is VrLabel -> it.text; else -> "" }
		}
		assertEquals(labels(menu), labels(sheet).take(labels(menu).size))
	}

	@Test fun actionsCallTheModel()
	{
		button("Back").activate()
		button("Oculus TV screen").activate()
		assertEquals(listOf("back", "oculus tv"), model.calls)
		// The Oculus TV row sits at the bottom of the left column, like Disconnect in the menu.
		assertEquals(VrMenu.HEIGHT - VrUi.deg(1f), button("Oculus TV screen").box.bottom, 0.01f)
	}

	@Test fun streamProfile()
	{
		assertTrue(segment("720p").isSelected)
		segment("1080p").activate()
		assertEquals(Preferences.Resolution.RES_1080P, model.resolution)
		segment("30 fps").activate()
		assertEquals(Preferences.FPS.FPS_30, model.fps)
		segment("H264").activate()
		assertEquals(Preferences.Codec.CODEC_H264, model.codec)
		// The bitrate stays adjustable on the plain screen, whose screen sliders are off.
		assertFalse(slider("Distance").enabled)
		assertTrue(slider("Bitrate").enabled)
		assertEquals("Auto (10.0 Mbps)", slider("Bitrate").format(0))
		slider("Bitrate").step(1)
		assertEquals(2500, model.bitrateKbps)
		assertEquals("2.5 Mbps", slider("Bitrate").format(model.bitrateKbps))
		slider("Bitrate").step(-1)
		assertEquals(0, model.bitrateKbps)
	}

	@Test fun padFocusReachesTheLastRow()
	{
		val focus = VrFocus()
		assertTrue(focus.move(sheet, 0, 1))
		assertSame(button("Back"), focus.focused)
		focus.move(sheet, 0, 1)
		assertSame(button("Oculus TV screen"), focus.focused)
		focus.set(segment("Plain"))
		repeat(20) { focus.move(sheet, 0, 1) }
		assertSame(slider("Bitrate"), focus.focused)
		assertTrue(sheet.lists[0].scroll > 0f)
	}
}
