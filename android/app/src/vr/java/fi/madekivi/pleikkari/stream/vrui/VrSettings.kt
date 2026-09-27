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

	// PLE-844: the latency experiments, stored under the same keys as Preferences and the adb runners.
	fun experiment(key: String): Boolean
	fun setExperiment(key: String, on: Boolean)
	/** The rooms' MSAA samples, one of [VrSettings.ROOM_MSAA_CHOICES]. */
	var roomMsaa: Int
	/** Every experiment back to its shipped value. */
	fun resetExperiments()
	/** Reconnects to the same console with the stored settings. */
	fun restartStream()
	/** The stats overlay's lines for the sheet's readout; empty with no stream. */
	val readout: List<String>
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
	val codecs: Map<Preferences.Codec, String>,
	val experiments: String,
	/** A title for every key in [VrSettings.EXPERIMENTS]. */
	val experimentTitles: Map<String, String>,
	val roomMsaa: String,
	val roomMsaaValues: Map<Int, String>,
	val applyRestart: String,
	val resetDefaults: String
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

	/**
	 * PLE-844: the default-off input-lag A/B switches, by preference key. Every one is read when a
	 * cinema or session starts, so it applies from the next stream. Match 60 Hz is here for the reset;
	 * its row is the screen rows' own.
	 */
	const val MATCH_60HZ_KEY = "stream_go_vr_match_60hz"
	val EXPERIMENTS = listOf(
		"stream_go_vr_late_start",
		"stream_go_vr_latch_on_signal",
		"stream_go_vr_input_thread",
		"stream_go_vr_input_thread_flush",
		"stream_go_vr_frame_listener_thread",
		MATCH_60HZ_KEY,
		"stream_decoder_qcom_vt_low_latency",
		"stream_go_vr_warm_up",
		"stream_go_vr_hold_drain",
		"stream_go_vr_flush_eyes",
		"stream_go_vr_room_high_gpu")
	const val ROOM_MSAA_KEY = "stream_go_vr_room_msaa"
	val ROOM_MSAA_CHOICES = listOf(4, 2)
	const val READOUT_LINES = 3

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
		y += rowH + 3 * gap

		// PLE-844: the latency experiments, then a readout to compare and the buttons that act on them.
		items += VrLabel(Box(right.left, y, right.right, y + labelHeight), "${text.experiments} · ${text.nextStream}", TextRole.SECONDARY)
		y += labelHeight
		for(key in EXPERIMENTS.filter { it != MATCH_60HZ_KEY })
		{
			items += VrToggle(Box(right.left, y, right.right, y + rowH), text.experimentTitles[key] ?: key,
				{ stream.experiment(key) }) { stream.setExperiment(key, it) }.also { it.detail = text.nextStream }
			y += rowH + gap
		}
		items += VrLabel(Box(right.left, y, right.right, y + labelHeight), "${text.roomMsaa} · ${text.nextStream}", TextRole.SECONDARY)
		y += labelHeight
		choice(ROOM_MSAA_CHOICES, text.roomMsaaValues, { stream.roomMsaa }) { stream.roomMsaa = it }
		y += gap
		val lineH = VrUi.deg(1.9f)
		repeat(READOUT_LINES) {
			items += VrLabel(Box(right.left, y, right.right, y + lineH), "", TextRole.SECONDARY)
			y += lineH
		}
		y += gap
		items += VrButton(Box(right.left, y, right.right, y + rowH), text.applyRestart, ButtonStyle.PRIMARY) { stream.restartStream() }
		y += rowH + gap
		items += VrButton(Box(right.left, y, right.right, y + rowH), text.resetDefaults) { stream.resetExperiments() }

		val screen = VrScreen(VrMenu.WIDTH, VrMenu.HEIGHT, text.title, actions, listOf(VrList(right, items)))
		refresh(screen, menu, stream)
		return screen
	}

	/** The room's screen sliders follow [VrMenu.refresh]; the bitrate is always adjustable; the readout follows the stream. */
	fun refresh(screen: VrScreen, menu: VrMenuModel, stream: VrStreamProfileModel)
	{
		VrMenu.refresh(screen, menu)
		screen.all.filterIsInstance<VrSlider>().last().enabled = true
		val lines = stream.readout
		readout(screen).forEachIndexed { i, label -> label.text = lines.getOrElse(i) { "" } }
	}

	/** The readout's labels, just above the two buttons that end the list. */
	fun readout(screen: VrScreen) = screen.lists[0].children.let { it.subList(it.size - 2 - READOUT_LINES, it.size - 2) }.map { it as VrLabel }

	fun mbps(kbps: Int) = String.format(Locale.US, "%.1f Mbps", kbps / 1000f)
}
