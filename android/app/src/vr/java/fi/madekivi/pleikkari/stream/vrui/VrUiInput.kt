// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
package fi.madekivi.pleikkari.stream.vrui

import kotlin.math.abs
import kotlin.math.hypot

/**
 * PLE-722: one frame of the Go remote as the cinema's draw saw it (vr-ui-layers.cpp's output).
 * [x] and [y] are panel texels, meaningful when [onPanel]; the touchpad is 0..1, y down.
 */
data class PointerSample(
	val onPanel: Boolean,
	val x: Float,
	val y: Float,
	val trigger: Boolean,
	val touchpadClick: Boolean,
	val touching: Boolean,
	val touchX: Float,
	val touchY: Float,
	val timeNs: Long
)
{
	val select get() = trigger || touchpadClick
}

/**
 * The pointer over one [VrScreen] (§10.6 "Widget states"): hover with a margin against the
 * remote's jitter, press on select down, activation on select up still inside, a 3-degree move
 * turning a press into a drag (a slider's track takes it, a list scrolls, anything else
 * cancels), and touchpad scrolling of the list under the ray with a fling on release.
 */
class VrPointer
{
	var hovered: Widget? = null
		private set
	var pressed: Widget? = null
		private set
	private var pressPart = Part.WHOLE
	private var pressX = 0f
	private var pressY = 0f
	private var pressFromTouchpad = false
	private var pressTouchX = 0f
	private var pressTouchY = 0f
	private var dragging = false
	private var cancelled = false
	private var dragList: VrList? = null
	private var lastY = 0f
	private var selectWas = false
	private var touchingWas = false
	private var lastTouchY = 0f
	private var touchVelocity = 0f
	private var touchList: VrList? = null
	private var lastTimeNs = 0L
	/** A select press that started with nothing under the ray, e.g. with no panel open. */
	var pressedNothing = false
		private set

	/** Hover, press, activation and scroll for one frame; true when the panel must redraw. */
	fun sample(screen: VrScreen, s: PointerSample, activated: (Widget, Part) -> Unit = { w, p -> w.activate(p) }): Boolean
	{
		var changed = false
		val dt = if(lastTimeNs != 0L) ((s.timeNs - lastTimeNs) / 1e9f).coerceIn(0f, 0.1f) else 0f
		lastTimeNs = s.timeNs

		// Hover.
		val keep = hovered?.takeIf { s.onPanel && stillOver(it, s.x, s.y) }
		val next = keep ?: if(s.onPanel) screen.widgetAt(s.x, s.y)?.takeIf { it.interactive } else null
		if(next !== hovered)
		{
			hovered?.hovered = false
			next?.hovered = true
			hovered = next
			changed = true
		}

		// Select.
		val select = s.select
		if(select && !selectWas)
		{
			pressedNothing = false
			val target = hovered?.takeIf { it.enabled }
			if(target != null)
			{
				pressed = target
				pressPart = target.partAt(s.x, s.y)
				target.pressed = true
				target.pressedPart = pressPart
				pressX = s.x
				pressY = s.y
				pressFromTouchpad = s.touchpadClick && !s.trigger
				pressTouchX = s.touchX
				pressTouchY = s.touchY
				cancelled = false
				dragging = target.takesDrag(pressPart)
				if(dragging)
					target.dragTo(s.x)
				changed = true
			}
			else if(s.onPanel && screen.listAt(s.x, s.y) != null)
			{
				// Grab the empty space of a list: a drag scrolls it.
				dragList = screen.listAt(s.x, s.y)
				lastY = s.y
			}
			else
				pressedNothing = true
		}
		else if(select)
		{
			val target = pressed
			if(target != null && !cancelled)
			{
				if(dragging)
				{
					target.dragTo(s.x)
					changed = true
				}
				else if(s.onPanel && hypot(s.x - pressX, s.y - pressY) > VrUi.DRAG_THRESHOLD)
				{
					// A move turns the press into a drag: a list scrolls (following the ray from
					// where it was pressed), anything else lets go.
					cancelled = true
					target.pressed = false
					dragList = target.list
					lastY = pressY
					changed = true
				}
			}
			dragList?.let { list ->
				if(s.onPanel)
				{
					if(list.scrollBy(lastY - s.y))
						changed = true
					lastY = s.y
				}
			}
		}
		else if(selectWas)
		{
			val target = pressed
			if(target != null)
			{
				target.pressed = false
				if(!cancelled && !dragging && s.onPanel && stillOver(target, s.x, s.y))
					activated(target, pressPart)
				changed = true
			}
			pressed = null
			dragging = false
			cancelled = false
			dragList = null
			pressedNothing = false
		}
		selectWas = select

		// Touchpad: a finger dragged while the ray is on a list scrolls it 1:1, a full pad height
		// a viewport; a touchpad click that moved more than 15 % of the pad is a scroll, not a click.
		if(s.touching)
		{
			if(!touchingWas)
			{
				lastTouchY = s.touchY
				touchVelocity = 0f
				touchList = if(s.onPanel) screen.listAt(s.x, s.y) ?: hovered?.list else null
				touchList?.velocity = 0f
			}
			else
			{
				val list = touchList
				val dy = s.touchY - lastTouchY
				if(list != null)
				{
					val delta = -dy * list.viewport.height
					if(list.scrollBy(delta))
						changed = true
					if(dt > 0f)
						touchVelocity = 0.6f * touchVelocity + 0.4f * delta / dt
				}
				lastTouchY = s.touchY
				val moved = hypot(s.touchX - pressTouchX, s.touchY - pressTouchY)
				if(moved > TOUCH_CLICK_SLOP && pressFromTouchpad && pressed != null && !cancelled && !dragging)
				{
					cancelled = true
					pressed?.pressed = false
					changed = true
				}
			}
		}
		else if(touchingWas)
		{
			touchList?.velocity = touchVelocity
			touchList = null
		}
		touchingWas = s.touching

		for(list in screen.lists)
			if(list !== touchList && list.fling(dt))
				changed = true
		return changed
	}

