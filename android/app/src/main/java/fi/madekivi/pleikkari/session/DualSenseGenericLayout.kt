// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.session

import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import java.util.concurrent.ConcurrentHashMap

/**
 * PLE-876: a DualSense on an Android with no key layout for it, the Oculus Go's 7.1 among them.
 * With no hid-playstation in the kernel, hid-generic numbers its buttons in HID report order from
 * BTN_GAMEPAD and Generic.kl names them as if they were an Xbox pad's: Square arrives as BUTTON_A
 * (the stream's Cross), Options as BUTTON_R2. [normalize] gives each button, by its scan code, the
 * key code a DualSense layout gives it. The analog triggers arrive as centred AXIS_RX / AXIS_RY
 * (-1 released, +1 pressed), which [TriggerAxisResolver] picks up through [isGeneric]; the sticks and
 * the hat are already right. A DualSense with a layout (Android 12+, or hid-playstation) reports
 * AXIS_LTRIGGER and is left alone. Called from the main thread and the Go's pad thread (PLE-802).
 */
class DualSenseGenericLayout(private val describe: (Int) -> Pad? = ::padOf)
{
	data class Pad(val vendorId: Int, val productId: Int, val hasLTrigger: Boolean, val hasRx: Boolean)

	companion object
	{
		private const val SONY = 0x054c
		private val DUALSENSE = setOf(0x0ce6, 0x0df2) // DualSense, DualSense Edge

		/** hid-generic's first gamepad button: HID button n arrives as scan code BTN_GAMEPAD + n - 1. */
		private const val BTN_GAMEPAD = 0x130

		/** The DualSense report's buttons in HID order, as a DualSense layout names them. */
		private val KEY_CODES = intArrayOf(
			KeyEvent.KEYCODE_BUTTON_X,      // 1 Square
			KeyEvent.KEYCODE_BUTTON_A,      // 2 Cross
			KeyEvent.KEYCODE_BUTTON_B,      // 3 Circle
			KeyEvent.KEYCODE_BUTTON_Y,      // 4 Triangle
			KeyEvent.KEYCODE_BUTTON_L1,     // 5
			KeyEvent.KEYCODE_BUTTON_R1,     // 6
			KeyEvent.KEYCODE_BUTTON_L2,     // 7
			KeyEvent.KEYCODE_BUTTON_R2,     // 8
			KeyEvent.KEYCODE_BUTTON_SELECT, // 9 Create
			KeyEvent.KEYCODE_BUTTON_START,  // 10 Options
			KeyEvent.KEYCODE_BUTTON_THUMBL, // 11 L3
			KeyEvent.KEYCODE_BUTTON_THUMBR, // 12 R3
			KeyEvent.KEYCODE_BUTTON_MODE,   // 13 PS
			KeyEvent.KEYCODE_BUTTON_1,      // 14 touchpad click: no stream mapping, no longer L3
			KeyEvent.KEYCODE_BUTTON_2       // 15 mute: no stream mapping, no longer R3
		)

		/** One layout for the process: the stream and the Go's VR menus read the same devices. */
		val shared by lazy { DualSenseGenericLayout() }

		fun keyCodeFor(scanCode: Int): Int? = KEY_CODES.getOrNull(scanCode - BTN_GAMEPAD)

		fun matches(pad: Pad) = pad.vendorId == SONY && pad.productId in DUALSENSE && !pad.hasLTrigger && pad.hasRx

		private fun padOf(deviceId: Int): Pad? = InputDevice.getDevice(deviceId)?.let {
			Pad(it.vendorId, it.productId,
				hasLTrigger = it.getMotionRange(MotionEvent.AXIS_LTRIGGER) != null,
				hasRx = it.getMotionRange(MotionEvent.AXIS_RX) != null)
		}
	}

	private val generic = ConcurrentHashMap<Int, Boolean>()

	/** A DualSense on the generic layout; the device is read once per id. */
	fun isGeneric(deviceId: Int): Boolean = generic.getOrPut(deviceId) { describe(deviceId)?.let(::matches) ?: false }

	/**
	 * [event] with the key code its button has on a DualSense layout, or [event] itself. The
	 * framework's fallback keys (Back for an unhandled BUTTON_B, ...) keep the scan code of the key
	 * they stand in for, so they pass through as they are.
	 */
	fun normalize(event: KeyEvent): KeyEvent
	{
		if(event.flags and KeyEvent.FLAG_FALLBACK != 0)
			return event
		val keyCode = keyCodeFor(event.scanCode) ?: return event
		if(keyCode == event.keyCode || !isGeneric(event.deviceId))
			return event
		return KeyEvent(event.downTime, event.eventTime, event.action, keyCode, event.repeatCount, event.metaState,
			event.deviceId, event.scanCode, event.flags, event.source)
	}
}
