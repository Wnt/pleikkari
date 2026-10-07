// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.session

import fi.madekivi.pleikkari.lib.ControllerState
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.Collections
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/** PLE-869: the adb pad script's grammar, and the runner that plays it into StreamInput. */
class PadScriptTest
{
	private fun presses(text: String, hold: Long = PadScript.DEFAULT_HOLD_MS, gap: Long = PadScript.DEFAULT_GAP_MS) =
		PadScript.parse(text, hold, gap).steps.map { it as PadStep.Press }

	private fun error(text: String) = assertThrows(PadScriptException::class.java) { PadScript.parse(text) }.message!!

	@Test
	fun everyButtonNameAndAlias()
	{
		val expected = mapOf(
			"cross" to ControllerState.BUTTON_CROSS, "x" to ControllerState.BUTTON_CROSS,
			"circle" to ControllerState.BUTTON_MOON, "o" to ControllerState.BUTTON_MOON,
			"square" to ControllerState.BUTTON_BOX, "triangle" to ControllerState.BUTTON_PYRAMID,
			"up" to ControllerState.BUTTON_DPAD_UP, "down" to ControllerState.BUTTON_DPAD_DOWN,
			"left" to ControllerState.BUTTON_DPAD_LEFT, "right" to ControllerState.BUTTON_DPAD_RIGHT,
			"l1" to ControllerState.BUTTON_L1, "r1" to ControllerState.BUTTON_R1,
			"l3" to ControllerState.BUTTON_L3, "r3" to ControllerState.BUTTON_R3,
			"options" to ControllerState.BUTTON_OPTIONS, "create" to ControllerState.BUTTON_SHARE,
			"share" to ControllerState.BUTTON_SHARE, "ps" to ControllerState.BUTTON_PS,
			"touchpad" to ControllerState.BUTTON_TOUCHPAD, "tp" to ControllerState.BUTTON_TOUCHPAD)
		for((name, mask) in expected)
		{
			val press = presses(name).single()
			assertEquals(name, ControllerState(buttons = mask), press.state)
			assertEquals(PadScript.DEFAULT_HOLD_MS, press.holdMs)
			assertEquals(PadScript.DEFAULT_GAP_MS, press.gapMs)
		}
	}

	@Test
	fun digitalTriggersPressFully()
	{
		assertEquals(ControllerState(l2State = 255U), presses("l2").single().state)
		assertEquals(ControllerState(r2State = 255U), presses("R2").single().state)
	}

	@Test
	fun separatorsAndCase()
	{
		val steps = presses(" Down,DOWN \tcross\n")
		assertEquals(listOf("down", "down", "cross"), steps.map { it.token })
	}

	@Test
	fun repeatHoldAndBoth()
	{
		val repeated = presses("down*3")
		assertEquals(3, repeated.size)
		assertTrue(repeated.all { it.state == ControllerState(buttons = ControllerState.BUTTON_DPAD_DOWN) })
		assertEquals(1200L, presses("ps:1200").single().holdMs)
		val both = presses("right*2:60")
		assertEquals(listOf(60L, 60L), both.map { it.holdMs })
	}

	@Test
	fun defaultsFromTheBroadcast()
	{
		val press = presses("cross", hold = 40, gap = 0).single()
		assertEquals(40L, press.holdMs)
		assertEquals(0L, press.gapMs)
		assertEquals(80L, presses("cross:80", hold = 40).single().holdMs)
	}

	@Test
	fun chordsSticksAndAnalogTriggers()
	{
		assertEquals(ControllerState(buttons = ControllerState.BUTTON_L1 or ControllerState.BUTTON_R1),
			presses("l1+r1").single().state)
		assertEquals(ControllerState(leftY = (-32767).toShort()), presses("ly=-1").single().state)
		assertEquals(ControllerState(leftX = 16384, rightX = 32767, rightY = 0),
			presses("lx=0.5+rx=1+ry=0").single().state)
		assertEquals(ControllerState(r2State = 128U, leftX = (-32767).toShort()),
			presses("r2=0.5+lx=-1").single().state)
		val held = presses("lx=-1:300").single()
		assertEquals(300L, held.holdMs)
		assertEquals((-32767).toShort(), held.state.leftX)
	}

