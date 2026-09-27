// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.stream

import android.app.Activity
import android.os.Looper
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.view.WindowManager
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config
import java.util.Collections
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/**
 * PLE-829: the phone's StreamActivity reads its pad on PLE-802's [InputThreadWindow], so the window's
 * own settings matter there: it must keep the system bars hidden (Android 11 to 13 give the focused
 * window the navigation bar) and carry PLE-91's unbuffered joystick request, while the Go's window
 * stays as PLE-802 made it. A key the stream refuses must reach the activity's handling on main, once.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [30])
class InputThreadWindowTest
{
	private class Router(private val streamTakes: Boolean): InputThreadWindow.Router
	{
		val calls: MutableList<String> = Collections.synchronizedList(mutableListOf<String>())
		override fun key(event: KeyEvent): Boolean
		{
			calls += "key ${Thread.currentThread().name}"
			return streamTakes
		}
		override fun motion(event: MotionEvent) = streamTakes
		override fun unhandledKey(event: KeyEvent): Boolean
		{
			calls += "unhandledKey ${Thread.currentThread().name}"
			return true
		}
		override fun unhandledMotion(event: MotionEvent) = false
	}

	private fun activity() = Robolectric.buildActivity(Activity::class.java).setup().visible().get()

	/** The window [window] added, once its thread has added it: its view and its parameters. */
	private fun addedWindow(window: InputThreadWindow, title: String): Pair<View, WindowManager.LayoutParams>
	{
		// The window is added by the first message on its thread: anything posted after it runs later.
		onInputThread(window) {}
		// Hidden from the SDK: the process's list of the windows it added.
		val globalClass = Class.forName("android.view.WindowManagerGlobal")
		val global = globalClass.getMethod("getInstance").invoke(null)!!
		val viewsField = globalClass.getDeclaredField("mViews").apply { isAccessible = true }
		val paramsField = globalClass.getDeclaredField("mParams").apply { isAccessible = true }
		val lock = globalClass.getDeclaredField("mLock").apply { isAccessible = true }.get(global)!!
		synchronized(lock)
		{
			@Suppress("UNCHECKED_CAST") val views = viewsField.get(global) as List<View>
			@Suppress("UNCHECKED_CAST") val params = paramsField.get(global) as List<WindowManager.LayoutParams>
			val index = params.indexOfFirst { it.title == title }
			assertTrue("no window titled $title was added", index >= 0)
			return views[index] to params[index]
		}
	}

	/** Runs [action] on the input thread and keeps the main looper running until it is done. */
	private fun onInputThread(window: InputThreadWindow, action: () -> Unit)
	{
		val done = CountDownLatch(1)
		assertTrue(window.post {
			try
			{
				action()
			}
			finally
			{
				done.countDown()
			}
		})
		val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5)
		while(!done.await(1, TimeUnit.MILLISECONDS))
		{
			shadowOf(Looper.getMainLooper()).idle()
			assertTrue("the input thread's action did not finish", System.nanoTime() < deadline)
		}
	}

	@Suppress("DEPRECATION")
	@Test
	fun phoneWindowHidesTheBarsAndTakesTheJoystickUnbuffered()
	{
		val activity = activity()
		val window = InputThreadWindow(activity, Router(streamTakes = true), "PadInput", hideSystemBars = true,
			unbufferedJoystick = true)
		assertTrue(window.start())
		val (view, params) = addedWindow(window, "PadInput")
		assertEquals(WindowManager.LayoutParams.TYPE_APPLICATION_PANEL, params.type)
		assertEquals(activity.window.decorView.windowToken, params.token)
		assertEquals(0f, params.alpha)
		val flags = WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE or WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL or
			WindowManager.LayoutParams.FLAG_ALT_FOCUSABLE_IM
		assertEquals(flags, params.flags and flags)
		assertEquals(0, params.flags and WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE)
		// Set before the window was added, so its first request to the window manager hides the bars.
		val hidden = View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_FULLSCREEN or View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
		assertEquals(hidden, view.systemUiVisibility and hidden)
		val unbuffered = View::class.java.getDeclaredField("mUnbufferedInputSource").apply { isAccessible = true }
		assertEquals(InputDevice.SOURCE_CLASS_JOYSTICK, unbuffered.getInt(view))
		window.stop()
	}

	@Suppress("DEPRECATION")
	@Test
	fun goWindowKeepsTheActivityFlagsAndBufferedJoystick()
	{
		val activity = activity()
		val window = InputThreadWindow(activity, Router(streamTakes = true))
		assertTrue(window.start())
		val (view, _) = addedWindow(window, InputThreadWindow.TAG)
		assertEquals(activity.window.decorView.systemUiVisibility, view.systemUiVisibility)
		val unbuffered = View::class.java.getDeclaredField("mUnbufferedInputSource").apply { isAccessible = true }
		assertEquals(0, unbuffered.getInt(view))
		window.stop()
	}

	@Test
	fun keyTheStreamRefusesGetsTheActivityHandlingOnMainOnce()
	{
		val activity = activity()
		val router = Router(streamTakes = false)
		val window = InputThreadWindow(activity, router, "PadInput", hideSystemBars = true)
		assertTrue(window.start())
		val (view, _) = addedWindow(window, "PadInput")
		var handled = false
		onInputThread(window) {
			handled = view.dispatchKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_VOLUME_UP))
		}
		assertTrue("the main thread's answer is the window's", handled)
		assertEquals(listOf("key PadInput", "unhandledKey ${Looper.getMainLooper().thread.name}"), router.calls.toList())
		window.stop()
	}

	@Test
	fun keyTheStreamTakesStaysOnTheInputThread()
	{
		val activity = activity()
		val router = Router(streamTakes = true)
		val window = InputThreadWindow(activity, router, "PadInput", hideSystemBars = true)
		assertTrue(window.start())
		val (view, _) = addedWindow(window, "PadInput")
		var handled = false
		onInputThread(window) {
			handled = view.dispatchKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BUTTON_THUMBR))
		}
		assertTrue(handled)
		assertEquals(listOf("key PadInput"), router.calls.toList())
		window.stop()
		assertFalse("a stopped window runs nothing", window.post {})
	}
}
