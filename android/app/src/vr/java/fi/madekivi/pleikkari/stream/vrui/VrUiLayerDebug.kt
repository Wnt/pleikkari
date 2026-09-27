// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
package fi.madekivi.pleikkari.stream.vrui

/**
 * PLE-761: debug-build switches for PLE-735's panel layer measurement on the Go, from
 * `adb shell setprop debug.pleikkari.vr_ui_layers <spec>` before the stream starts
 * (vr-ui-layers.h's VrUiDebug). [DEFAULT] is PLE-722's shipped panels.
 *
 * The spec is comma separated, in any order:
 * - `quad`: a flat projection-layer quad instead of the cylinder layer
 * - `overlay`: panels submitted after the eye buffer (no footprint; the laser and reticle, drawn in
 *   the eye buffer, then sit under the panel unless `reticle` is on too)
 * - `tpd=N`: N texels per degree instead of [VrUi.TEXELS_PER_DEGREE] (8..32, same angular size)
 * - `filter`: VRAPI_FRAME_LAYER_FLAG_FILTER_EXPENSIVE on the panel layers
 * - `panels=N`: at most N panel layers (1: the menu only, never the stats readout)
 * - `reticle`: PLE-776, the laser and reticle in their own eye-sized projection layer submitted
 *   last, over the panels (with `overlay`, a working pointer; with the underlay, the same extra
 *   layer's cost for a like-for-like comparison)
 */
data class VrUiLayerDebug(
	val quad: Boolean = false,
	val overlay: Boolean = false,
	val texelsPerDegree: Float = VrUi.TEXELS_PER_DEGREE,
	val filterExpensive: Boolean = false,
	val maxPanels: Int = 2,
	val reticleLayer: Boolean = false
)
{
	/** The panel texture's texels per toolkit texel: the Canvas scale. */
	val texelScale get() = texelsPerDegree / VrUi.TEXELS_PER_DEGREE

	val isDefault get() = this == DEFAULT

	companion object
	{
		val DEFAULT = VrUiLayerDebug()

		fun parse(spec: String): VrUiLayerDebug =
			spec.split(',').map { it.trim().lowercase() }.filter { it.isNotEmpty() }.fold(DEFAULT) { debug, item ->
				val value = item.substringAfter('=', "")
				when
				{
					item == "quad" -> debug.copy(quad = true)
					item == "cylinder" -> debug.copy(quad = false)
					item == "overlay" -> debug.copy(overlay = true)
					item == "underlay" -> debug.copy(overlay = false)
					item == "reticle" -> debug.copy(reticleLayer = true)
					item == "filter" -> debug.copy(filterExpensive = true)
					item.startsWith("tpd=") -> value.toFloatOrNull()?.let { debug.copy(texelsPerDegree = it.coerceIn(8f, 32f)) } ?: debug
					item.startsWith("panels=") -> value.toIntOrNull()?.let { debug.copy(maxPanels = it.coerceIn(1, 2)) } ?: debug
					else -> debug
				}
			}
	}
}