	@Test
	fun waitsAndDuration()
	{
		val script = PadScript.parse("down wait:500 cross:200", 100, 50)
		assertEquals(PadStep.Wait(500), script.steps[1])
		assertEquals(2, script.presses)
		assertEquals(150L + 500L + 250L, script.durationMs)
	}

	@Test
	fun errorsNameTheToken()
	{
		assertTrue(error("").contains("empty"))
		assertTrue(error("cross jump").contains("'jump'"))
		assertTrue(error("l1+").contains("empty chord part"))
		assertTrue(error("lx=2").contains("outside -1..1"))
		assertTrue(error("l2=-0.5").contains("outside 0..1"))
		assertTrue(error("lx=left").contains("needs a number"))
		assertTrue(error("lz=1").contains("unknown axis"))
		assertTrue(error("down*0").contains("repeat"))
		assertTrue(error("down*51").contains("repeat"))
		assertTrue(error("cross:5").contains("hold"))
		assertTrue(error("wait:30001").contains("wait"))
		assertTrue(error("down:50*2").contains("not a pad token"))
		assertTrue(error("down*50 ".repeat(11)).contains("steps"))
		assertThrows(PadScriptException::class.java) { PadScript.parse("cross", holdMs = 5) }
		assertThrows(PadScriptException::class.java) { PadScript.parse("cross", gapMs = -1) }
	}

	@Test
	fun runnerPressesReleasesAndKeepsOrder()
	{
		val sent = Collections.synchronizedList(mutableListOf<ControllerState>())
		val runner = PadScriptRunner { sent += it }
		val done = CountDownLatch(2)
		val outcomes = Collections.synchronizedList(mutableListOf<PadScriptRunner.Outcome>())
		runner.submit(PadScript.parse("cross down", 20, 0)) { outcomes += it; done.countDown() }
		runner.submit(PadScript.parse("wait:10 l1+r1", 20, 5)) { outcomes += it; done.countDown() }
		assertTrue(done.await(5, TimeUnit.SECONDS))
		runner.shutdown()
		val released = ControllerState()
		assertEquals(listOf(
			ControllerState(buttons = ControllerState.BUTTON_CROSS), released,
			ControllerState(buttons = ControllerState.BUTTON_DPAD_DOWN), released,
			ControllerState(buttons = ControllerState.BUTTON_L1 or ControllerState.BUTTON_R1), released), sent)
		assertEquals(listOf(2, 1), outcomes.map { it.presses })
		assertTrue(outcomes.none { it.cancelled })
		assertTrue(outcomes[0].elapsedMs >= 40)
	}

	@Test
	fun cancelReleasesTheHeldButtonAndDropsTheQueue()
	{
		val sent = Collections.synchronizedList(mutableListOf<ControllerState>())
		val runner = PadScriptRunner { sent += it }
		val done = CountDownLatch(2)
		val outcomes = Collections.synchronizedList(mutableListOf<PadScriptRunner.Outcome>())
		runner.submit(PadScript.parse("ps:10000 cross")) { outcomes += it; done.countDown() }
		runner.submit(PadScript.parse("triangle")) { outcomes += it; done.countDown() }
		// Cancel while the 10 s PS hold is down.
		val deadline = System.nanoTime() + 5_000_000_000L
		while(sent.isEmpty() && System.nanoTime() < deadline)
			Thread.sleep(5)
		runner.cancel()
		assertTrue(done.await(5, TimeUnit.SECONDS))
		runner.shutdown()
		assertEquals(listOf(ControllerState(buttons = ControllerState.BUTTON_PS), ControllerState()), sent)
		assertEquals(listOf(true, true), outcomes.map { it.cancelled })
		assertEquals(listOf(1, 0), outcomes.map { it.presses })
		assertTrue(outcomes[0].elapsedMs < 5_000)
	}
}
