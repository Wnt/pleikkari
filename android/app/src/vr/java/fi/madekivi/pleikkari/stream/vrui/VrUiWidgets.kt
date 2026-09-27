// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
package fi.madekivi.pleikkari.stream.vrui

import fi.madekivi.pleikkari.stream.VrEnvironmentConfig
import fi.madekivi.pleikkari.stream.VrEnvironmentKind
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

/**
 * PLE-722: the Go VR UI toolkit's widget model (docs/design/vr-ui.md §10.5). Plain Kotlin with no
 * Android types, so layout, hit mapping, focus and widget behaviour run in JVM tests;
 * [VrUiPainter] draws it with Canvas. Every length is in panel texels, 16 to a degree.
 */
object VrUi {
	const val TEXELS_PER_DEGREE = 16f
	fun deg(degrees: Float) = degrees * TEXELS_PER_DEGREE
	/** The panel texture's transparent border (§10.2: cylinder TextureRect clamping). */
	const val BORDER = 2f
	val PANEL_CORNER = deg(0.5f)
	val ROW_CORNER = deg(0.35f)
	val ROW_HEIGHT = deg(3.6f)
	val ROW_GAP = deg(0.4f)
	val HEADER_HEIGHT = deg(4.5f)
	/** A widget keeps its hover until the ray is this far outside it: the remote's jitter (§10.6). */
	val HOVER_MARGIN = deg(0.3f)
	/** A press that moves this far becomes a drag (Skybox's 3 degrees). */
	val DRAG_THRESHOLD = deg(3f)

	/** vr-ui-panel.c's pleikkari_vr_panel_radius: 2 m, in front of the picture, never nearer than 0.6 m. */
	fun panelRadius(screenDistanceM: Float) = min(2f, 0.8f * screenDistanceM).coerceAtLeast(0.6f)

	/** The plain screen is PLE-602's fixed 3 m strip; the rooms use the configured distance. */
	fun panelRadius(config: VrEnvironmentConfig) =
		panelRadius(if(config.environment == VrEnvironmentKind.PLAIN) 3f else config.clamped().screenDistanceCm / 100f)
}

data class Box(val left: Float, val top: Float, val right: Float, val bottom: Float)
{
	val width get() = right - left
	val height get() = bottom - top
	val centerX get() = (left + right) / 2
	val centerY get() = (top + bottom) / 2
	fun contains(x: Float, y: Float, margin: Float = 0f) =
		x >= left - margin && x <= right + margin && y >= top - margin && y <= bottom + margin
	fun offset(dx: Float, dy: Float) = Box(left + dx, top + dy, right + dx, bottom + dy)
	fun intersects(other: Box) = left < other.right && other.left < right && top < other.bottom && other.top < bottom
}

enum class TextRole { TITLE, LABEL, SECONDARY }
enum class ButtonStyle { PRIMARY, SECONDARY, DESTRUCTIVE }

/** The part of a composite widget (a slider's step buttons and track) a press landed on. */
enum class Part { WHOLE, MINUS, TRACK, PLUS }

abstract class Widget(var box: Box)
{
	/** The scrolling list this widget lies in, if any; set by [VrList]. */
	var list: VrList? = null
	var enabled = true
	var hovered = false
	var pressed = false
	var pressedPart = Part.WHOLE
	var focused = false
	/** Reacts to the pointer and the pad (the larger reticle; focus can land on it). */
	open val interactive get() = true
	val focusable get() = interactive && enabled
	/** Which part of the widget the panel point is on. */
	open fun partAt(x: Float, y: Float) = Part.WHOLE
	/** Select released on the widget, or the pad's A on it. */
	open fun activate(part: Part = Part.WHOLE) {}
	/** The pad's left or right on the focused widget; true when it used the step itself. */
	open fun step(direction: Int) = false
	/** A drag on [Part.TRACK] of a widget that takes it (a slider), at panel x. */
	open fun dragTo(x: Float) {}
	open fun takesDrag(part: Part) = false
	/** The box on the panel, after its list's scroll. */
	val screenBox get() = list?.let { box.offset(0f, -it.scroll) } ?: box
}

class VrLabel(box: Box, var text: String, val role: TextRole = TextRole.SECONDARY): Widget(box)
{
	override val interactive get() = false
}

class VrButton(box: Box, var text: String, val style: ButtonStyle = ButtonStyle.SECONDARY,
	private val onClick: () -> Unit): Widget(box)
{
	override fun activate(part: Part) = onClick()
}

/** One segment of a choice row (the room picker, §10.5); a group of these is the choice. */
class VrSegment(box: Box, val text: String, private val selected: () -> Boolean,
	private val onSelect: () -> Unit): Widget(box)
{
	val isSelected get() = selected()
	override fun activate(part: Part) = onSelect()
}

/** A row that toggles a switch; the whole row is the target. */
class VrToggle(box: Box, val label: String, private val value: () -> Boolean,
	private val onChange: (Boolean) -> Unit): Widget(box)
{
	val checked get() = value()
	/** A second line under the label, e.g. when the change takes effect. */
	var detail: String? = null
	override fun activate(part: Part) = onChange(!value())
	val switchBox get() = Box(box.right - VrUi.deg(4.6f), box.centerY - VrUi.deg(1f), box.right - VrUi.deg(1f), box.centerY + VrUi.deg(1f))
}

