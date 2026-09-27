// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
package fi.madekivi.pleikkari.stream

import android.app.Activity
import android.content.Context
import android.graphics.PixelFormat
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.IBinder
import android.os.Looper
import android.os.Process
import android.util.Log
import android.view.Gravity
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.view.WindowManager
import java.util.concurrent.Callable
import java.util.concurrent.ExecutionException
import java.util.concurrent.FutureTask
import java.util.concurrent.TimeUnit
import java.util.concurrent.TimeoutException
import java.util.concurrent.atomic.AtomicBoolean

/**
 * PLE-802: an activity's key and joystick input, read on a thread of its own instead of the main looper.
 *
 * A window's input events arrive on the looper of the thread that added the window: ViewRootImpl
 * reads the window's input channel on the `Looper.myLooper()` of its setView. An activity's own
 * window is the main thread's, and `Window.takeInputQueue` does not change that: ViewRootImpl still
 * reads each event on the main looper, then forwards it to the queue. So this adds a second window
 * from a [HandlerThread] of its own. It is a 1x1 sub-window of the activity's, with alpha 0 so
 * SurfaceFlinger has no layer to composite (on Android 14+ it is off the screen instead: [add]). It is
 * focusable, because key and joystick events go to the focused window, but not touchable, so touches
 * still reach the activity. It is not an IME target, so no key takes a round trip through the
 * keyboard's process first.
 *
 * [Router.key] and [Router.motion] see each event on that thread. An event they do not take goes
 * back to the main thread, which gives it the activity window's own handling ([handBack]). Its
 * answer is this window's answer, so the system's fallback keys (a pad's B as Back, volume) behave
 * as they did.
 *
 * One ordering caveat, at [start] only. Events the activity window took just before this window had
 * the focus can still be waiting on the main looper when newer ones arrive here, including the
 * cancel the input dispatcher sends for a key held down across the handover. One of those can land
 * after a newer event handled here: the console then holds the older state (a stick position, or a
 * re-pressed button released) until the pad's next event of that kind. Once this window has the
 * focus, events arrive in order on one thread.
 *
 * PLE-829: the phone's StreamActivity uses it too, with [hideSystemBars] and [unbufferedJoystick],
 * because the focused window is also the one the navigation bar (Android 11 to 13) and the
 * joystick's batching follow.
 */
