// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
package fi.madekivi.pleikkari.stream.vrui

/**
 * PLE-733: the address pad (docs/design/vr-ui.md §10.7) for a console's manual address, the one
 * discovery cannot find. Shown as a modal on the menu panel, as [VrPinPad] is: a page of digits
 * with `.`, `:` and `-`, and an `abc` page for host names. [StreamVrActivity] backs it with the
 * Library flow; tests back it with plain fields.
 */
interface VrAddressPadModel
{
	/** The trimmed address, once it is not blank. */
	fun save(address: String)
	fun cancel()
}

/** The pad's words; the activity fills them from resources, tests from literals. */
data class VrAddressPadText(
	val title: String,
	val save: String,
	val cancel: String,
	val clear: String,
	val backspace: String,
	val letters: String,
	val digits: String
)

/** One address pad screen and the address typed on it, starting from [initial]. */
class VrAddressPad(private val model: VrAddressPadModel, private val text: VrAddressPadText, initial: String = "")
{
	var address = initial.take(MAX_LENGTH)
		private set
	var lettersPage = false
		private set
	private val field: VrLabel
	private val keys: List<VrButton>
	private val page: VrButton
	private val clear: VrButton
	private val backspace: VrButton
	private val save: VrButton
	val screen: VrScreen

	init
	{
		val pad = VrUi.deg(1f)
		val gap = VrUi.ROW_GAP
		val top = VrUi.HEADER_HEIGHT
		val keyW = VrUi.deg(4f)
		val keyH = VrUi.deg(3.5f)
		val gridW = COLUMNS * keyW + (COLUMNS - 1) * gap
		val actionW = VrUi.deg(13.75f)
		val left = (WIDTH - gridW - pad * 2 - actionW) / 2
		val fieldH = VrUi.deg(3f)
		field = VrLabel(Box(left, top, left + gridW, top + fieldH), "", TextRole.TITLE)
		val keysTop = top + fieldH + gap
		fun box(col: Int, row: Int, span: Int = 1): Box
		{
			val x = left + col * (keyW + gap)
			val y = keysTop + row * (keyH + gap)
			return Box(x, y, x + span * keyW + (span - 1) * gap, y + keyH)
		}
		keys = (0 until COLUMNS * ROWS).map { i ->
			lateinit var key: VrButton
			key = VrButton(box(i % COLUMNS, i / COLUMNS), "") { type(key.text) }
			key
		}
		page = VrButton(box(0, ROWS, 2), "") { lettersPage = !lettersPage }
		clear = VrButton(box(2, ROWS, 2), text.clear) { address = "" }
		backspace = VrButton(box(4, ROWS, 3), text.backspace) { address = address.dropLast(1) }
		val actionLeft = left + gridW + pad * 2
		val rowH = VrUi.ROW_HEIGHT
		save = VrButton(Box(actionLeft, keysTop, actionLeft + actionW, keysTop + rowH), text.save, ButtonStyle.PRIMARY) {
			address.trim().takeIf { it.isNotEmpty() }?.let(model::save)
		}
		val gridBottom = keysTop + (ROWS + 1) * keyH + ROWS * gap
		val cancel = VrButton(Box(actionLeft, gridBottom - rowH, actionLeft + actionW, gridBottom), text.cancel) {
			model.cancel()
		}
		screen = VrScreen(WIDTH, HEIGHT, text.title,
			listOf(field) + keys + listOf(page, clear, backspace, save, cancel), initialFocus = keys.first())
		refresh()
	}

	private fun type(key: String)
	{
		if(key.isNotEmpty() && address.length < MAX_LENGTH)
			address += key
	}

	/** After every activation: the field, the page's keys, and what is enabled. */
	fun refresh()
	{
		val shown = if(address.length > FIELD_CHARS) "…" + address.takeLast(FIELD_CHARS - 1) else address
		field.text = "${shown}_"
		val labels = if(lettersPage) LETTERS else DIGITS
		keys.forEachIndexed { i, key ->
			key.text = labels.getOrNull(i) ?: ""
			key.enabled = key.text.isNotEmpty()
		}
		page.text = if(lettersPage) text.digits else text.letters
		clear.enabled = address.isNotEmpty()
		backspace.enabled = address.isNotEmpty()
		save.enabled = address.isNotBlank()
	}

	companion object
	{
		const val WIDTH = VrMenu.WIDTH
		const val HEIGHT = VrMenu.HEIGHT
		const val COLUMNS = 7
		const val ROWS = 4
		/** A DNS name's limit. */
		const val MAX_LENGTH = 253
		/** What fits the field at the title size; a longer address shows its tail. */
		const val FIELD_CHARS = 28
		/** IPv4, IPv6 (`:` and hex on the letters page) and a port. */
		val DIGITS = listOf("1", "2", "3", "4", "5", "6", "7", "8", "9", "0", ".", ":", "-")
		/** Host names: a to z, `.` and `-`, one full grid. */
		val LETTERS = ('a'..'z').map { it.toString() } + listOf(".", "-")
	}
}
