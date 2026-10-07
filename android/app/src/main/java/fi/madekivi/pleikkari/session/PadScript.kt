// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.session

import fi.madekivi.pleikkari.lib.ControllerState
import kotlin.math.roundToInt

class PadScriptException(message: String): Exception(message)

/** One step of a [PadScript]. */
sealed class PadStep
{
	abstract val durationMs: Long

	/** The pad held in [state] for [holdMs], then every button up and stick centred for [gapMs]. */
	data class Press(val token: String, val state: ControllerState, val holdMs: Long, val gapMs: Long): PadStep()
	{
		override val durationMs get() = holdMs + gapMs
	}

	/** Nothing pressed for [ms]. */
	data class Wait(val ms: Long): PadStep()
	{
		override val durationMs get() = ms
	}
}

/**
 * PLE-869: a sequence of DualSense presses for [PadScriptReceiver], parsed from one adb string so a
 * whole menu path costs one broadcast. Tokens are separated by spaces or commas, case-insensitive:
 *
 * - `cross`, `circle`, `square`, `triangle` (`x`, `o`), `up`, `down`, `left`, `right`, `l1`, `r1`,
 *   `l2`, `r2`, `l3`, `r3`, `options`, `create` (`share`), `ps`, `touchpad` (`tp`): a tap.
 * - `lx`, `ly`, `rx`, `ry` `=`-1..1 (negative is left/up), `l2`, `r2` `=`0..1: a stick or analog
 *   trigger, e.g. `ly=-1` pushes the left stick fully up.
 * - `a+b`: a chord, every part held together, e.g. `l1+r1` or `r2+lx=0.5`.
 * - `token*N`: N taps, each followed by the gap, e.g. `down*3`.
 * - `token:MS`: hold for MS instead of the default hold, e.g. `ps:1200`; after a repeat, `down*3:60`.
 * - `wait:MS`: nothing pressed for MS, e.g. while a menu animates in.
 */
data class PadScript(val steps: List<PadStep>)
{
	val durationMs: Long get() = steps.sumOf { it.durationMs }
	val presses: Int get() = steps.count { it is PadStep.Press }

