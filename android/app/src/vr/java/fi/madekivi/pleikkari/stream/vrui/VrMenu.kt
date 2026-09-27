// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
package fi.madekivi.pleikkari.stream.vrui

import fi.madekivi.pleikkari.stream.VrEnvironmentConfig
import fi.madekivi.pleikkari.stream.VrEnvironmentKind
import java.util.Locale

/**
 * PLE-722: what the in-stream VR menu shows and changes (§10.8). [StreamVrActivity] backs it
 * with the stored settings and the cinema; tests back it with plain fields.
 */
interface VrMenuModel
{
	val leaveIsExit: Boolean
	fun resume()
	fun recentre()
	fun leave()
	var room: VrEnvironmentKind
	var screenDistanceCm: Int
	var screenWidthCm: Int
	var match60Hz: Boolean
	var statsOverlay: Boolean
}

/** The menu's words; [StreamVrActivity] fills them from resources, tests from literals. */
data class VrMenuText(
	val title: String,
	val resume: String,
	val recentre: String,
	val disconnect: String,
	val exit: String,
	val room: String,
	val rooms: Map<VrEnvironmentKind, String>,
	val distance: String,
	val size: String,
	val roomsOnly: String,
	val match60Hz: String,
	val match60HzDetail: String,
	val statsOverlay: String
)

object VrMenu
{
	/** §10.3: 50 x 28 degrees. */
	const val WIDTH = 800
	const val HEIGHT = 448
	const val DISTANCE_STEP_CM = 25
	const val WIDTH_STEP_CM = 25

	/** Two columns: actions on the left, settings in a scrolling list on the right. */
	fun build(model: VrMenuModel, text: VrMenuText): VrScreen
	{
		val pad = VrUi.deg(1f)
		val top = VrUi.HEADER_HEIGHT
		val bottom = HEIGHT - pad
		val leftColumn = Box(pad, top, pad + VrUi.deg(13.75f), bottom)
		val right = Box(leftColumn.right + pad, top, WIDTH - pad, bottom)
		val rowH = VrUi.ROW_HEIGHT
		val gap = VrUi.ROW_GAP

		fun row(y: Float, box: Box = leftColumn) = Box(box.left, y, box.right, y + rowH)
		val actions = listOf(
			VrButton(row(top), text.resume, ButtonStyle.PRIMARY) { model.resume() },
			VrButton(row(top + rowH + 2 * gap), text.recentre) { model.recentre() },
			VrButton(Box(leftColumn.left, bottom - rowH, leftColumn.right, bottom),
				if(model.leaveIsExit) text.exit else text.disconnect, ButtonStyle.DESTRUCTIVE) { model.leave() }
		)

		val items = mutableListOf<Widget>()
		var y = top
		val labelHeight = VrUi.deg(1.9f)
		items += VrLabel(Box(right.left, y, right.right, y + labelHeight), text.room, TextRole.SECONDARY)
		y += labelHeight
		// §10.5 Choice: at least 7 degrees a segment, so three to a line in this column.
		val rooms = VrEnvironmentKind.values().toList()
		val perLine = 3
		val segmentW = (right.width - (perLine - 1) * gap) / perLine
		rooms.chunked(perLine).forEach { line ->
			line.forEachIndexed { i, kind ->
				val left = right.left + i * (segmentW + gap)
				items += VrSegment(Box(left, y, left + segmentW, y + rowH), text.rooms[kind] ?: kind.value,
					selected = { model.room == kind }) { model.room = kind }
			}
			y += rowH + gap
		}
		y += gap
		val roomsOnly = { model.room == VrEnvironmentKind.PLAIN }
		val distance = VrSlider(row(y, right), text.distance, VrEnvironmentConfig.SCREEN_DISTANCE_CM_MIN,
			VrEnvironmentConfig.SCREEN_DISTANCE_CM_MAX, DISTANCE_STEP_CM, { model.screenDistanceCm },
			{ if(roomsOnly()) text.roomsOnly else metres(it) }) { model.screenDistanceCm = it }
		items += distance
		y += rowH + gap
		val width = VrSlider(row(y, right), text.size, VrEnvironmentConfig.SCREEN_WIDTH_CM_MIN,
			VrEnvironmentConfig.SCREEN_WIDTH_CM_MAX, WIDTH_STEP_CM, { model.screenWidthCm },
			{ if(roomsOnly()) text.roomsOnly else metres(it) }) { model.screenWidthCm = it }
		items += width
		y += rowH + gap
		items += VrToggle(row(y, right), text.match60Hz, { model.match60Hz }) { model.match60Hz = it }
			.also { it.detail = text.match60HzDetail }
		y += rowH + gap
		items += VrToggle(row(y, right), text.statsOverlay, { model.statsOverlay }) { model.statsOverlay = it }

		val screen = VrScreen(WIDTH, HEIGHT, text.title, actions, listOf(VrList(right, items)))
		refresh(screen, model)
		return screen
	}

	/** Enables what the current room allows: screen distance and size apply to the rooms, not the plain screen. */
	fun refresh(screen: VrScreen, model: VrMenuModel)
	{
		val plain = model.room == VrEnvironmentKind.PLAIN
		screen.all.filterIsInstance<VrSlider>().forEach { it.enabled = !plain }
	}

	fun metres(cm: Int) = String.format(Locale.US, "%.2f m", cm / 100f)
}
