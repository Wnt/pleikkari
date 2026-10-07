// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.session

import android.view.InputDevice
import android.view.KeyEvent
import fi.madekivi.pleikkari.common.Preferences
import fi.madekivi.pleikkari.lib.ControllerState
import org.junit.Assert.assertEquals
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config

/** PLE-869: an adb pad script and a physical pad are separate layers of the state the console gets. */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [25])
class StreamInputScriptedTest
{
	private val context = RuntimeEnvironment.getApplication()
	private val preferences = Preferences(context)

	private fun key(code: Int, down: Boolean) = KeyEvent(1L, 1L,
		if(down) KeyEvent.ACTION_DOWN else KeyEvent.ACTION_UP, code, 0, 0, 1, 0, 0, InputDevice.SOURCE_GAMEPAD)

	@Test
	fun scriptAndPadDoNotClearEachOther()
	{
		val input = StreamInput(context, preferences)
		val sent = mutableListOf<ControllerState>()
		input.controllerStateChangedCallback = { sent += it }
		input.dispatchKeyEvent(key(preferences.mappingCross, true))
		input.scriptedControllerState = ControllerState(buttons = ControllerState.BUTTON_DPAD_DOWN, leftY = -32767)
		assertEquals(ControllerState.BUTTON_CROSS or ControllerState.BUTTON_DPAD_DOWN, input.controllerState.buttons)
		assertEquals((-32767).toShort(), input.controllerState.leftY)

		// The pad lets go of Cross: the scripted D-pad stays down.
		input.dispatchKeyEvent(key(preferences.mappingCross, false))
		assertEquals(ControllerState.BUTTON_DPAD_DOWN, input.controllerState.buttons)

		// The script releases: nothing is left down.
		input.scriptedControllerState = ControllerState()
		assertEquals(0U, input.controllerState.buttons)
		assertEquals(0.toShort(), input.controllerState.leftY)
	}

	@Test
	fun scriptedStateIsCopied()
	{
		val input = StreamInput(context, preferences)
		val state = ControllerState(buttons = ControllerState.BUTTON_PS)
		input.scriptedControllerState = state
		state.buttons = 0U
		assertEquals(ControllerState.BUTTON_PS, input.controllerState.buttons)
	}
}
