// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
package fi.madekivi.pleikkari.stream.vrui

/**
 * PLE-730: VR Home on the Go (docs/design/vr-ui.md §10.8). It replaces GoVrLibraryFlow's text chooser
 * with a card per linked console, showing its state and a Play pill, and replaces the status text
 * with sheets for finding, waking and connecting to a console. Home is drawn on the in-stream menu's
 * gaze panel, at the same size, whenever the menu is closed, so it adds no layer. The Library flow
 * (or the activity, while a stream connects) describes a page as a [VrHomeState]; [VrHome] builds its
 * widgets, and [VrUiHost] draws the page and routes the pointer and the pad to it.
 */
sealed class VrHomeState
{
	abstract val title: String
	abstract val buttons: List<VrHomeButton>

	/** The linked consoles, then the Oculus TV screen row. [focus]: the card the pad starts on. */
	data class Consoles(
		override val title: String,
		val cards: List<VrConsoleCard>,
		val focus: String?,
		/** The cards' pill. */
		val play: String,
		val oculusTv: String,
		override val buttons: List<VrHomeButton>
	): VrHomeState()

	/**
	 * Finding, waking or connecting, or why not: a message, a spinner while [busy], a few lines
	 * of [detail] and buttons. [back] is what Back does here; null opens the menu, as on the cards.
	 */
	data class Status(
		override val title: String,
		val message: String,
		val detail: List<String>,
		val busy: Boolean,
		override val buttons: List<VrHomeButton>,
		val back: VrHomeAction?
	): VrHomeState()
}

/** How a card's dot shows the console's state. */
enum class VrCardTone { READY, ASLEEP, UNKNOWN }

/** One console. [id] is its MAC address, which Play hands back. */
data class VrConsoleCard(val id: String, val name: String, val detail: String, val tone: VrCardTone)

sealed class VrHomeAction
{
	data class Play(val id: String): VrHomeAction()
	data object Cancel: VrHomeAction()
	data object Retry: VrHomeAction()
	data object Consoles: VrHomeAction()
	data object OculusTv: VrHomeAction()
	data object Settings: VrHomeAction()
	data object Exit: VrHomeAction()
}

data class VrHomeButton(val label: String, val action: VrHomeAction, val style: ButtonStyle = ButtonStyle.SECONDARY)

/** A console card (§10.8): name, state and a Play pill. The whole card is the target, and Play is its one action. */
class VrCard(box: Box, val id: String, var name: String, var detail: String, var tone: VrCardTone, val action: String,
	private val onPlay: () -> Unit): Widget(box)
{
	override fun activate(part: Part) = onPlay()
	val pillBox get() = Box(box.right - PILL_MARGIN - PILL_WIDTH, box.centerY - PILL_HEIGHT / 2,
		box.right - PILL_MARGIN, box.centerY + PILL_HEIGHT / 2)

	companion object
	{
		val PILL_WIDTH = VrUi.deg(8f)
		val PILL_HEIGHT = VrUi.deg(2.8f)
		val PILL_MARGIN = VrUi.deg(0.8f)
	}
}

/** §10.5 Progress, the spinner: an arc the host turns a step at a time while the spinner is shown. */
class VrSpinner(box: Box): Widget(box)
{
	override val interactive get() = false
	/** 0..1 of a turn. */
	var phase = 0f
}

/**
 * One built Home page: its [screen] and the widgets whose words can change in place. A new
 * [VrHomeState] of the same shape updates those words and keeps the pointer's press and the pad's
 * focus. A different shape builds a new page.
 */
class VrHomePage private constructor(state: VrHomeState, val screen: VrScreen, private val cards: List<VrCard>,
	private val message: VrLabel?, private val detail: List<VrLabel>)
{
	var state = state
		private set
	/** What Back does on this page; null opens the menu. */
	val back get() = (state as? VrHomeState.Status)?.back
	val spinner = screen.widgets.filterIsInstance<VrSpinner>().firstOrNull()
	/** For the log. */
	val name get() = when(val s = state)
	{
		is VrHomeState.Consoles -> "${s.cards.size} console card(s)"
		is VrHomeState.Status -> "'${s.message}'"
	}

	/** Takes [next] in place when only its words changed. Returns false when a new page must be built. */
	fun update(next: VrHomeState): Boolean
	{
		if(shape(next) != shape(state))
			return false
		when(next)
		{
			is VrHomeState.Consoles -> next.cards.forEachIndexed { i, card ->
				cards[i].name = card.name
				cards[i].detail = card.detail
				cards[i].tone = card.tone
			}
			is VrHomeState.Status ->
			{
				message?.text = next.message
				next.detail.forEachIndexed { i, line -> detail.getOrNull(i)?.text = line }
			}
		}
		state = next
		return true
	}

	companion object
	{
		/** A page's shape: everything but the words that [update] can change in place. */
		private fun shape(state: VrHomeState): VrHomeState = when(state)
		{
			is VrHomeState.Consoles -> state.copy(focus = null,
				cards = state.cards.map { VrConsoleCard(it.id, "", "", VrCardTone.UNKNOWN) })
			is VrHomeState.Status -> state.copy(message = "", detail = state.detail.map { "" })
		}

		fun build(state: VrHomeState, act: (VrHomeAction) -> Unit): VrHomePage
		{
			val buttons = VrHome.buttonRow(state.buttons, act)
			val primary = buttons.firstOrNull { it.style == ButtonStyle.PRIMARY } ?: buttons.firstOrNull()
			return when(state)
			{
				is VrHomeState.Consoles ->
				{
					val cards = VrHome.cards(state.cards, state.play, act)
					val rows = cards + VrButton(VrHome.rowBelow(cards), state.oculusTv) { act(VrHomeAction.OculusTv) }
					val focus = cards.firstOrNull { it.id == state.focus } ?: cards.firstOrNull() ?: primary
					val screen = VrScreen(VrHome.WIDTH, VrHome.HEIGHT, state.title, buttons,
						listOf(VrList(VrHome.listViewport, rows)), initialFocus = focus)
					VrHomePage(state, screen, cards, null, emptyList())
				}
				is VrHomeState.Status ->
				{
					val (widgets, message, detail) = VrHome.status(state)
					VrHomePage(state, VrScreen(VrHome.WIDTH, VrHome.HEIGHT, state.title, widgets + buttons,
						initialFocus = primary), emptyList(), message, detail)
				}
			}
		}
	}
}

