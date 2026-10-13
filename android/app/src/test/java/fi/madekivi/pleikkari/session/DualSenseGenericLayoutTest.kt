// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.session

import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import fi.madekivi.pleikkari.common.Preferences
import fi.madekivi.pleikkari.lib.ControllerState
import org.junit.Assert.assertEquals
import org.junit.Assert.assertSame
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config

/**
 * PLE-876: a DualSense on the Oculus Go (Android 7.1, no key layout for it, hid-generic) names its
 * buttons in HID order through Generic.kl; the stream and the VR menus get their DualSense names.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [25])
class DualSenseGenericLayoutTest
{
	companion object
	{
		const val PAD = 5
		val GO_DUALSENSE = DualSenseGenericLayout.Pad(0x054c, 0x0ce6, hasLTrigger = false, hasRx = true)
		val LAYOUT_DUALSENSE = DualSenseGenericLayout.Pad(0x054c, 0x0ce6, hasLTrigger = true, hasRx = false)

		/** What the Go delivers, by scan code (BTN_GAMEPAD + HID button - 1): Generic.kl's names. */
		val GENERIC = listOf(
			0x130 to KeyEvent.KEYCODE_BUTTON_A, 0x131 to KeyEvent.KEYCODE_BUTTON_B,
			0x132 to KeyEvent.KEYCODE_BUTTON_C, 0x133 to KeyEvent.KEYCODE_BUTTON_X,
			0x134 to KeyEvent.KEYCODE_BUTTON_Y, 0x135 to KeyEvent.KEYCODE_BUTTON_Z,
			0x136 to KeyEvent.KEYCODE_BUTTON_L1, 0x137 to KeyEvent.KEYCODE_BUTTON_R1,
			0x138 to KeyEvent.KEYCODE_BUTTON_L2, 0x139 to KeyEvent.KEYCODE_BUTTON_R2,
			0x13a to KeyEvent.KEYCODE_BUTTON_SELECT, 0x13b to KeyEvent.KEYCODE_BUTTON_START,
			0x13c to KeyEvent.KEYCODE_BUTTON_MODE, 0x13d to KeyEvent.KEYCODE_BUTTON_THUMBL,
			0x13e to KeyEvent.KEYCODE_BUTTON_THUMBR)

		fun key(scanCode: Int, keyCode: Int, down: Boolean = true, flags: Int = 0) = KeyEvent(1L, 1L,
			if(down) KeyEvent.ACTION_DOWN else KeyEvent.ACTION_UP, keyCode, 0, 0, PAD, scanCode, flags,
			InputDevice.SOURCE_GAMEPAD)
	}

	private val context = RuntimeEnvironment.getApplication()
	private val preferences = Preferences(context)

	/** [pad] describes the device [PAD]; any other device id is a pad with a layout of its own. */
	private fun layout(pad: DualSenseGenericLayout.Pad?) = DualSenseGenericLayout({ if(it == PAD) pad else null })

	@Test
	fun everyButtonGetsItsDualSenseName()
	{
		val layout = layout(GO_DUALSENSE)
		val names = GENERIC.map { (scan, code) -> layout.normalize(key(scan, code)).keyCode }
		assertEquals(listOf(
			KeyEvent.KEYCODE_BUTTON_X,      // Square
			KeyEvent.KEYCODE_BUTTON_A,      // Cross
			KeyEvent.KEYCODE_BUTTON_B,      // Circle
			KeyEvent.KEYCODE_BUTTON_Y,      // Triangle
			KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_R1,
			KeyEvent.KEYCODE_BUTTON_L2, KeyEvent.KEYCODE_BUTTON_R2,
			KeyEvent.KEYCODE_BUTTON_SELECT, // Create
			KeyEvent.KEYCODE_BUTTON_START,  // Options
			KeyEvent.KEYCODE_BUTTON_THUMBL, KeyEvent.KEYCODE_BUTTON_THUMBR,
			KeyEvent.KEYCODE_BUTTON_MODE,   // PS
			KeyEvent.KEYCODE_BUTTON_1,      // touchpad click
			KeyEvent.KEYCODE_BUTTON_2       // mute
		), names)
	}

	@Test
	fun theRestOfTheEventIsKept()
	{
		val event = layout(GO_DUALSENSE).normalize(key(0x130, KeyEvent.KEYCODE_BUTTON_A, down = false))
		assertEquals(KeyEvent.ACTION_UP, event.action)
		assertEquals(0x130, event.scanCode)
		assertEquals(PAD, event.deviceId)
		assertEquals(InputDevice.SOURCE_GAMEPAD, event.source)
	}

	@Test
	fun padsWithALayoutAreLeftAlone()
	{
		val sony = key(0x130, KeyEvent.KEYCODE_BUTTON_A) // hid-playstation's BTN_SOUTH: Cross
		assertSame(sony, layout(LAYOUT_DUALSENSE).normalize(sony))
		assertSame(sony, layout(GO_DUALSENSE.copy(productId = 0x09cc)).normalize(sony)) // DualShock 4
		assertSame(sony, layout(GO_DUALSENSE.copy(vendorId = 0x045e)).normalize(sony)) // Xbox
		assertSame(sony, layout(null).normalize(sony))
	}

	@Test
	fun fallbackKeysPassThrough()
	{
		val back = key(0x131, KeyEvent.KEYCODE_BACK, flags = KeyEvent.FLAG_FALLBACK)
		assertSame(back, layout(GO_DUALSENSE).normalize(back))
	}

	@Test
	fun theDeviceIsReadOncePerId()
	{
		var reads = 0
		val layout = DualSenseGenericLayout({ reads++; GO_DUALSENSE })
		repeat(3) { layout.normalize(key(0x130, KeyEvent.KEYCODE_BUTTON_A)) }
		assertEquals(1, reads)
	}

	@Test
	fun theConsoleGetsTheButtonPressed()
	{
		val input = StreamInput(context, preferences, layout(GO_DUALSENSE))
		fun press(scan: Int) = GENERIC.first { it.first == scan }.second.let { code ->
			input.dispatchKeyEvent(key(scan, code))
			input.controllerState.also { input.dispatchKeyEvent(key(scan, code, down = false)) }
		}
		assertEquals(ControllerState.BUTTON_BOX, press(0x130).buttons)        // Square, not Cross
		assertEquals(ControllerState.BUTTON_CROSS, press(0x131).buttons)
		assertEquals(ControllerState.BUTTON_MOON, press(0x132).buttons)
		assertEquals(ControllerState.BUTTON_PYRAMID, press(0x133).buttons)
		assertEquals(ControllerState.BUTTON_L1, press(0x134).buttons)
		assertEquals(ControllerState.BUTTON_R1, press(0x135).buttons)
		assertEquals(UByte.MAX_VALUE, press(0x136).l2State)
		assertEquals(UByte.MAX_VALUE, press(0x137).r2State)
		assertEquals(ControllerState.BUTTON_SHARE, press(0x138).buttons)
		press(0x139).let {                                                       // Options, not R2
			assertEquals(ControllerState.BUTTON_OPTIONS, it.buttons)
			assertEquals(0.toUByte(), it.r2State)
		}
		assertEquals(ControllerState.BUTTON_L3, press(0x13a).buttons)
		assertEquals(ControllerState.BUTTON_R3, press(0x13b).buttons)
		assertEquals(ControllerState.BUTTON_PS, press(0x13c).buttons)
		assertEquals(0U, press(0x13d).buttons)                                   // touchpad: not L3
		assertEquals(0U, press(0x13e).buttons)                                   // mute: not R3
	}

	private fun motion(device: Int, vararg axes: Pair<Int, Float>): MotionEvent
	{
		val coords = MotionEvent.PointerCoords().apply { for((axis, value) in axes) setAxisValue(axis, value) }
		val properties = MotionEvent.PointerProperties().apply { id = 0 }
		return MotionEvent.obtain(1L, 1L, MotionEvent.ACTION_MOVE, 1, arrayOf(properties), arrayOf(coords), 0, 0,
			1f, 1f, device, 0, InputDevice.SOURCE_JOYSTICK or InputDevice.SOURCE_GAMEPAD, 0)
	}

	@Test
	fun theAnalogTriggersAreReadFromRxAndRy()
	{
		val input = StreamInput(context, preferences, layout(GO_DUALSENSE))
		fun triggers(rx: Float, ry: Float): Pair<UByte, UByte>
		{
			input.onGenericMotionEvent(motion(PAD, MotionEvent.AXIS_RX to rx, MotionEvent.AXIS_RY to ry))
			return input.controllerState.run { l2State to r2State }
		}
		assertEquals(0.toUByte() to 0.toUByte(), triggers(-1f, -1f))             // both released
		assertEquals(UByte.MAX_VALUE to 127.toUByte(), triggers(1f, 0f))         // L2 full, R2 half
	}

	@Test
	fun anIdlePadDoesNotResetTheOtherPadsTriggerOrStick()
	{
		// The operator's Go, 2026-10-13: an Xbox pad and a DualSense paired; the Xbox's L2 stopped braking.
		val xbox = 6
		val input = StreamInput(context, preferences, layout(GO_DUALSENSE), deviceConnected = { true })
		input.onGenericMotionEvent(motion(xbox, MotionEvent.AXIS_LTRIGGER to 1f, MotionEvent.AXIS_X to -1f))
		input.onGenericMotionEvent(motion(PAD, MotionEvent.AXIS_X to 0.01f, MotionEvent.AXIS_RX to -1f,
			MotionEvent.AXIS_RY to -1f))
		input.controllerState.run {
			assertEquals(UByte.MAX_VALUE, l2State)
			assertEquals((-32767).toShort(), leftX)
		}
		// The Xbox lets go: nothing is held.
		input.onGenericMotionEvent(motion(xbox))
		input.controllerState.run {
			assertEquals(0.toUByte(), l2State)
			assertEquals(327.toShort(), leftX)                                   // the DualSense's own 1 %
		}
	}

	@Test
	fun aPadThatWentAwayHoldsNothing()
	{
		val xbox = 6
		var xboxConnected = true
		val input = StreamInput(context, preferences, layout(GO_DUALSENSE), deviceConnected = { it != xbox || xboxConnected })
		input.onGenericMotionEvent(motion(xbox, MotionEvent.AXIS_LTRIGGER to 1f))
		xboxConnected = false
		input.onGenericMotionEvent(motion(PAD, MotionEvent.AXIS_RX to -1f, MotionEvent.AXIS_RY to -1f))
		assertEquals(0.toUByte(), input.controllerState.l2State)
	}
}
