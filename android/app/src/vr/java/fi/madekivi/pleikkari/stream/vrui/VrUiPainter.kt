// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
package fi.madekivi.pleikkari.stream.vrui

import android.content.Context
import android.graphics.*
import android.text.TextPaint
import android.text.TextUtils
import androidx.core.content.ContextCompat
import fi.madekivi.pleikkari.R

/**
 * PLE-722: draws a [VrScreen] with Canvas into a panel's Surface (§10.4). System Roboto only, no
 * bundled font or image. Hover is a tint, never a pop or a scale, so text never moves; the pad's
 * focus ring is an accent outline. Colours come from the app's theme (colors.xml), drawn opaque.
 */
class VrUiPainter(context: Context)
{
	private val appContext = context.applicationContext
	private fun color(id: Int) = ContextCompat.getColor(appContext, id)
	private val panel = color(R.color.stream_control_dock_background) or 0xFF000000.toInt()
	private val accent = color(R.color.accent)
	private val destructive = color(R.color.stream_quit)
	private val row = Color.rgb(0x2A, 0x28, 0x30)
	private val rowHover = Color.rgb(0x38, 0x35, 0x3F)
	private val rowPressed = Color.rgb(0x21, 0x1F, 0x25)
	private val accentHover = Color.rgb(0xFF, 0xC6, 0xF4)
	private val accentPressed = Color.rgb(0xE6, 0x91, 0xD4)
	private val destructiveHover = Color.rgb(0x44, 0x2E, 0x2E)
	private val text = Color.rgb(0xEC, 0xEA, 0xF0)
	private val secondary = Color.rgb(0xB5, 0xB1, 0xBA)
	private val disabled = Color.rgb(0x7A, 0x76, 0x80)
	private val onAccent = Color.rgb(0x1C, 0x1B, 0x1F)
	private val trackOff = Color.rgb(0x4A, 0x46, 0x52)

