// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
package fi.madekivi.pleikkari.stream.vrui

import fi.madekivi.pleikkari.lib.StreamStatsEvent
import fi.madekivi.pleikkari.stream.VrEnvironmentConfig
import fi.madekivi.pleikkari.stream.VrEnvironmentKind
import java.util.Locale
import kotlin.math.atan
import kotlin.math.atan2
import kotlin.math.max
import kotlin.math.min

/**
 * PLE-722: the stats overlay of the in-stream menu, as a small panel at the picture's top-left
 * corner (§10.5 "Stats readout"), redrawn once a second. Non-interactive.
 */
object VrStatsPanel
{
	/** 25 x 7 degrees. */
	const val WIDTH = 400
	const val HEIGHT = 112
	private const val HALF_W_DEG = WIDTH / VrUi.TEXELS_PER_DEGREE / 2
	private const val HALF_H_DEG = HEIGHT / VrUi.TEXELS_PER_DEGREE / 2
	private const val MARGIN_DEG = 1f
	/** PLE-602's plain screen: 80.2 x 43 degrees, centred on the gaze. */
	private const val PLAIN_HALF_W_DEG = 40.1f
	private const val PLAIN_HALF_H_DEG = 21.5f

	/** Degrees right and up of the picture's middle for the panel's middle: inside its top-left corner. */
	fun anchor(config: VrEnvironmentConfig): Pair<Float, Float>
	{
		val (halfW, halfH, centreY) = pictureAngles(config)
		val x = -max(halfW - HALF_W_DEG - MARGIN_DEG, 0f)
		val y = centreY + max(halfH - HALF_H_DEG - MARGIN_DEG, 0f)
		return x to y
	}

	/** Half width, half height and centre elevation of the picture, degrees from the viewer. */
	fun pictureAngles(config: VrEnvironmentConfig): Triple<Float, Float, Float>
	{
		val c = config.clamped()
		if(c.environment == VrEnvironmentKind.PLAIN)
			return Triple(PLAIN_HALF_W_DEG, PLAIN_HALF_H_DEG, 0f)
		val d = c.screenDistanceCm / 100f
		val w = c.screenWidthCm / 100f
		// A curved screen centred on the viewer spans its arc length over the distance.
		val halfW = if(c.screenCurveRadiusCm > 0) min(w / 2 / d, Math.PI.toFloat() / 2) else atan2(w / 2, d)
		val halfH = atan(w * 9f / 16f / 2f / d)
		val centre = atan(c.screenHeightOffsetCm / 100f / d)
		return Triple(degrees(halfW), degrees(halfH), degrees(centre))
	}

	/** Three lines for the readout; [stats] is null before the first second of a stream, or in the preview. */
	fun lines(stats: StreamStatsEvent?, panelHz: Float, preview: Boolean): List<String>
	{
		if(stats == null)
			return listOf(
				String.format(Locale.US, "panel %.0f Hz", panelHz),
				if(preview) "preview: no console" else "waiting for stream stats",
				"")
		val interval = stats.intervalMillis.coerceAtLeast(1)
		fun rate(count: Long) = count * 1000.0 / interval
		return listOf(
			String.format(Locale.US, "stream %.1f fps · decoder %.1f fps · panel %.0f Hz",
				rate(stats.streamFrames), rate(stats.decoderFrames), panelHz),
			// Lost and dropped are session totals (PLE-474, PLE-484).
			String.format(Locale.US, "decode %.1f ms · this stream: lost %d, dropped %d",
				stats.decodeMeanMicros / 1000.0, stats.videoFramesLost, stats.presenterFramesDropped + stats.decoderInputFramesDropped),
			String.format(Locale.US, "network %.1f / %.1f Mbps · RTT %.1f ms",
				stats.measuredThroughputBps / 1e6, stats.targetBitrateBps / 1e6, stats.measuredRttMicros / 1000.0))
	}

	fun screen(lines: List<String>): VrScreen
	{
		val pad = VrUi.deg(1f)
		val lineH = VrUi.deg(1.05f * 1.3f)
		val top = (HEIGHT - lineH * lines.size) / 2
		return VrScreen(WIDTH, HEIGHT, "", lines.mapIndexed { i, line ->
			VrLabel(Box(pad, top + i * lineH, WIDTH - pad, top + (i + 1) * lineH), line, if(i == 0) TextRole.LABEL else TextRole.SECONDARY)
		})
	}

	private fun degrees(rad: Float) = rad * 57.29578f
}