/**
 * A row with a label and value on the left, then minus, a track and plus: the step buttons
 * matter on a 3DoF laser (§10.5). Snaps to [stepSize]; D-pad left and right step.
 */
class VrSlider(box: Box, val label: String, val min: Int, val max: Int, val stepSize: Int,
	private val value: () -> Int, val format: (Int) -> String,
	private val onChange: (Int) -> Unit): Widget(box)
{
	val current get() = value()
	val labelWidth = VrUi.deg(12f)
	val minusBox get() = Box(box.left + labelWidth, box.top, box.left + labelWidth + VrUi.ROW_HEIGHT, box.bottom)
	val plusBox get() = Box(box.right - VrUi.ROW_HEIGHT, box.top, box.right, box.bottom)
	val trackBox get() = Box(minusBox.right + VrUi.deg(1.2f), box.top, plusBox.left - VrUi.deg(1.2f), box.bottom)
	/** 0..1 along the track. */
	val fraction get() = if(max > min) (current - min).toFloat() / (max - min) else 0f

	override fun partAt(x: Float, y: Float): Part
	{
		// Parts are tested in the widget's own box (list scroll removed by the caller).
		return when
		{
			x < minusBox.left -> Part.WHOLE
			x <= minusBox.right -> Part.MINUS
			x >= plusBox.left -> Part.PLUS
			else -> Part.TRACK
		}
	}

	override fun takesDrag(part: Part) = part == Part.TRACK
	override fun activate(part: Part)
	{
		when(part)
		{
			Part.MINUS -> step(-1)
			Part.PLUS -> step(1)
			else -> {}
		}
	}

	/** To the next multiple of [stepSize] that way, so an odd stored value lands on the grid. */
	override fun step(direction: Int): Boolean
	{
		val grid = if(direction > 0) Math.floorDiv(current, stepSize) + 1 else -Math.floorDiv(-current, stepSize) - 1
		set(grid * stepSize)
		return true
	}

	override fun dragTo(x: Float)
	{
		val track = trackBox
		val f = ((x - track.left) / track.width).coerceIn(0f, 1f)
		set(((min + f * (max - min)) / stepSize).roundToInt() * stepSize)
	}

	private fun set(v: Int)
	{
		val snapped = v.coerceIn(min, max)
		if(snapped != current)
			onChange(snapped)
	}
}

/** A vertical scroll of widgets clipped to [viewport] (§10.5: List). Its children's boxes are laid out unscrolled. */
class VrList(val viewport: Box, val children: List<Widget>)
{
	var scroll = 0f
		private set
	val contentHeight = (children.maxOfOrNull { it.box.bottom } ?: viewport.top) - viewport.top
	val maxScroll get() = max(0f, contentHeight - viewport.height)
	/** Touchpad fling, texels per second. */
	var velocity = 0f

	init
	{
		children.forEach { it.list = this }
	}

	/** Returns whether the scroll moved. */
	fun scrollBy(dy: Float): Boolean
	{
		val next = (scroll + dy).coerceIn(0f, maxScroll)
		val moved = next != scroll
		scroll = next
		return moved
	}

	/** Scrolls just enough to show [widget] whole, as pad focus moves onto it. */
	fun reveal(widget: Widget): Boolean
	{
		val margin = VrUi.ROW_GAP
		return when
		{
			widget.box.top - scroll < viewport.top -> scrollBy(widget.box.top - margin - viewport.top - scroll)
			widget.box.bottom - scroll > viewport.bottom -> scrollBy(widget.box.bottom + margin - viewport.bottom - scroll)
			else -> false
		}
	}

	fun visible(widget: Widget) = widget.screenBox.intersects(viewport)

	/** Advances a fling by [seconds]: friction 5 per second (Meta's sample ScrollManager, §10.6). */
	fun fling(seconds: Float): Boolean
	{
		if(abs(velocity) < FLING_STOP)
		{
			velocity = 0f
			return false
		}
		val moved = scrollBy(velocity * seconds)
		velocity *= kotlin.math.exp(-FLING_FRICTION * seconds)
		if(!moved)
			velocity = 0f
		return moved
	}

	companion object
	{
		const val FLING_FRICTION = 5f
		const val FLING_STOP = 8f
	}
}

/**
 * One panel's content: its widgets (lists included) and the pointer and pad state over them.
 * [initialFocus]: where the pad's first move lands (PLE-730: Home's last-played card), else the first focusable.
 */
class VrScreen(val width: Int, val height: Int, val title: String, val widgets: List<Widget>, val lists: List<VrList> = emptyList(),
	val initialFocus: Widget? = null, val titleAlert: Boolean = false)
{
	/** PLE-757: the title's horizontal offset in panel units; [VrUiHost] shakes an alert title once when shown. */
	var titleShift = 0f
	val all: List<Widget> get() = widgets + lists.flatMap { it.children }

	/** The widget under a panel point: a list's children only inside its viewport. */
	fun widgetAt(x: Float, y: Float, margin: Float = 0f): Widget?
	{
		for(list in lists)
		{
			if(!list.viewport.contains(x, y))
				continue
			list.children.firstOrNull { it.screenBox.contains(x, y, margin) }?.let { return it }
		}
		return widgets.firstOrNull { it.box.contains(x, y, margin) }
	}

	fun listAt(x: Float, y: Float) = lists.firstOrNull { it.viewport.contains(x, y) }
}
