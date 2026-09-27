// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
package fi.madekivi.pleikkari.stream.vrui

import android.content.Context
import android.os.Handler
import android.os.HandlerThread
import android.os.Process
import android.util.Log
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.Surface
import java.util.Locale
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.math.abs

/**
 * PLE-722: runs the Go VR UI (docs/design/vr-ui.md §10) on its own `GoVrUi` thread. The cinema's
 * GoCinema thread only copies [control] into each frame and hands back what the remote did
 * ([frame]); every widget, every Canvas redraw and every key decision happens here, so the head
 * loop never waits on the UI. Redraws happen only on change.
 *
 * Panels: [MENU] (the in-stream menu, summoned at the gaze) and [STATS] (the stats readout,
 * anchored to the picture).
 */
class VrUiHost(
	context: Context,
	private val buildMenu: () -> VrScreen,
	private val refreshMenu: (VrScreen) -> Unit,
	/** Main thread: the menu opened or closed (the activity neutralises the pad's console input). */
	private val menuChanged: (Boolean) -> Unit,
	/** Share and Options, as the console mapping has them: held together they open the menu. */
	private val shareKey: Int,
	private val optionsKey: Int,
	/** Debug builds: `debug.pleikkari.vr_pointer` = "x,y" degrees, read while the menu is open. */
	private val debugPointer: (() -> String)?
)
{
	private val thread = HandlerThread("GoVrUi", Process.THREAD_PRIORITY_DEFAULT).apply { start() }
	private val handler = Handler(thread.looper)
	private val main = Handler(context.mainLooper)
	private val painter = VrUiPainter(context)
	private val surfaces = arrayOfNulls<Surface>(2)
	private var menu: VrScreen? = null
	/** PLE-731: a screen shown on the menu panel in place of the menu (the PIN pad) until [closeModal]. */
	private class Modal(val screen: VrScreen, val refresh: () -> Unit, val back: () -> Unit)
	private var modal: Modal? = null
	/** What the menu panel shows: the modal screen when there is one. */
	private val current get() = modal?.screen ?: menu
	private fun refreshCurrent(screen: VrScreen) = modal?.refresh?.invoke() ?: refreshMenu(screen)
	private val pointer = VrPointer()
	private val focus = VrFocus()
	private val stick = VrStickRepeat()
	private var scrollStick = 0f
	private var statsLines: List<String> = emptyList()
	private var lastSample: PointerSample? = null
	private var lastSelect = false
	private var lastHoverName: String? = null

	@Volatile var menuOpen = false
		private set
	@Volatile var statsOpen = false
	/** A click with no menu open opens it, except while a Library launch picks its console. */
	@Volatile var clickOpensMenu = true
	@Volatile private var interactive = false
	@Volatile private var pointerOverride: FloatArray? = null
	@Volatile var radius = 2f
	@Volatile var statsAnchor = 0f to 0f
	private val placeRequest = AtomicBoolean(false)
	private val recentreRequest = AtomicBoolean(false)
	private val environmentRequest = AtomicBoolean(false)

	// Redraw accounting, logged once a second.
	private var redraws = 0
	private var redrawNs = 0L
	private var redrawMaxNs = 0L
	private var windowStartNs = System.nanoTime()

	// ---- GoCinema thread ----

	/** The panel Surfaces, from VrCinemaNative.createPanel. */
	fun attach(menuSurface: Surface?, statsSurface: Surface?) = handler.post {
		surfaces[MENU] = menuSurface
		surfaces[STATS] = statsSurface
		Log.i(TAG, "Panels attached: menu ${menuSurface != null}, stats ${statsSurface != null}")
		// Drawn once while closed, so the layer's first frame on opening already has its pixels.
		val screen = menu ?: buildMenu().also { menu = it }
		refreshMenu(screen)
		modal?.refresh?.invoke()
		redrawMenu()
		if(statsOpen)
			redrawStats()
	}

	/** Fills the frame's VrUiFrame control slots (vr-cinema.cpp's draw). */
	fun control(out: FloatArray)
	{
		val override = pointerOverride
		var flags = 0
		if(interactive) flags = flags or FLAG_INTERACTIVE
		if(placeRequest.getAndSet(false)) flags = flags or FLAG_PLACE
		if(override != null && menuOpen) flags = flags or FLAG_DEBUG_POINTER
		out[0] = ((if(menuOpen) 1 shl MENU else 0) or (if(statsOpen) 1 shl STATS else 0)).toFloat()
		out[1] = flags.toFloat()
		out[2] = override?.get(0) ?: 0f
		out[3] = override?.get(1) ?: 0f
		out[4] = radius
		out[5] = statsAnchor.first
		out[6] = statsAnchor.second
	}

	/** What the remote did this frame (vr-ui-layers.cpp's output); posted to the UI thread when it changed. */
	fun frame(out: FloatArray, nowNs: Long)
	{
		val panel = out[0].toInt()
		val onPanel = panel == MENU && out[1] in -0.05f..1.05f && out[2] in -0.05f..1.05f
		val buttons = out[3].toInt()
		val sample = PointerSample(
			onPanel = onPanel,
			x = if(panel == MENU) out[1] * VrMenu.WIDTH else -1f,
			y = if(panel == MENU) out[2] * VrMenu.HEIGHT else -1f,
			trigger = buttons and BUTTON_TRIGGER != 0,
			touchpadClick = buttons and BUTTON_TOUCHPAD != 0,
			touching = out[4] != 0f,
			touchX = out[5],
			touchY = out[6],
			timeNs = nowNs)
		val last = lastSample
		if(last != null && last.copy(timeNs = nowNs) == sample)
			return
		lastSample = sample
		handler.post { pointerSample(sample) }
	}

	/** The remote's Back: opens the menu, or closes it (the only level so far); on a modal screen, its own Back. */
	fun back() = handler.post {
		val m = modal
		when
		{
			m != null -> m.back()
			menuOpen -> close("back")
			else -> open("back")
		}
	}

	/**
	 * PLE-731: shows [screen] on the menu panel, opened at the gaze, until [closeModal]; Back and
	 * the pad's B call [back] instead of closing it. [refresh] runs after every activation.
	 */
	fun showModal(screen: VrScreen, refresh: () -> Unit, back: () -> Unit) = handler.post {
		modal = Modal(screen, refresh, back)
		Log.i(TAG, "Modal '${screen.title}' shown")
		if(menuOpen)
		{
			pointer.reset(lastSelect)
			focus.reset()
			placeRequest.set(true)
			redrawMenu()
		}
		else
			open("modal")
	}

	/** PLE-731: takes the modal screen down and closes the panel. */
	fun closeModal(reason: String) = handler.post {
		if(modal == null)
			return@post
		modal = null
		Log.i(TAG, "Modal closed ($reason)")
		closeNow(reason)
		// Drawn once while closed, as [attach] does, so the menu opens with its own pixels.
		redrawMenu()
	}

	fun takeRecentre() = recentreRequest.getAndSet(false)
	fun takeEnvironmentChange() = environmentRequest.getAndSet(false)

	// ---- Main thread ----

	/** A key from Android. True when the UI took it (the console never sees it). */
	fun key(event: KeyEvent): Boolean
	{
		val code = event.keyCode
		val down = event.action == KeyEvent.ACTION_DOWN
		if(!menuOpen)
		{
			if(code == KeyEvent.KEYCODE_MENU)
			{
				if(down && event.repeatCount == 0)
					handler.post { open("pad Menu key") }
				return true
			}
			if(code == shareKey || code == optionsKey)
				handler.post { chord(code, down) }
			return false
		}
		if(!isPad(event) && code != KeyEvent.KEYCODE_MENU)
			return false
		handler.post { menuKey(code, down, event.repeatCount) }
		return true
	}

	/** Sticks and the hat while the menu is open; the console gets nothing. */
	fun motion(event: MotionEvent): Boolean
	{
		if(!menuOpen || event.source and InputDevice.SOURCE_CLASS_JOYSTICK != InputDevice.SOURCE_CLASS_JOYSTICK)
			return false
		val hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X)
		val hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y)
		val x = if(abs(hatX) > abs(event.getAxisValue(MotionEvent.AXIS_X))) hatX else event.getAxisValue(MotionEvent.AXIS_X)
		val y = if(abs(hatY) > abs(event.getAxisValue(MotionEvent.AXIS_Y))) hatY else event.getAxisValue(MotionEvent.AXIS_Y)
		val scroll = event.getAxisValue(MotionEvent.AXIS_RZ)
		handler.post {
			lastStick = x to y
			scrollStick = if(abs(scroll) > STICK_DEAD_ZONE) scroll else 0f
			stick.update(x, y, System.nanoTime())?.let { (dx, dy) -> padMove(dx, dy) }
			scheduleTick()
		}
		return true
	}

	// ---- Any thread ----

	fun stats(lines: List<String>) = handler.post {
		if(lines != statsLines)
		{
			statsLines = lines
			if(statsOpen)
				redrawStats()
		}
	}

	fun showStats(show: Boolean)
	{
		statsOpen = show
		handler.post {
			Log.i(TAG, "Stats overlay ${if(show) "on" else "off"}")
			if(show)
				redrawStats()
		}
	}

	fun requestEnvironment() = environmentRequest.set(true)

	/** Recentre the picture and summon the menu again at the gaze (the Recentre item). */
	fun recentre()
	{
		recentreRequest.set(true)
		placeRequest.set(true)
		Log.i(TAG, "Recentre")
	}

	fun close(reason: String) = handler.post {
		// PLE-731: a modal screen stays until [closeModal].
		if(modal == null)
			closeNow(reason)
	}

	private fun closeNow(reason: String)
	{
		if(!menuOpen)
			return
		menuOpen = false
		interactive = false
		pointer.reset(lastSelect)
		focus.reset()
		chordDown.clear()
		Log.i(TAG, "Menu closed ($reason)")
		main.post { menuChanged(false) }
	}

	/** Stops the UI thread; call before the panels' swapchains go (the cinema's destroy). */
	fun shutdown()
	{
		handler.removeCallbacksAndMessages(null)
		thread.quitSafely()
		thread.join()
	}

	// ---- GoVrUi thread ----

	private fun open(reason: String)
	{
		if(menuOpen)
			return
		val screen = menu ?: buildMenu().also { menu = it }
		refreshMenu(screen)
		modal?.refresh?.invoke()
		pointer.reset(lastSelect)
		focus.reset()
		menuOpen = true
		placeRequest.set(true)
		Log.i(TAG, "Menu opened ($reason)")
		main.post { menuChanged(true) }
		redrawMenu()
		pollDebugPointer()
	}

	private fun pointerSample(s: PointerSample)
	{
		val previous = lastSelect
		lastSelect = s.select
		if(!menuOpen)
		{
			if(s.select && !previous && clickOpensMenu)
				open(if(s.trigger) "trigger" else "touchpad click")
			return
		}
		val screen = current ?: return
		var changed = pointer.sample(screen, s) { widget, part ->
			Log.i(TAG, "Activate ${name(widget)}${if(part != Part.WHOLE) " (${part.name.lowercase(Locale.US)})" else ""}")
			widget.activate(part)
			refreshCurrent(screen)
		}
		if(s.onPanel && focus.ringVisible && pointer.hovered != null && focus.hideRing())
			changed = true
		interactive = pointer.hovered != null
		val hover = pointer.hovered?.let { name(it) }
		if(hover != lastHoverName)
		{
			lastHoverName = hover
			Log.i(TAG, "Hover ${hover ?: "none"}")
		}
		if(changed)
			redrawMenu()
		if(screen.lists.any { it.velocity != 0f })
			scheduleTick()
	}

	private val chordDown = mutableSetOf<Int>()
	private val chordOpen = Runnable { open("pad Share+Options") }

	private fun chord(code: Int, down: Boolean)
	{
		if(down) chordDown += code else chordDown -= code
		handler.removeCallbacks(chordOpen)
		if(shareKey in chordDown && optionsKey in chordDown)
			handler.postDelayed(chordOpen, CHORD_MS)
	}

	private fun menuKey(code: Int, down: Boolean, repeat: Int)
	{
		val screen = current ?: return
		when(code)
		{
			KeyEvent.KEYCODE_DPAD_UP, KeyEvent.KEYCODE_DPAD_DOWN, KeyEvent.KEYCODE_DPAD_LEFT, KeyEvent.KEYCODE_DPAD_RIGHT ->
			{
				val now = System.nanoTime()
				if(!down || (repeat > 0 && now - lastKeyRepeatNs < VrStickRepeat.REPEAT_NS))
					return
				lastKeyRepeatNs = now
				when(code)
				{
					KeyEvent.KEYCODE_DPAD_UP -> padMove(0, -1)
					KeyEvent.KEYCODE_DPAD_DOWN -> padMove(0, 1)
					KeyEvent.KEYCODE_DPAD_LEFT -> padMove(-1, 0)
					else -> padMove(1, 0)
				}
			}
			KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_DPAD_CENTER, KeyEvent.KEYCODE_ENTER ->
				if(!down)
				{
					focus.focused?.let { Log.i(TAG, "Activate ${name(it)} (pad)") }
					if(focus.activate())
					{
						refreshCurrent(screen)
						redrawMenu()
					}
				}
			KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_BACK, KeyEvent.KEYCODE_MENU ->
				if(!down) modal?.back?.invoke() ?: closeNow("pad")
			KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_R1 ->
				if(down)
				{
					val list = focus.focused?.list ?: screen.lists.firstOrNull() ?: return
					val page = list.viewport.height * if(code == KeyEvent.KEYCODE_BUTTON_L1) -1 else 1
					if(list.scrollBy(page))
						redrawMenu()
				}
		}
	}
	private var lastKeyRepeatNs = 0L

	private fun padMove(dx: Int, dy: Int)
	{
		val screen = current ?: return
		val before = focus.focused
		if(focus.move(screen, dx, dy))
		{
			if(focus.focused !== before)
				Log.i(TAG, "Focus ${focus.focused?.let { name(it) } ?: "none"}")
			else if(focus.focused is VrSlider)
				Log.i(TAG, "Step ${name(focus.focused!!)}")
			refreshCurrent(screen)
			redrawMenu()
		}
	}

	private var ticking = false
	private var lastTickNs = 0L
	private val tick = object: Runnable
	{
		override fun run()
		{
			ticking = false
			val screen = current ?: return
			if(!menuOpen)
				return
			val now = System.nanoTime()
			val dt = ((now - lastTickNs) / 1e9f).coerceIn(0f, 0.1f)
			lastTickNs = now
			var changed = false
			// Held stick: repeats come from VrStickRepeat's clock, so feed it the last value again.
			if(stick.held)
				lastStick?.let { (x, y) -> stick.update(x, y, now)?.let { (dx, dy) -> padMove(dx, dy) } }
			if(scrollStick != 0f)
			{
				val list = focus.focused?.list ?: screen.lists.firstOrNull()
				if(list != null && list.scrollBy(scrollStick * STICK_SCROLL_PER_S * dt))
					changed = true
			}
			if(screen.lists.any { it.fling(dt) })
				changed = true
			if(changed)
				redrawMenu()
			if(stick.held || scrollStick != 0f || screen.lists.any { it.velocity != 0f })
				scheduleTick()
		}
	}
	private var lastStick: Pair<Float, Float>? = null

	private fun scheduleTick()
	{
		if(ticking)
			return
		ticking = true
		lastTickNs = System.nanoTime()
		handler.postDelayed(tick, TICK_MS)
	}

	private fun pollDebugPointer()
	{
		val read = debugPointer ?: return
		handler.postDelayed(object: Runnable
		{
			override fun run()
			{
				val value = read().split(',').mapNotNull { it.trim().toFloatOrNull() }
				val next = if(value.size == 2) floatArrayOf(value[0], value[1]) else null
				if(!(next contentEquals pointerOverride))
				{
					pointerOverride = next
					Log.i(TAG, "Debug pointer ${next?.let { "at ${it[0]}, ${it[1]} degrees" } ?: "off"}")
				}
				if(menuOpen)
					handler.postDelayed(this, DEBUG_POINTER_POLL_MS)
			}
		}, 0)
	}

	private fun redrawMenu()
	{
		val screen = current ?: return
		draw(surfaces[MENU]) { painter.draw(it, screen, focus) }
	}

	private fun redrawStats() = draw(surfaces[STATS]) { painter.draw(it, VrStatsPanel.screen(statsLines), VrFocus()) }

	private inline fun draw(surface: Surface?, paint: (android.graphics.Canvas) -> Unit)
	{
		if(surface == null || !surface.isValid)
			return
		val start = System.nanoTime()
		try
		{
			val canvas = surface.lockHardwareCanvas()
			try
			{
				paint(canvas)
			}
			finally
			{
				surface.unlockCanvasAndPost(canvas)
			}
		}
		catch(e: RuntimeException)
		{
			// The swapchain went away under us (the cinema stopping); the next start builds new ones.
			Log.w(TAG, "Panel redraw failed: $e")
			return
		}
		val took = System.nanoTime() - start
		redraws++
		redrawNs += took
		redrawMaxNs = maxOf(redrawMaxNs, took)
		val now = System.nanoTime()
		if(now - windowStartNs >= 1_000_000_000L)
		{
			Log.i(TAG, String.format(Locale.US, "Redraws: %d in %.1f s, mean %.2f ms, max %.2f ms",
				redraws, (now - windowStartNs) / 1e9, redrawNs / 1e6 / redraws, redrawMaxNs / 1e6))
			redraws = 0
			redrawNs = 0L
			redrawMaxNs = 0L
			windowStartNs = now
		}
	}

	private fun name(widget: Widget) = when(widget)
	{
		is VrButton -> "button '${widget.text}'"
		is VrSegment -> "choice '${widget.text}'"
		is VrToggle -> "toggle '${widget.label}' ${if(widget.checked) "on" else "off"}"
		is VrSlider -> "slider '${widget.label}' ${widget.format(widget.current)}"
		is VrLabel -> "label '${widget.text}'"
		else -> widget.javaClass.simpleName
	}

	private fun isPad(event: KeyEvent) = event.isFromSource(InputDevice.SOURCE_GAMEPAD) ||
		event.isFromSource(InputDevice.SOURCE_JOYSTICK) || event.isFromSource(InputDevice.SOURCE_DPAD) ||
		KeyEvent.isGamepadButton(event.keyCode)

	companion object
	{
		const val TAG = "GoVrUi"
		const val MENU = 0
		const val STATS = 1
		/** VrUiFrame sizes: vr-cinema.cpp's draw reads 7 control floats and writes 8. */
		const val CONTROL_SIZE = 7
		const val OUTPUT_SIZE = 8
		private const val FLAG_INTERACTIVE = 1
		private const val FLAG_PLACE = 2
		private const val FLAG_DEBUG_POINTER = 4
		private const val BUTTON_TRIGGER = 1
		private const val BUTTON_TOUCHPAD = 2
		const val CHORD_MS = 800L
		private const val TICK_MS = 16L
		private const val STICK_DEAD_ZONE = 0.25f
		private const val STICK_SCROLL_PER_S = 600f
		private const val DEBUG_POINTER_POLL_MS = 250L
	}
}
