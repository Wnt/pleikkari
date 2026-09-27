// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
package fi.madekivi.pleikkari.stream.vrui

/**
 * PLE-731: the console login PIN pad (docs/design/vr-ui.md §10.7), shown on
 * `StreamStateLoginPinRequest` in place of the old "use Oculus TV" message. [StreamVrActivity]
 * backs it with the session; tests back it with plain fields.
 */
interface VrPinPadModel
{
	/** The whole PIN, once [VrPinPad.LENGTH] digits are in. */
	fun submit(pin: String)
	/** Quit, as the phone dialog's Quit and Back do. */
	fun quit()
}

/** The pad's words; the activity fills them from resources, tests from literals. */
data class VrPinPadText(
	val title: String,
	val titleIncorrect: String,
	val connect: String,
	val quit: String,
	val clear: String,
	val backspace: String
)

/** One PIN pad screen and the digits typed on it; a new request builds a new one. */
class VrPinPad(private val model: VrPinPadModel, text: VrPinPadText, incorrect: Boolean)
{
	var digits = ""
		private set
	private val slots: VrLabel
	private val clear: VrButton
	private val backspace: VrButton
	private val connect: VrButton
	val screen: VrScreen

	init
	{
		val pad = VrUi.deg(1f)
		val gap = VrUi.ROW_GAP
		val top = VrUi.HEADER_HEIGHT
		val keyW = VrUi.deg(6f)
		// §10.7 asks 4.8 degrees; four rows of that and the slots outgrow the 28-degree panel.
		val keyH = VrUi.deg(4.4f)
		val gridW = 3 * keyW + 2 * gap
		val actionW = VrUi.deg(13.75f)
		// The grid and the action column, centred together.
		val left = (WIDTH - gridW - pad * 2 - actionW) / 2
		val slotH = VrUi.deg(3f)
		slots = VrLabel(Box(left, top, left + gridW, top + slotH), "", TextRole.TITLE)
		val keysTop = top + slotH + gap
		fun key(col: Int, row: Int, label: String, onClick: () -> Unit): VrButton
		{
			val x = left + col * (keyW + gap)
			val y = keysTop + row * (keyH + gap)
			return VrButton(Box(x, y, x + keyW, y + keyH), label, onClick = onClick)
		}
		val keys = mutableListOf<Widget>()
		for(d in 1..9)
			keys += key((d - 1) % 3, (d - 1) / 3, d.toString()) { type(d) }
		clear = key(0, 3, text.clear) { digits = "" }
		keys += clear
		keys += key(1, 3, "0") { type(0) }
		backspace = key(2, 3, text.backspace) { digits = digits.dropLast(1) }
		keys += backspace
		val actionLeft = left + gridW + pad * 2
		val rowH = VrUi.ROW_HEIGHT
		connect = VrButton(Box(actionLeft, keysTop, actionLeft + actionW, keysTop + rowH), text.connect, ButtonStyle.PRIMARY) {
			if(digits.length == LENGTH) model.submit(digits)
		}
		val gridBottom = keysTop + 4 * keyH + 3 * gap
		val quit = VrButton(Box(actionLeft, gridBottom - rowH, actionLeft + actionW, gridBottom), text.quit, ButtonStyle.DESTRUCTIVE) {
			model.quit()
		}
		screen = VrScreen(WIDTH, HEIGHT, if(incorrect) text.titleIncorrect else text.title,
			listOf(slots) + keys + listOf(connect, quit), titleAlert = incorrect)
		refresh()
	}

	private fun type(digit: Int)
	{
		if(digits.length < LENGTH)
			digits += digit.toString()
	}

	/** After every activation: the slots, and what is enabled (§10.7). */
	fun refresh()
	{
		slots.text = (0 until LENGTH).joinToString("  ") { if(it < digits.length) "•" else "_" }
		clear.enabled = digits.isNotEmpty()
		backspace.enabled = digits.isNotEmpty()
		connect.enabled = digits.length == LENGTH
	}

	companion object
	{
		/** The PS4 and PS5 login PIN is four digits. */
		const val LENGTH = 4
		const val WIDTH = VrMenu.WIDTH
		const val HEIGHT = VrMenu.HEIGHT
		/** PLE-757: §10.7's wrong-PIN shake, 0.3 degrees for 0.4 s. */
		const val SHAKE_NS = 400_000_000L
		private const val SHAKE_CYCLES = 3

		/** The title's offset [elapsedNs] into the shake: a decaying sine, back to 0 at and after [SHAKE_NS]. */
		fun shakeShift(elapsedNs: Long): Float
		{
			if(elapsedNs <= 0 || elapsedNs >= SHAKE_NS)
				return 0f
			val t = elapsedNs.toFloat() / SHAKE_NS
			return VrUi.deg(0.3f) * (1f - t) * kotlin.math.sin(2f * Math.PI.toFloat() * SHAKE_CYCLES * t)
		}
	}
}
