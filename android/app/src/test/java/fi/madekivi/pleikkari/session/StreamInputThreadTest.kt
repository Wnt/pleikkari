// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.session

import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import fi.madekivi.pleikkari.common.Preferences
import fi.madekivi.pleikkari.lib.ControllerState
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import java.util.Collections
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference

/**
 * PLE-802: the Go can deliver pad input to [StreamInput] on its GoPadInput thread instead of the
 * main looper (stream_go_vr_input_thread). The console must get the same controller states either
 * way, and with sensors and the VR menu still changing the state from the main thread, the state
 * sent last must be the newest.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [25])
class StreamInputThreadTest
{
	private val context = RuntimeEnvironment.getApplication()
	private val preferences = Preferences(context)

	/** One step of a scripted pad session: what StreamInput answered and the states it sent. */
	private data class Step(val handled: Boolean, val sent: List<ControllerState>)

	private fun key(code: Int, down: Boolean, time: Long) = KeyEvent(time, time,
		if(down) KeyEvent.ACTION_DOWN else KeyEvent.ACTION_UP, code, 0, 0, PAD_DEVICE, 0, 0,
		InputDevice.SOURCE_GAMEPAD)

	private fun stick(time: Long, x: Float = 0f, y: Float = 0f, z: Float = 0f, rz: Float = 0f,
		l2: Float = 0f, r2: Float = 0f, hatX: Float = 0f, hatY: Float = 0f,
		source: Int = InputDevice.SOURCE_JOYSTICK or InputDevice.SOURCE_GAMEPAD): MotionEvent
	{
		val coords = MotionEvent.PointerCoords().apply {
			setAxisValue(MotionEvent.AXIS_X, x)
			setAxisValue(MotionEvent.AXIS_Y, y)
			setAxisValue(MotionEvent.AXIS_Z, z)
			setAxisValue(MotionEvent.AXIS_RZ, rz)
			setAxisValue(MotionEvent.AXIS_LTRIGGER, l2)
			setAxisValue(MotionEvent.AXIS_RTRIGGER, r2)
			setAxisValue(MotionEvent.AXIS_HAT_X, hatX)
			setAxisValue(MotionEvent.AXIS_HAT_Y, hatY)
		}
		val properties = MotionEvent.PointerProperties().apply { id = 0 }
		return MotionEvent.obtain(time, time, MotionEvent.ACTION_MOVE, 1, arrayOf(properties), arrayOf(coords),
			0, 0, 1f, 1f, PAD_DEVICE, 0, source, 0)
	}

	/** Every mapped button, the digital triggers, both sticks, the hat, and the events StreamInput refuses. */
	private fun session(input: StreamInput): List<() -> Boolean>
	{
		val buttons = listOf(preferences.mappingCross, preferences.mappingCircle, preferences.mappingSquare,
			preferences.mappingTriangle, preferences.mappingL1, preferences.mappingR1, preferences.mappingL3,
			preferences.mappingR3, preferences.mappingShare, preferences.mappingOptions, preferences.mappingPs,
			preferences.mappingL2, preferences.mappingR2)
		val steps = mutableListOf<() -> Boolean>()
		var time = 1_000L
		for(code in buttons)
		{
			val t = time++
			steps += { input.dispatchKeyEvent(key(code, true, t)) }
		}
		// Chords held while the sticks and the hat move, released in another order.
		for(i in 0 until 24)
		{
			val t = time++
			val f = (i - 12) / 12f
			steps += {
				input.onGenericMotionEvent(stick(t, x = f, y = -f, z = f / 2, rz = -f / 3, l2 = (i % 5) / 4f,
					r2 = (i % 3) / 2f, hatX = if(i % 4 == 0) 1f else if(i % 4 == 2) -1f else 0f,
					hatY = if(i % 6 < 3) 1f else -1f))
			}
		}
		for(code in buttons.reversed())
		{
			val t = time++
			steps += { input.dispatchKeyEvent(key(code, false, t)) }
		}
		// Refused: an unmapped key, an ACTION_MULTIPLE and a mouse's generic motion.
		val t = time++
		steps += { input.dispatchKeyEvent(key(KeyEvent.KEYCODE_VOLUME_UP, true, t)) }
		steps += { input.dispatchKeyEvent(KeyEvent(t, t, KeyEvent.ACTION_MULTIPLE, preferences.mappingCross, 1)) }
		steps += { input.onGenericMotionEvent(stick(t, x = 1f, source = InputDevice.SOURCE_MOUSE)) }
		// The VR menu's release, then the pad again.
		steps += { input.dispatchKeyEvent(key(preferences.mappingCross, true, t)) }
		steps += { input.onGenericMotionEvent(stick(t, x = 0.5f, l2 = 1f, hatY = -1f)) }
		steps += { input.releasePad(); true }
		steps += { input.dispatchKeyEvent(key(preferences.mappingSquare, true, t + 1)) }
		return steps
	}

	private fun run(onInputThread: Boolean): List<Step>
	{
		val input = StreamInput(context, preferences)
		val sent = Collections.synchronizedList(mutableListOf<ControllerState>())
		input.controllerStateChangedCallback = { sent += it }
		val steps = session(input)
		val result = mutableListOf<Step>()
		fun runAll()
		{
			for(step in steps)
			{
				val before = sent.size
				val handled = step()
				result += Step(handled, sent.subList(before, sent.size).toList())
			}
		}
		if(!onInputThread)
		{
			assertEquals(Looper.getMainLooper(), Looper.myLooper())
			runAll()
			return result
		}
		val thread = HandlerThread("GoPadInput").apply { start() }
		val done = CountDownLatch(1)
		Handler(thread.looper).post {
			runAll()
			done.countDown()
		}
		assertTrue("input thread finished", done.await(10, TimeUnit.SECONDS))
		thread.quitSafely()
		return result
	}

	@Test
	fun theInputThreadSendsTheSameStatesAsTheMainThread()
	{
		val onMain = run(onInputThread = false)
		val onInputThread = run(onInputThread = true)
		assertEquals(onMain, onInputThread)
		// The script is not trivial: it presses every button and moves every axis.
		val states = onMain.flatMap { it.sent }
		assertEquals(onMain.size - 3, onMain.count { it.handled })
		assertTrue(states.any { it.buttons == ALL_KEY_BUTTONS })
		assertTrue(states.any { it.leftX < 0 && it.leftY > 0 && it.rightX < 0 && it.rightY > 0 })
		assertTrue(states.any { it.buttons and ControllerState.BUTTON_DPAD_LEFT != 0U })
		assertTrue(states.any { it.l2State in 1U..254U })
		// releasePad clears the pad; the next press is sent on its own.
		assertEquals(ControllerState(), onMain[onMain.size - 2].sent.single())
		assertEquals(ControllerState(buttons = ControllerState.BUTTON_BOX), onMain.last().sent.single())
	}

	@Test
	fun bothThreadsChangingTheStateSendOnlyWholeStatesAndEndOnTheNewest()
	{
		val input = StreamInput(context, preferences)
		// Counted, not kept: the main thread sends several hundred thousand states meanwhile.
		val sent = AtomicInteger(0)
		val torn = AtomicInteger(0)
		val firstTorn = AtomicReference<ControllerState>()
		val last = AtomicReference<ControllerState>()
		input.controllerStateChangedCallback = { state ->
			sent.incrementAndGet()
			if(!whole(state))
			{
				torn.incrementAndGet()
				firstTorn.compareAndSet(null, state)
			}
			last.set(state)
		}
		val rounds = 1_000
		val thread = HandlerThread("GoPadInput").apply { start() }
		val start = CountDownLatch(1)
		val done = CountDownLatch(1)
		var padChanges = 0
		Handler(thread.looper).post {
			start.await()
			for(i in 0 until rounds)
			{
				val t = i.toLong()
				if(input.dispatchKeyEvent(key(preferences.mappingCross, i % 2 == 0, t))) padChanges++
				// Each event moves both sticks, both triggers and the hat together, so a state
				// read halfway through one shows as axes that disagree ([whole]).
				val v = ((i % 13) - 6) / 6f
				val hat = if(i % 3 == 0) 1f else if(i % 3 == 1) -1f else 0f
				if(input.onGenericMotionEvent(stick(t, x = v, y = v, z = v, rz = v, l2 = (i % 5) / 4f,
						r2 = (i % 5) / 4f, hatX = hat, hatY = hat))) padChanges++
			}
			done.countDown()
		}
		// Meanwhile the main thread, for as long as the pad runs: the touch overlay's state and the VR
		// menu taking the pad.
		start.countDown()
		var mainChanges = 0
		var i = 0
		val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(30)
		while(done.count > 0 && System.nanoTime() < deadline)
		{
			input.touchControllerState = ControllerState(buttons = if(i % 2 == 0) ControllerState.BUTTON_TOUCHPAD else 0U)
			mainChanges++
			if(i % 97 == 0)
			{
				input.releasePad()
				mainChanges++
			}
			i++
		}
		assertTrue("input thread finished", done.await(30, TimeUnit.SECONDS))
		thread.quitSafely()
		// One state per change, none lost; every one whole; the last one sent is what StreamInput holds now.
		assertEquals(padChanges + mainChanges, sent.get())
		assertEquals("torn states, the first ${firstTorn.get()}", 0, torn.get())
		assertEquals(input.controllerState, last.get())
	}

	/** Not half of one of the concurrency test's joystick events and half of another. */
	private fun whole(state: ControllerState) =
		state.leftX == state.leftY && state.leftX == state.rightX && state.leftX == state.rightY &&
			state.l2State == state.r2State &&
			(state.buttons and ControllerState.BUTTON_DPAD_RIGHT != 0U) == (state.buttons and ControllerState.BUTTON_DPAD_DOWN != 0U) &&
			(state.buttons and ControllerState.BUTTON_DPAD_LEFT != 0U) == (state.buttons and ControllerState.BUTTON_DPAD_UP != 0U)

	private companion object
	{
		const val PAD_DEVICE = 7
		val ALL_KEY_BUTTONS = ControllerState.BUTTON_CROSS or ControllerState.BUTTON_MOON or
			ControllerState.BUTTON_BOX or ControllerState.BUTTON_PYRAMID or ControllerState.BUTTON_L1 or
			ControllerState.BUTTON_R1 or ControllerState.BUTTON_L3 or ControllerState.BUTTON_R3 or
			ControllerState.BUTTON_SHARE or ControllerState.BUTTON_OPTIONS or ControllerState.BUTTON_PS
	}
}
