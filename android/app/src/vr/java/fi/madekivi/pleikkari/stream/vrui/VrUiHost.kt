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
 * anchored to the picture). PLE-730: while [home] has a page (VR Home before a stream), [MENU]
 * shows it whenever the menu is closed.
 */
class VrUiHost(
	context: Context,
	/** PLE-732: the menu panel shows one [VrPage] at a time. */
	private val buildMenu: (VrPage) -> VrScreen,
	private val refreshMenu: (VrPage, VrScreen) -> Unit,
	/** Main thread: the menu opened or closed (the activity neutralises the pad's console input). */
	private val menuChanged: (Boolean) -> Unit,
	/** Share and Options, as the console mapping has them: held together they open the menu. */
	private val shareKey: Int,
	private val optionsKey: Int,
	/** Debug builds: `debug.pleikkari.vr_pointer` = "x,y" degrees, read while the menu or Home is open. */
	private val debugPointer: (() -> String)?,
	/** PLE-730: a Home button, on this thread; the activity acts on it on the main thread. */
	private val homeAction: (VrHomeAction) -> Unit = {}
)
{
	private val thread = HandlerThread("GoVrUi", Process.THREAD_PRIORITY_DEFAULT).apply { start() }
	private val handler = Handler(thread.looper)
	private val main = Handler(context.mainLooper)
	private val painter = VrUiPainter(context)
	private val surfaces = arrayOfNulls<Surface>(2)
	private var menu: VrScreen? = null
	private var page = VrPage.MENU
	/** PLE-731: a screen shown on the menu panel in place of the menu (the PIN pad) until [closeModal]. */
	private class Modal(val screen: VrScreen, val refresh: () -> Unit, val back: () -> Unit)
	private var modal: Modal? = null
	/** PLE-730: Home's page, if any; shown on [MENU] while the menu is closed. */
	private var home: VrHomePage? = null
	/** What the menu panel shows: the modal screen when there is one, the menu while open, else Home's page. */
	private val current get() = modal?.screen ?: if(menuOpen) menu else home?.screen
	private fun refreshCurrent(screen: VrScreen)
	{
		val m = modal
		if(m != null) m.refresh() else if(screen === menu) refreshMenu(page, screen)
	}
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
	/** PLE-730: Home is up (the panel stays open with the menu closed); the cinema draws no status text under it. */
	@Volatile var homeShown = false
		private set
	/** The panels have their Surfaces and their first pixels: the panel may show. */
	@Volatile private var attached = false
	private var debugPolling = false
	@Volatile var statsOpen = false
	/** A click with no menu open opens it, except while a Library launch picks its console. */
	@Volatile var clickOpensMenu = true
	@Volatile private var interactive = false
	@Volatile private var pointerOverride: FloatArray? = null
	@Volatile var radius = 2f
	/** PLE-761: the panel textures' texels per toolkit texel (VrUiLayerDebug.texelScale); set before [attach]. */
	@Volatile var texelScale = 1f
	@Volatile var statsAnchor = 0f to 0f
	private val placeRequest = AtomicBoolean(false)
	private val recentreRequest = AtomicBoolean(false)
	private val environmentRequest = AtomicBoolean(false)

	// Redraw accounting, logged once a second.
	private var redraws = 0
	private var redrawNs = 0L
	private var redrawMaxNs = 0L
	/** Of [redrawNs], the part spent in unlockCanvasAndPost (HWUI's render and queue). */
	private var postNs = 0L
	/** PLE-763: of [redrawNs], the part spent in lockHardwareCanvas (dequeueing a buffer). */
	private var lockNs = 0L
	private var windowStartNs = System.nanoTime()

	// ---- GoCinema thread ----

	/** The panel Surfaces, from VrCinemaNative.createPanel. */
	fun attach(menuSurface: Surface?, statsSurface: Surface?) = handler.post {
		surfaces[MENU] = menuSurface
		surfaces[STATS] = statsSurface
		Log.i(TAG, "Panels attached: menu ${menuSurface != null}, stats ${statsSurface != null}")
		// Drawn once while closed, so the layer's first frame on opening already has its pixels.
		val screen = menu ?: buildMenu(page).also { menu = it }
		refreshMenu(page, screen)
		modal?.refresh?.invoke()
		redrawMenu()
		if(statsOpen)
			redrawStats()
		attached = true
		spin()
	}

	/** Fills the frame's VrUiFrame control slots (vr-cinema.cpp's draw). */
	fun control(out: FloatArray)
	{
		val override = pointerOverride
		var flags = 0
		if(interactive) flags = flags or FLAG_INTERACTIVE
		if(placeRequest.getAndSet(false)) flags = flags or FLAG_PLACE
		val panel = menuOpen || homeShown && attached
		if(override != null && panel) flags = flags or FLAG_DEBUG_POINTER
		out[0] = ((if(panel) 1 shl MENU else 0) or (if(statsOpen) 1 shl STATS else 0)).toFloat()
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

	/**
	 * The remote's Back: on a modal screen, its own Back; returns from Settings to the menu, or
	 * closes it; with the menu closed, Home's Back (a status sheet's Cancel, PLE-730) or opens it.
	 */
	fun back() = handler.post { backPressed("back") }

	/**
	 * PLE-730: debug builds, the PLE-739 broadcast's touchpad thirds while Home is up: left and
	 * right move the pad focus up and down, centre is the pad's A. The real remote uses the pointer.
	 */
	fun debugKey(code: Int) = handler.post {
		menuKey(code, true, 0)
		menuKey(code, false, 0)
	}

	/** PLE-732: swaps the menu panel to [target] (Settings and back), keeping it open where it is. */
	fun showPage(target: VrPage) = handler.post { switchPage(target) }

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
		if(screen.titleAlert)
			shake(screen, System.nanoTime())
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
				// On the key's release, as the menu closes on it: opening on the press let the
				// same key's release close the menu again at once.
				if(!down)
					handler.post { open("pad Menu key") }
				return true
			}
			// PLE-730: Home takes the pad; no stream has its keys yet.
			if(homeShown && isPad(event))
			{
				handler.post { menuKey(code, down, event.repeatCount) }
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
		if(!menuOpen && !homeShown || event.source and InputDevice.SOURCE_CLASS_JOYSTICK != InputDevice.SOURCE_CLASS_JOYSTICK)
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

	/** PLE-730: Home's page, or null when a stream shows (or Home is not for this launch). Any thread. */
	fun home(state: VrHomeState?) = handler.post { setHome(state) }

	/** PLE-730: Home's Settings: the menu panel on PLE-732's Settings page; Back returns to the menu, then Home. */
	fun openSettings(reason: String) = handler.post {
		open(reason)
		switchPage(VrPage.SETTINGS)
	}

	private fun closeNow(reason: String)
	{
		if(!menuOpen)
			return
		menuOpen = false
		interactive = false
		// The next open starts on the in-stream menu again.
		if(page != VrPage.MENU)
		{
			page = VrPage.MENU
			menu = null
		}
		pointer.reset(lastSelect)
		focus.reset()
		chordDown.clear()
		Log.i(TAG, "Menu closed ($reason)")
		main.post { menuChanged(false) }
		// PLE-730: Home again, where the menu was.
		if(homeShown)
		{
			redrawMenu()
			spin()
		}
	}

	/** Stops the UI thread; call before the panels' swapchains go (the cinema's destroy). */
	fun shutdown()
	{
		handler.removeCallbacksAndMessages(null)
		thread.quitSafely()
		thread.join()
	}

	// ---- GoVrUi thread ----

	private fun switchPage(target: VrPage)
	{
		if(page == target)
			return
		page = target
		val screen = buildMenu(page).also { menu = it }
		refreshMenu(page, screen)
		pointer.reset(lastSelect)
		focus.reset()
		Log.i(TAG, "Menu page $page")
		redrawMenu()
	}

	private fun open(reason: String)
	{
		if(menuOpen)
			return
		val screen = menu ?: buildMenu(page).also { menu = it }
		refreshMenu(page, screen)
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

	private fun backPressed(reason: String)
	{
		val m = modal
		val homeBack = home?.back
		when
		{
			m != null -> m.back()
			menuOpen && page != VrPage.MENU -> switchPage(VrPage.MENU)
			menuOpen -> closeNow(reason)
			homeShown && homeBack != null ->
			{
				Log.i(TAG, "Home Back: $homeBack")
				homeAction(homeBack)
			}
			else -> open(reason)
		}
	}

	/** PLE-730: a new page builds its widgets; the same page with new words keeps its pointer and focus. */
	private fun setHome(state: VrHomeState?)
	{
		val existing = home
		if(state == null)
		{
			if(existing == null)
				return
			home = null
			homeShown = false
			if(!menuOpen)
			{
				interactive = false
				pointer.reset(lastSelect)
				focus.reset()
			}
			Log.i(TAG, "Home closed")
			return
		}
		if(existing != null && existing.state == state)
			return
		if(existing != null && existing.update(state))
		{
			Log.i(TAG, "Home: ${existing.name}")
			if(!menuOpen)
				redrawMenu()
			return
		}
		val built = VrHomePage.build(state, homeAction)
		home = built
		if(!menuOpen)
		{
			pointer.reset(lastSelect)
			focus.reset()
			lastHoverName = null
		}
		Log.i(TAG, "Home: ${built.name}")
		if(!homeShown)
		{
			homeShown = true
			pollDebugPointer()
		}
		if(!menuOpen)
			redrawMenu()
		spin()
	}

	// PLE-730: the status sheet's spinner turns while it shows (§10.5 Progress), a step per redraw.
	private var spinning = false
	private val spinStep = object: Runnable
	{
		override fun run()
		{
			spinning = false
			// Stops while the menu covers Home or the page has no spinner; closeNow and setHome start it again.
			val spinner = home?.spinner?.takeIf { attached && homeShown && !menuOpen } ?: return
			spinner.phase = (System.nanoTime() % SPIN_PERIOD_NS).toFloat() / SPIN_PERIOD_NS
			redrawMenu()
			spin()
		}
	}

	/** PLE-757: shakes an alert modal's title (§10.7's wrong PIN), a redraw per step until it settles or leaves. */
	private fun shake(screen: VrScreen, startNs: Long)
	{
		if(modal?.screen !== screen)
			return
		val elapsed = System.nanoTime() - startNs
		screen.titleShift = VrPinPad.shakeShift(elapsed)
		redrawMenu()
		if(elapsed < VrPinPad.SHAKE_NS)
			handler.postDelayed({ shake(screen, startNs) }, TICK_MS)
	}

	private fun spin()
	{
		if(spinning || home?.spinner == null)
			return
		spinning = true
		handler.postDelayed(spinStep, SPIN_STEP_MS)
	}

	private fun pointerSample(s: PointerSample)
	{
		val previous = lastSelect
		lastSelect = s.select
		val screen = current
		if(screen == null)
		{
			if(s.select && !previous && clickOpensMenu)
				open(if(s.trigger) "trigger" else "touchpad click")
			return
		}
		// PLE-730: a click away from Home brings it back in front of the gaze (it is summoned, then world-locked).
		if(!menuOpen && s.select && !previous && !s.onPanel)
		{
			placeRequest.set(true)
			Log.i(TAG, "Home summoned to the gaze")
		}
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
			// PLE-730: B is Back, as on the remote (a modal's Back, Settings to the menu, close, Home's Back).
			KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_BACK, KeyEvent.KEYCODE_MENU ->
				if(!down) backPressed("pad")
			KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_R1 ->
				if(down)
				{
					val list = focus.focused?.list ?: screen.lists.firstOrNull() ?: return
					val pageHeight = list.viewport.height * if(code == KeyEvent.KEYCODE_BUTTON_L1) -1 else 1
					if(list.scrollBy(pageHeight))
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
		// PLE-730: one poll for the menu and Home together.
		if(debugPolling)
			return
		debugPolling = true
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
				if(menuOpen || homeShown)
					handler.postDelayed(this, DEBUG_POINTER_POLL_MS)
				else
					debugPolling = false
			}
		}, 0)
	}

	/** [MENU]'s screen; with the menu closed and no Home, the menu itself, drawn ahead of its opening. */
	private fun redrawMenu()
	{
		val screen = current ?: menu ?: return
		draw(surfaces[MENU]) { painter.draw(it, screen, focus) }
	}

	private fun redrawStats() = draw(surfaces[STATS]) { painter.draw(it, VrStatsPanel.screen(statsLines), VrFocus()) }

	private inline fun draw(surface: Surface?, paint: (android.graphics.Canvas) -> Unit)
	{
		if(surface == null || !surface.isValid)
			return
		val start = System.nanoTime()
		var posting = 0L
		var locked = 0L
		try
		{
			val canvas = surface.lockHardwareCanvas()
			locked = System.nanoTime()
			try
			{
				if(texelScale != 1f)
					canvas.scale(texelScale, texelScale)
				paint(canvas)
			}
			finally
			{
				posting = System.nanoTime()
				surface.unlockCanvasAndPost(canvas)
			}
		}
		catch(e: RuntimeException)
		{
			// The swapchain went away under us (the cinema stopping); the next start builds new ones.
			Log.w(TAG, "Panel redraw failed: $e")
			return
		}
		val end = System.nanoTime()
		val took = end - start
		postNs += end - posting
		lockNs += locked - start
		redraws++
		redrawNs += took
		redrawMaxNs = maxOf(redrawMaxNs, took)
		val now = System.nanoTime()
		if(now - windowStartNs >= 1_000_000_000L)
		{
			Log.i(TAG, String.format(Locale.US, "Redraws: %d in %.1f s, mean %.2f ms (lock %.2f, paint %.2f, post %.2f), max %.2f ms",
				redraws, (now - windowStartNs) / 1e9, redrawNs / 1e6 / redraws, lockNs / 1e6 / redraws,
				(redrawNs - lockNs - postNs) / 1e6 / redraws, postNs / 1e6 / redraws, redrawMaxNs / 1e6))
			redraws = 0
			redrawNs = 0L
			postNs = 0L
			lockNs = 0L
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
		is VrCard -> "card '${widget.name}'"
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
		/** PLE-730: the spinner's turn and its step: 8 redraws a second while a status sheet shows. */
		private const val SPIN_PERIOD_NS = 1_000_000_000L
		private const val SPIN_STEP_MS = 125L
	}
}

/** PLE-732: what the menu panel shows. */
enum class VrPage { MENU, SETTINGS }