/** Home's layout, in panel texels on the menu's panel (§10.3: 50 x 28 degrees). */
object VrHome
{
	const val WIDTH = VrMenu.WIDTH
	const val HEIGHT = VrMenu.HEIGHT
	val PAD = VrUi.deg(1f)
	val CARD_HEIGHT = VrUi.deg(5.2f)
	val BUTTON_WIDTH = VrUi.deg(14f)
	val SPINNER_SIZE = VrUi.deg(2.5f)
	val LINE_HEIGHT = VrUi.deg(1.9f)
	/** The status sheet's detail lines that fit above its buttons. */
	const val MAX_DETAIL_LINES = 6

	private val buttonsTop = HEIGHT - PAD - VrUi.ROW_HEIGHT
	/** The cards scroll between the header and the button row. */
	val listViewport = Box(PAD, VrUi.HEADER_HEIGHT, WIDTH - PAD, buttonsTop - VrUi.deg(0.8f))

	/** Right-aligned along the bottom, in order, the last one rightmost. */
	fun buttonRow(buttons: List<VrHomeButton>, act: (VrHomeAction) -> Unit): List<VrButton>
	{
		val gap = PAD
		var left = WIDTH - PAD - buttons.size * BUTTON_WIDTH - (buttons.size - 1).coerceAtLeast(0) * gap
		return buttons.map { button ->
			val box = Box(left, buttonsTop, left + BUTTON_WIDTH, buttonsTop + VrUi.ROW_HEIGHT)
			left += BUTTON_WIDTH + gap
			VrButton(box, button.label, button.style) { act(button.action) }
		}
	}

	fun cards(cards: List<VrConsoleCard>, play: String, act: (VrHomeAction) -> Unit): List<VrCard>
	{
		var top = listViewport.top
		return cards.map { card ->
			val box = Box(listViewport.left, top, listViewport.right, top + CARD_HEIGHT)
			top += CARD_HEIGHT + VrUi.ROW_GAP
			VrCard(box, card.id, card.name, card.detail, card.tone, play) { act(VrHomeAction.Play(card.id)) }
		}
	}

	/** A full-width row under [above] (the Oculus TV screen row under the cards). */
	fun rowBelow(above: List<Widget>): Box
	{
		val top = above.maxOfOrNull { it.box.bottom + VrUi.ROW_GAP } ?: listViewport.top
		return Box(listViewport.left, top, listViewport.right, top + VrUi.ROW_HEIGHT)
	}

	/** The status sheet: the spinner and the message on one line, the detail under the message. */
	fun status(state: VrHomeState.Status): Triple<List<Widget>, VrLabel, List<VrLabel>>
	{
		val widgets = mutableListOf<Widget>()
		val top = VrUi.HEADER_HEIGHT + VrUi.deg(2f)
		var left = PAD + VrUi.deg(1f)
		if(state.busy)
		{
			widgets += VrSpinner(Box(left, top, left + SPINNER_SIZE, top + SPINNER_SIZE))
			left += SPINNER_SIZE + VrUi.deg(1.2f)
		}
		val message = VrLabel(Box(left, top, WIDTH - PAD, top + SPINNER_SIZE), state.message, TextRole.LABEL)
		widgets += message
		val detail = state.detail.take(MAX_DETAIL_LINES).mapIndexed { i, line ->
			val y = message.box.bottom + VrUi.deg(1f) + i * LINE_HEIGHT
			VrLabel(Box(left, y, WIDTH - PAD, y + LINE_HEIGHT), line, TextRole.SECONDARY)
		}
		widgets += detail
		return Triple(widgets, message, detail)
	}
}