	companion object
	{
		const val DEFAULT_HOLD_MS = 100L
		const val DEFAULT_GAP_MS = 100L
		/** Below about two input frames the console can miss a press; above this, use several tokens. */
		const val MIN_HOLD_MS = 20L
		const val MAX_HOLD_MS = 10_000L
		const val MAX_WAIT_MS = 30_000L
		const val MAX_REPEAT = 50
		const val MAX_STEPS = 500

		private val BUTTONS = mapOf(
			"cross" to ControllerState.BUTTON_CROSS, "x" to ControllerState.BUTTON_CROSS,
			"circle" to ControllerState.BUTTON_MOON, "o" to ControllerState.BUTTON_MOON,
			"square" to ControllerState.BUTTON_BOX,
			"triangle" to ControllerState.BUTTON_PYRAMID,
			"up" to ControllerState.BUTTON_DPAD_UP, "down" to ControllerState.BUTTON_DPAD_DOWN,
			"left" to ControllerState.BUTTON_DPAD_LEFT, "right" to ControllerState.BUTTON_DPAD_RIGHT,
			"l1" to ControllerState.BUTTON_L1, "r1" to ControllerState.BUTTON_R1,
			"l3" to ControllerState.BUTTON_L3, "r3" to ControllerState.BUTTON_R3,
			"options" to ControllerState.BUTTON_OPTIONS,
			"create" to ControllerState.BUTTON_SHARE, "share" to ControllerState.BUTTON_SHARE,
			"ps" to ControllerState.BUTTON_PS,
			"touchpad" to ControllerState.BUTTON_TOUCHPAD, "tp" to ControllerState.BUTTON_TOUCHPAD)

		private val TOKEN = Regex("""^([a-z0-9+=.\-]+?)(?:\*(\d+))?(?::(\d+))?$""")
		private val WAIT = Regex("""^wait:(\d+)$""")

		fun parse(text: String, holdMs: Long = DEFAULT_HOLD_MS, gapMs: Long = DEFAULT_GAP_MS): PadScript
		{
			if(holdMs !in MIN_HOLD_MS..MAX_HOLD_MS)
				throw PadScriptException("hold_ms $holdMs is outside $MIN_HOLD_MS..$MAX_HOLD_MS")
			if(gapMs !in 0L..MAX_WAIT_MS)
				throw PadScriptException("gap_ms $gapMs is outside 0..$MAX_WAIT_MS")
			val tokens = text.lowercase().split(' ', ',', '\t', '\n').filter { it.isNotEmpty() }
			if(tokens.isEmpty())
				throw PadScriptException("empty sequence")
			val steps = mutableListOf<PadStep>()
			for(token in tokens)
			{
				val wait = WAIT.matchEntire(token)
				if(wait != null)
				{
					val ms = wait.groupValues[1].toLongOrNull()?.takeIf { it <= MAX_WAIT_MS }
						?: throw PadScriptException("'$token': wait is over $MAX_WAIT_MS ms")
					steps += PadStep.Wait(ms)
					continue
				}
				val match = TOKEN.matchEntire(token) ?: throw PadScriptException("'$token': not a pad token")
				val (chord, repeatText, holdText) = match.destructured
				val count = if(repeatText.isEmpty()) 1 else repeatText.toIntOrNull()?.takeIf { it in 1..MAX_REPEAT }
					?: throw PadScriptException("'$token': repeat is outside 1..$MAX_REPEAT")
				val hold = if(holdText.isEmpty()) holdMs else holdText.toLongOrNull()?.takeIf { it in MIN_HOLD_MS..MAX_HOLD_MS }
					?: throw PadScriptException("'$token': hold is outside $MIN_HOLD_MS..$MAX_HOLD_MS ms")
				val state = chordState(token, chord)
				repeat(count) { steps += PadStep.Press(token, state, hold, gapMs) }
				if(steps.size > MAX_STEPS)
					throw PadScriptException("over $MAX_STEPS steps; send the rest in another broadcast")
			}
			return PadScript(steps)
		}

		private fun chordState(token: String, chord: String): ControllerState
		{
			val state = ControllerState()
			for(part in chord.split('+'))
			{
				if(part.isEmpty())
					throw PadScriptException("'$token': empty chord part")
				val eq = part.indexOf('=')
				if(eq < 0)
				{
					when(part)
					{
						"l2" -> state.l2State = UByte.MAX_VALUE
						"r2" -> state.r2State = UByte.MAX_VALUE
						else -> state.buttons = state.buttons or (BUTTONS[part]
							?: throw PadScriptException("'$token': unknown button '$part'"))
					}
					continue
				}
				val axis = part.substring(0, eq)
				val value = part.substring(eq + 1).toFloatOrNull()
					?: throw PadScriptException("'$token': '$part' needs a number")
				fun stick(): Short
				{
					if(value !in -1f..1f)
						throw PadScriptException("'$token': $axis is outside -1..1")
					return (value * Short.MAX_VALUE).roundToInt().toShort()
				}
				fun trigger(): UByte
				{
					if(value !in 0f..1f)
						throw PadScriptException("'$token': $axis is outside 0..1")
					return (value * UByte.MAX_VALUE.toFloat()).roundToInt().toUByte()
				}
				when(axis)
				{
					"lx" -> state.leftX = stick()
					"ly" -> state.leftY = stick()
					"rx" -> state.rightX = stick()
					"ry" -> state.rightY = stick()
					"l2" -> state.l2State = trigger()
					"r2" -> state.r2State = trigger()
					else -> throw PadScriptException("'$token': unknown axis '$axis'")
				}
			}
			return state
		}
	}
}
