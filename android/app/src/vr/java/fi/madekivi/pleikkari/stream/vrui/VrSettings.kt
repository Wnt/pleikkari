// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
package fi.madekivi.pleikkari.stream.vrui

import fi.madekivi.pleikkari.common.Preferences
import java.util.Locale

/**
 * PLE-732: the stream profile the Go Settings sheet changes. Every setter is stored at once and
 * takes effect from the next stream, as on the phone's Settings screen.
 */
interface VrStreamProfileModel
{
	var resolution: Preferences.Resolution
	var fps: Preferences.FPS
	/** kbit/s; 0 is the automatic bitrate for the resolution ([bitrateAutoKbps]). */
	var bitrateKbps: Int
	val bitrateAutoKbps: Int
	var codec: Preferences.Codec
	/** Back to the in-stream menu. */
	fun back()
	/** Everything else: the 2D app on the Oculus TV screen. This ends the VR task and its stream. */
	fun openOculusTv()
}

data class VrSettingsText(
	val title: String,
	val back: String,
	val oculusTv: String,
	val stream: String,
	val nextStream: String,
	val resolutions: Map<Preferences.Resolution, String>,
	val fpsValues: Map<Preferences.FPS, String>,
	val bitrate: String,
	val bitrateAuto: String,
	val codecs: Map<Preferences.Codec, String>
)

/**
 * PLE-732: the Go Settings sheet (docs/design/vr-ui.md §10.8), in the menu panel. Left: Back,
 * and "Oculus TV screen" at the bottom for everything the Go subset leaves out. Right, one
 * scrolling column: the in-stream menu's room and screen rows, then the stream profile.
 */
object VrSettings
{
	const val BITRATE_STEP_KBPS = 2500
	const val BITRATE_MAX_KBPS = 100000

	fun build(menu: VrMenuModel, stream: VrStreamProfileModel, menuText: VrMenuText, text: VrSettingsText): VrScreen
	{
		val pad = VrUi.deg(1f)
		val top = VrUi.HEADER_HEIGHT
		val bottom = VrMenu.HEIGHT - pad
		val leftColumn = Box(pad, top, pad + VrUi.deg(13.75f), bottom)
		val right = Box(leftColumn.right + pad, top, VrMenu.WIDTH - pad, bottom)
		val rowH = VrUi.ROW_HEIGHT
		val gap = VrUi.ROW_GAP

		val actions = listOf(
			VrButton(Box(leftColumn.left, top, leftColumn.right, top + rowH), text.back, ButtonStyle.PRIMARY) { stream.back() },
			VrButton(Box(leftColumn.left, bottom - rowH, leftColumn.right, bottom), text.oculusTv) { stream.openOculusTv() }
		)

		val items = mutableListOf<Widget>()
		var y = VrMenu.screenRows(menu, menuText, right, top, items) + 3 * gap
		val labelHeight = VrUi.deg(1.9f)
		items += VrLabel(Box(right.left, y, right.right, y + labelHeight), "${text.stream} · ${text.nextStream}", TextRole.SECONDARY)
		y += labelHeight
		fun <T> choice(values: List<T>, labels: Map<T, String>, get: () -> T, set: (T) -> Unit)
		{
			val segmentW = (right.width - (values.size - 1) * gap) / values.size
			values.forEachIndexed { i, v ->
				val left = right.left + i * (segmentW + gap)
				items += VrSegment(Box(left, y, left + segmentW, y + rowH), labels[v] ?: v.toString(),
					selected = { get() == v }) { set(v) }
			}
			y += rowH + gap
		}
		choice(Preferences.Resolution.values().toList(), text.resolutions, { stream.resolution }) { stream.resolution = it }
		choice(Preferences.FPS.values().toList(), text.fpsValues, { stream.fps }) { stream.fps = it }
		choice(Preferences.Codec.values().toList(), text.codecs, { stream.codec }) { stream.codec = it }
		items += VrSlider(Box(right.left, y, right.right, y + rowH), text.bitrate, 0, BITRATE_MAX_KBPS, BITRATE_STEP_KBPS,
			{ stream.bitrateKbps }, { if(it == 0) String.format(Locale.US, text.bitrateAuto, mbps(stream.bitrateAutoKbps)) else mbps(it) })
			{ stream.bitrateKbps = it }

		val screen = VrScreen(VrMenu.WIDTH, VrMenu.HEIGHT, text.title, actions, listOf(VrList(right, items)))
		refresh(screen, menu)
		return screen
	}

	/** The room's screen sliders follow [VrMenu.refresh]; the bitrate is always adjustable. */
	fun refresh(screen: VrScreen, menu: VrMenuModel)
	{
		VrMenu.refresh(screen, menu)
		screen.all.filterIsInstance<VrSlider>().last().enabled = true
	}

	fun mbps(kbps: Int) = String.format(Locale.US, "%.1f Mbps", kbps / 1000f)
}