class InputThreadWindow(
	private val activity: Activity,
	private val router: Router,
	/** The thread's name, the window's title and the log tag. */
	private val name: String = TAG,
	/**
	 * PLE-829: keep the system bars hidden while this window has the focus, revealed by a swipe for a
	 * moment as the stream's own window has them. On Android 11 to 13 the focused window controls the
	 * navigation bar (InsetsPolicy.getNavControlTarget), so a window that asks nothing would show it
	 * mid-stream; 14 leaves it to the full-screen activity window while that hides it. Off, the window
	 * copies only the activity window's legacy flags, as the Go does.
	 */
	private val hideSystemBars: Boolean = false,
	/**
	 * PLE-829: PLE-91's `stream_gamepad_unbuffered`, requested on this window (Android 11+), since it
	 * is the one that takes the joystick's events.
	 */
	private val unbufferedJoystick: Boolean = false)
{
	interface Router
	{
		/** Input thread: true when the stream took the event. */
		fun key(event: KeyEvent): Boolean
		fun motion(event: MotionEvent): Boolean
		/** Main thread: the activity's own handling of an event the stream did not take. */
		fun unhandledKey(event: KeyEvent): Boolean
		fun unhandledMotion(event: MotionEvent): Boolean
	}

	private val main = Handler(Looper.getMainLooper())
	// Main thread.
	private var thread: HandlerThread? = null
	private var handler: Handler? = null
	// Input thread.
	private var view: View? = null

	/**
	 * Main thread, with the activity's window attached. False when it has no window token yet: the
	 * input then stays on the main looper.
	 */
	fun start(): Boolean
	{
		if(thread != null)
			return true
		val decor = activity.window.decorView
		val token = decor.windowToken ?: return false
		// The focused window's system UI flags are the screen's; keep the activity's. Before the window is
		// added, so that it never asks for the bars, even for a moment. Android 11+ reads the legacy flags
		// into the window's requested insets, and IMMERSIVE_STICKY is BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE.
		@Suppress("DEPRECATION") val systemUi = decor.systemUiVisibility or (if(hideSystemBars) HIDDEN_SYSTEM_BARS else 0)
		val thread = HandlerThread(name, Process.THREAD_PRIORITY_DISPLAY).also { it.start() }
		val handler = Handler(thread.looper)
		this.thread = thread
		this.handler = handler
		handler.post { add(token, systemUi) }
		return true
	}

	/**
	 * Main thread: runs [action] on the input thread, after the event it is handling, if any. False,
	 * with nothing run, when the window is stopped.
	 */
	fun post(action: () -> Unit): Boolean = handler?.post(action) ?: false

	/**
	 * Main thread. It never waits for the input thread, which may be waiting for the main thread in
	 * [handBack]; an event already on the input thread still reaches the stream after this returns.
	 */
	fun stop()
	{
		val thread = thread ?: return
		val handler = handler ?: return
		this.thread = null
		this.handler = null
		// quitSafely still runs the removal: it is due now.
		handler.post { remove() }
		thread.quitSafely()
	}

	private fun add(token: IBinder, systemUi: Int)
	{
		val view = InputView(activity)
		@Suppress("DEPRECATION")
		view.systemUiVisibility = systemUi
		val params = WindowManager.LayoutParams(1, 1, WindowManager.LayoutParams.TYPE_APPLICATION_PANEL,
			WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE or WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL or
				WindowManager.LayoutParams.FLAG_ALT_FOCUSABLE_IM,
			PixelFormat.TRANSLUCENT).apply {
			this.token = token
			gravity = Gravity.TOP or Gravity.START
			title = name
			if(Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE)
			{
				// PLE-829: from Android 14, SurfaceFlinger gives no input to a layer that has drawn a buffer
				// at alpha 0 (Layer::canReceiveInput). An alpha-0 window never gets the focus: the input
				// dispatcher dropped every key and raised an ANR on the API 36 emulator. So the window keeps
				// its alpha and sits off the screen, where SurfaceFlinger composites nothing
				// (Output::ensureOutputLayerIfVisible). It draws nothing anyway. A multi-window task
				// ignores NO_LIMITS and pulls it back onto the display, where it is a transparent pixel.
				flags = flags or WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS
				y = OFF_SCREEN_Y
			}
			else
				alpha = 0f
		}
		try
		{
			activity.windowManager.addView(view, params)
			this.view = view
			// A request that lasts; the activity's own window asks again on each joystick event.
			if(unbufferedJoystick && Build.VERSION.SDK_INT >= Build.VERSION_CODES.R)
				view.requestUnbufferedDispatch(InputDevice.SOURCE_CLASS_JOYSTICK)
			Log.i(name, "Input window added; key and joystick events arrive on $name" +
				(if(hideSystemBars) ", system bars hidden" else "") +
				(if(unbufferedJoystick && Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) ", joystick unbuffered" else ""))
		}
		catch(e: RuntimeException)
		{
			// WindowManager.BadTokenException (the activity's window went first) or InvalidDisplayException.
			Log.e(name, "Input window refused; input stays on the main thread", e)
		}
	}

	private fun remove()
	{
		val view = view ?: return
		this.view = null
		try
		{
			activity.windowManager.removeViewImmediate(view)
			Log.i(name, "Input window removed; input is back on the main thread")
		}
		catch(e: IllegalArgumentException)
		{
			// Already gone with the activity's window.
		}
	}

	/**
	 * Input thread: [unhandled] on the main thread, returning its answer. If the main thread has not
	 * started it within [HAND_BACK_TIMEOUT_MS], it never will, and the event counts as not handled:
	 * this window's own fallback handling (volume, media keys) still acts on it. Once started, its
	 * answer is waited for, so an event is never handled both there and here.
	 */
	private fun handBack(unhandled: () -> Boolean): Boolean
	{
		val claimed = AtomicBoolean(false)
		val task = FutureTask(Callable { claimed.compareAndSet(false, true) && unhandled() })
		if(!main.post(task))
			return false
		return try
		{
			try
			{
				task.get(HAND_BACK_TIMEOUT_MS, TimeUnit.MILLISECONDS)
			}
			catch(e: TimeoutException)
			{
				if(claimed.compareAndSet(false, true))
				{
					Log.w(name, "Main thread did not take a handed-back event within $HAND_BACK_TIMEOUT_MS ms")
					false
				}
				else
					task.get()
			}
		}
		catch(e: ExecutionException)
		{
			// As the exception would have been on the main thread.
			throw e.cause ?: e
		}
	}

	private inner class InputView(context: Context) : View(context)
	{
		override fun dispatchKeyEvent(event: KeyEvent): Boolean =
			router.key(event) || handBack { router.unhandledKey(event) }

		override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean
		{
			if(router.motion(event))
				return true
			// The dispatcher recycles its MotionEvent once this returns, which a timeout could make too soon.
			val copy = MotionEvent.obtain(event)
			return handBack {
				try
				{
					router.unhandledMotion(copy)
				}
				finally
				{
					copy.recycle()
				}
			}
		}

		override fun onWindowFocusChanged(hasWindowFocus: Boolean)
		{
			super.onWindowFocusChanged(hasWindowFocus)
			Log.i(name, "Input window ${if(hasWindowFocus) "has the focus" else "lost the focus"}")
		}
	}

	companion object
	{
		const val TAG = "GoPadInput"
		/** Far above any display the window's parent can start on; WindowLayout clamps to -100000. */
		private const val OFF_SCREEN_Y = -10000
		@Suppress("DEPRECATION")
		private const val HIDDEN_SYSTEM_BARS = View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_FULLSCREEN or
			View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
		private const val HAND_BACK_TIMEOUT_MS = 1000L
	}
}