	private val medium = Typeface.create("sans-serif-medium", Typeface.NORMAL)
	private val fill = Paint(Paint.ANTI_ALIAS_FLAG)
	private val stroke = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = VrUi.deg(0.15f); color = accent }
	/** PLE-730: a card's rest-mode dot and the spinner's arc. */
	private val line = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeCap = Paint.Cap.ROUND }
	private val titlePaint = textPaint(VrUi.deg(1.6f), medium)
	private val labelPaint = textPaint(VrUi.deg(1.2f), medium)
	private val secondaryPaint = textPaint(VrUi.deg(1.05f), Typeface.DEFAULT)
	private val rect = RectF()

	private fun textPaint(size: Float, face: Typeface) = TextPaint(Paint.ANTI_ALIAS_FLAG).apply {
		textSize = size
		typeface = face
		isSubpixelText = true
	}

	fun draw(canvas: Canvas, screen: VrScreen, focus: VrFocus)
	{
		canvas.drawColor(Color.TRANSPARENT, PorterDuff.Mode.CLEAR)
		val b = VrUi.BORDER
		fill.color = panel
		rect.set(b, b, screen.width - b, screen.height - b)
		canvas.drawRoundRect(rect, VrUi.PANEL_CORNER, VrUi.PANEL_CORNER, fill)
		if(screen.title.isNotEmpty())
		{
			val pad = VrUi.deg(1f)
			drawText(canvas, screen.title, titlePaint, text, pad, 0f, screen.width - 2 * pad, VrUi.HEADER_HEIGHT, center = false)
		}
		screen.widgets.forEach { drawWidget(canvas, it, focus) }
		screen.lists.forEach { drawList(canvas, it, focus) }
	}

	private fun drawList(canvas: Canvas, list: VrList, focus: VrFocus)
	{
		val v = list.viewport
		canvas.save()
		canvas.clipRect(v.left - VrUi.deg(0.2f), v.top, v.right + VrUi.deg(0.2f), v.bottom)
		canvas.translate(0f, -list.scroll)
		list.children.forEach { if(list.visible(it)) drawWidget(canvas, it, focus) }
		canvas.restore()
		// Fades where the list is clipped, and a thin scroll bar (§10.5).
		val fade = VrUi.deg(1.5f)
		if(list.scroll > 0f)
			drawFade(canvas, v.left, v.top, v.right, v.top + fade, fadeDown = true)
		if(list.scroll < list.maxScroll)
			drawFade(canvas, v.left, v.bottom - fade, v.right, v.bottom, fadeDown = false)
		if(list.maxScroll > 0f)
		{
			val barX = v.right + VrUi.deg(0.3f)
			val thumb = v.height * v.height / (v.height + list.maxScroll)
			val y = v.top + (v.height - thumb) * (list.scroll / list.maxScroll)
			fill.color = trackOff
			rect.set(barX, y, barX + VrUi.deg(0.4f), y + thumb)
			canvas.drawRoundRect(rect, VrUi.deg(0.2f), VrUi.deg(0.2f), fill)
		}
	}

	private fun drawFade(canvas: Canvas, left: Float, top: Float, right: Float, bottom: Float, fadeDown: Boolean)
	{
		val solid = panel
		val clear = panel and 0x00FFFFFF
		fill.shader = LinearGradient(0f, top, 0f, bottom, if(fadeDown) solid else clear, if(fadeDown) clear else solid, Shader.TileMode.CLAMP)
		canvas.drawRect(left, top, right, bottom, fill)
		fill.shader = null
	}

	private fun drawWidget(canvas: Canvas, w: Widget, focus: VrFocus)
	{
		val box = w.box
		when(w)
		{
			is VrLabel -> drawText(canvas, w.text, if(w.role == TextRole.TITLE) titlePaint else if(w.role == TextRole.LABEL) labelPaint else secondaryPaint,
				if(w.role == TextRole.SECONDARY) secondary else text, box.left + VrUi.deg(0.3f), box.top, box.width, box.height, center = false)
			is VrButton ->
			{
				val (bg, fg) = when(w.style)
				{
					ButtonStyle.PRIMARY -> tint(w, accent, accentHover, accentPressed) to onAccent
					ButtonStyle.DESTRUCTIVE -> tint(w, row, destructiveHover, rowPressed) to destructive
					ButtonStyle.SECONDARY -> tint(w, row, rowHover, rowPressed) to text
				}
				roundRect(canvas, box, bg)
				drawText(canvas, w.text, labelPaint, if(w.enabled) fg else disabled, box.left, box.top, box.width, box.height, center = true)
			}
			is VrSegment ->
			{
				val selected = w.isSelected
				val bg = if(selected) tint(w, accent, accentHover, accentPressed) else tint(w, row, rowHover, rowPressed)
				roundRect(canvas, box, bg)
				drawText(canvas, w.text, labelPaint, if(selected) onAccent else text, box.left, box.top, box.width, box.height, center = true)
			}
			is VrToggle ->
			{
				roundRect(canvas, box, tint(w, row, rowHover, rowPressed))
				val pad = VrUi.deg(1f)
				val detail = w.detail
				if(detail == null)
					drawText(canvas, w.label, labelPaint, text, box.left + pad, box.top, box.width - VrUi.deg(6f), box.height, center = false)
				else
				{
					drawText(canvas, w.label, labelPaint, text, box.left + pad, box.top + VrUi.deg(0.2f), box.width - VrUi.deg(6f), box.height / 2, center = false)
					drawText(canvas, detail, secondaryPaint, secondary, box.left + pad, box.centerY - VrUi.deg(0.2f), box.width - VrUi.deg(6f), box.height / 2, center = false)
				}
				val s = w.switchBox
				fill.color = if(w.checked) accent else trackOff
				rect.set(s.left, s.top, s.right, s.bottom)
				canvas.drawRoundRect(rect, s.height / 2, s.height / 2, fill)
				val r = s.height / 2 - VrUi.deg(0.2f)
				fill.color = if(w.checked) onAccent else text
				canvas.drawCircle(if(w.checked) s.right - s.height / 2 else s.left + s.height / 2, s.centerY, r, fill)
			}
			is VrSlider -> drawSlider(canvas, w)
			is VrCard -> drawCard(canvas, w)
			is VrSpinner -> drawSpinner(canvas, w)
		}
		if(focus.ringVisible && focus.focused === w)
		{
			val inset = stroke.strokeWidth / 2
			rect.set(box.left + inset, box.top + inset, box.right - inset, box.bottom - inset)
			canvas.drawRoundRect(rect, VrUi.ROW_CORNER, VrUi.ROW_CORNER, stroke)
		}
	}

	private fun drawSlider(canvas: Canvas, w: VrSlider)
	{
		val box = w.box
		val on = w.enabled
		val pad = VrUi.deg(1f)
		roundRect(canvas, box, if(w.hovered && w.pressedPart == Part.WHOLE && on) rowHover else row)
		drawText(canvas, w.label, labelPaint, if(on) text else disabled, box.left + pad, box.top + VrUi.deg(0.2f), w.labelWidth - pad, box.height / 2, center = false)
		drawText(canvas, w.format(w.current), secondaryPaint, if(on) secondary else disabled, box.left + pad, box.centerY - VrUi.deg(0.2f), w.labelWidth - pad, box.height / 2, center = false)
		for((part, b, sign) in listOf(Triple(Part.MINUS, w.minusBox, "−"), Triple(Part.PLUS, w.plusBox, "+")))
		{
			val bg = when
			{
				!on -> row
				w.pressed && w.pressedPart == part -> rowPressed
				w.hovered -> rowHover
				else -> trackOff
			}
			val inner = Box(b.left + VrUi.deg(0.3f), b.top + VrUi.deg(0.3f), b.right - VrUi.deg(0.3f), b.bottom - VrUi.deg(0.3f))
			roundRect(canvas, inner, bg)
			drawText(canvas, sign, titlePaint, if(on) text else disabled, inner.left, inner.top, inner.width, inner.height, center = true)
		}
		val t = w.trackBox
		val trackH = VrUi.deg(0.3f)
		fill.color = trackOff
		rect.set(t.left, t.centerY - trackH / 2, t.right, t.centerY + trackH / 2)
		canvas.drawRoundRect(rect, trackH / 2, trackH / 2, fill)
		if(on)
		{
			val x = t.left + t.width * w.fraction
			fill.color = accent
			rect.set(t.left, t.centerY - trackH / 2, x, t.centerY + trackH / 2)
			canvas.drawRoundRect(rect, trackH / 2, trackH / 2, fill)
			fill.color = if(w.pressed && w.pressedPart == Part.TRACK) accentPressed else if(w.hovered) accentHover else accent
			canvas.drawCircle(x, t.centerY, VrUi.deg(0.8f), fill)
		}
	}

	/** PLE-730: a Home console card: a state dot, the name over its state, and the Play pill on the right. */
	private fun drawCard(canvas: Canvas, w: VrCard)
	{
		val box = w.box
		roundRect(canvas, box, tint(w, row, rowHover, rowPressed))
		val inset = VrUi.deg(0.6f)
		val lineH = (box.height - 2 * inset) / 2
		val dotX = box.left + VrUi.deg(1.4f)
		val dotY = box.top + inset + lineH / 2
		val dot = VrUi.deg(0.45f)
		when(w.tone)
		{
			VrCardTone.READY -> { fill.color = accent; canvas.drawCircle(dotX, dotY, dot, fill) }
			VrCardTone.ASLEEP ->
			{
				line.color = secondary
				line.strokeWidth = VrUi.deg(0.15f)
				canvas.drawCircle(dotX, dotY, dot - line.strokeWidth / 2, line)
			}
			VrCardTone.UNKNOWN -> { fill.color = disabled; canvas.drawCircle(dotX, dotY, dot, fill) }
		}
		val pill = w.pillBox
		val left = box.left + VrUi.deg(2.6f)
		val width = pill.left - VrUi.deg(1f) - left
		drawText(canvas, w.name, labelPaint, text, left, box.top + inset, width, lineH, center = false)
		drawText(canvas, w.detail, secondaryPaint, secondary, left, box.top + inset + lineH, width, lineH, center = false)
		fill.color = if(w.pressed) accentPressed else if(w.hovered) accentHover else accent
		rect.set(pill.left, pill.top, pill.right, pill.bottom)
		canvas.drawRoundRect(rect, pill.height / 2, pill.height / 2, fill)
		drawText(canvas, w.action, labelPaint, onAccent, pill.left, pill.top, pill.width, pill.height, center = true)
	}

	/** PLE-730: §10.5 Progress, a three-quarter arc the host turns between redraws. */
	private fun drawSpinner(canvas: Canvas, w: VrSpinner)
	{
		val b = w.box
		line.color = accent
		line.strokeWidth = VrUi.deg(0.3f)
		val half = line.strokeWidth / 2
		rect.set(b.left + half, b.top + half, b.right - half, b.bottom - half)
		canvas.drawArc(rect, w.phase * 360f - 90f, 270f, false, line)
	}

	private fun tint(w: Widget, normal: Int, hover: Int, pressed: Int) = when
	{
		!w.enabled -> row
		w.pressed -> pressed
		w.hovered -> hover
		else -> normal
	}

	private fun roundRect(canvas: Canvas, box: Box, color: Int)
	{
		fill.color = color
		rect.set(box.left, box.top, box.right, box.bottom)
		canvas.drawRoundRect(rect, VrUi.ROW_CORNER, VrUi.ROW_CORNER, fill)
	}

	private fun drawText(canvas: Canvas, value: String, paint: TextPaint, color: Int, left: Float, top: Float,
		width: Float, height: Float, center: Boolean)
	{
		paint.color = color
		val shown = TextUtils.ellipsize(value, paint, width, TextUtils.TruncateAt.END).toString()
		val fm = paint.fontMetrics
		val baseline = top + height / 2 - (fm.ascent + fm.descent) / 2
		val x = if(center) left + (width - paint.measureText(shown)) / 2 else left
		canvas.drawText(shown, x, baseline, paint)
	}
}