	/**
	 * Lets go of everything, e.g. when the panel opens or closes. [selectHeld]: select is down
	 * right now (the click that opened the menu), so its release activates nothing.
	 */
	fun reset(selectHeld: Boolean = false)
	{
		selectWas = selectHeld
		hovered?.hovered = false
		pressed?.pressed = false
		hovered = null
		pressed = null
		dragging = false
		cancelled = false
		dragList = null
		touchList = null
		pressedNothing = false
	}

	private fun stillOver(widget: Widget, x: Float, y: Float): Boolean
	{
		val list = widget.list
		if(list != null && !list.viewport.contains(x, y, VrUi.HOVER_MARGIN))
			return false
		return widget.screenBox.contains(x, y, VrUi.HOVER_MARGIN)
	}

	companion object
	{
		const val TOUCH_CLICK_SLOP = 0.15f
	}
}

/**
 * Pad focus (§10.6): the D-pad or left stick moves to the nearest focusable widget in that
 * direction; A activates; left and right step a focused slider. The ring shows only after pad
 * input and hides once the pointer moves.
 */
class VrFocus
{
	var focused: Widget? = null
		private set
	var ringVisible = false
		private set

	fun set(widget: Widget?)
	{
		focused?.focused = false
		focused = widget
		widget?.focused = true
		widget?.list?.reveal(widget)
	}

	/** dx, dy: -1, 0 or 1. Returns true when something changed. */
	fun move(screen: VrScreen, dx: Int, dy: Int): Boolean
	{
		val current = focused?.takeIf { it.focusable && it in screen.all }
		if(current == null)
		{
			set(screen.all.firstOrNull { it.focusable })
			ringVisible = true
			return true
		}
		val wasVisible = ringVisible
		ringVisible = true
		if(dx != 0 && dy == 0 && current.step(dx))
			return true
		val from = current.screenBox
		// Android's FocusFinder rule: a candidate overlapping the current widget across the move
		// (in its beam) beats any that does not; then the nearest edge wins.
		var best: Widget? = null
		var bestScore = Float.MAX_VALUE
		var bestInBeam = false
		for(candidate in screen.all)
		{
			if(candidate === current || !candidate.focusable)
				continue
			val to = candidate.screenBox
			val centre = if(dx != 0) (to.centerX - from.centerX) * dx else (to.centerY - from.centerY) * dy
			if(centre <= 1f)
				continue
			val edge = if(dx > 0) to.left - from.right else if(dx < 0) from.left - to.right
				else if(dy > 0) to.top - from.bottom else from.top - to.bottom
			val orthogonal = if(dx != 0) gap(from.top, from.bottom, to.top, to.bottom) else gap(from.left, from.right, to.left, to.right)
			val inBeam = orthogonal == 0f
			// Out of the beam, only what lies wholly past the edge: down at the end of the settings
			// column must not land on Disconnect beside it.
			if(!inBeam && edge < 0f)
				continue
			val score = maxOf(edge, 0f) + 0.1f * centre + 2f * orthogonal
			if(inBeam && !bestInBeam || inBeam == bestInBeam && score < bestScore)
			{
				bestScore = score
				best = candidate
				bestInBeam = inBeam
			}
		}
		if(best == null)
			return !wasVisible
		set(best)
		return true
	}

	fun activate(): Boolean
	{
		val widget = focused?.takeIf { it.focusable } ?: return false
		ringVisible = true
		widget.activate(Part.WHOLE)
		return true
	}

	fun hideRing(): Boolean
	{
		val was = ringVisible
		ringVisible = false
		return was
	}

	fun reset()
	{
		set(null)
		ringVisible = false
	}

	private fun gap(aStart: Float, aEnd: Float, bStart: Float, bEnd: Float) = when
	{
		bEnd < aStart -> aStart - bEnd
		bStart > aEnd -> bStart - aEnd
		else -> 0f
	}
}

/**
 * A stick or hat held past half travel repeats as D-pad presses: after 400 ms, then every
 * 150 ms (§10.6). Pure timing; the host feeds it axis values and a clock.
 */
class VrStickRepeat
{
	private var direction = 0 to 0
	private var nextNs = 0L

	/** The direction to move now, or null. */
	fun update(x: Float, y: Float, nowNs: Long): Pair<Int, Int>?
	{
		val dir = when
		{
			abs(x) >= THRESHOLD && abs(x) >= abs(y) -> (if(x > 0) 1 else -1) to 0
			abs(y) >= THRESHOLD -> 0 to (if(y > 0) 1 else -1)
			else -> 0 to 0
		}
		if(dir != direction)
		{
			direction = dir
			if(dir == 0 to 0)
				return null
			nextNs = nowNs + FIRST_REPEAT_NS
			return dir
		}
		if(dir == 0 to 0 || nowNs < nextNs)
			return null
		nextNs = nowNs + REPEAT_NS
		return dir
	}

	val held get() = direction != 0 to 0

	companion object
	{
		const val THRESHOLD = 0.5f
		const val FIRST_REPEAT_NS = 400_000_000L
		const val REPEAT_NS = 150_000_000L
	}
}
