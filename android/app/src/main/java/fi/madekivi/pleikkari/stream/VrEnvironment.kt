// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.stream

import android.os.Build
import androidx.annotation.StringRes
import fi.madekivi.pleikkari.R

/**
 * PLE-603: the ambient environment drawn around the curved stream screen in the Oculus Go
 * cinema activity (PLE-602). The renderer is native (src/main/cpp/vr-environment.h); this
 * file holds the setting's vocabulary and the clamped configuration handed to it.
 *
 * Rule 4: [VrEnvironmentKind.PLAIN] is today's picture and the default. The settings only
 * appear where the VR activity can exist ([VrEnvironmentSupport.offered]).
 */
enum class VrEnvironmentKind(val value: String, val nativeValue: Int, @StringRes val title: Int)
{
	PLAIN("plain", 0, R.string.preferences_vr_environment_plain),
	VOID("void", 1, R.string.preferences_vr_environment_void),
	CINEMA("cinema", 2, R.string.preferences_vr_environment_cinema),
	TERRACE("terrace", 3, R.string.preferences_vr_environment_terrace);

	companion object
	{
		val default = PLAIN
		fun fromValue(value: String?) = values().firstOrNull { it.value == value } ?: default
	}
}

/**
 * Screen and room parameters in the units the settings store them in. [toNative] converts
 * to the metres and unit fractions the renderer takes; the ranges mirror the native clamp
 * so a value typed at either end shows what the headset will draw.
 */
data class VrEnvironmentConfig(
	val environment: VrEnvironmentKind = VrEnvironmentKind.default,
	val screenDistanceCm: Int = SCREEN_DISTANCE_CM_DEFAULT,
	val screenWidthCm: Int = SCREEN_WIDTH_CM_DEFAULT,
	val screenCurveRadiusCm: Int = SCREEN_CURVE_RADIUS_CM_DEFAULT,
	val screenHeightOffsetCm: Int = SCREEN_HEIGHT_OFFSET_CM_DEFAULT,
	val glowPercent: Int = GLOW_PERCENT_DEFAULT,
	val roomLightPercent: Int = ROOM_LIGHT_PERCENT_DEFAULT
)
{
	fun clamped() = copy(
		screenDistanceCm = screenDistanceCm.coerceIn(SCREEN_DISTANCE_CM_MIN, SCREEN_DISTANCE_CM_MAX),
		screenWidthCm = screenWidthCm.coerceIn(SCREEN_WIDTH_CM_MIN, SCREEN_WIDTH_CM_MAX),
		screenCurveRadiusCm = if(screenCurveRadiusCm <= 0) 0 else screenCurveRadiusCm.coerceIn(SCREEN_CURVE_RADIUS_CM_MIN, SCREEN_CURVE_RADIUS_CM_MAX),
		screenHeightOffsetCm = screenHeightOffsetCm.coerceIn(-SCREEN_HEIGHT_OFFSET_CM_MAX, SCREEN_HEIGHT_OFFSET_CM_MAX),
		glowPercent = glowPercent.coerceIn(0, 100),
		roomLightPercent = roomLightPercent.coerceIn(0, 100)
	)

	/** Arguments for [VrEnvironmentNative.create] and [VrEnvironmentNative.setConfig]. */
	fun toNative(): VrEnvironmentNativeConfig
	{
		val c = clamped()
		return VrEnvironmentNativeConfig(
			environment = c.environment.nativeValue,
			screenDistanceM = c.screenDistanceCm / 100f,
			screenWidthM = c.screenWidthCm / 100f,
			screenCurveRadiusM = c.screenCurveRadiusCm / 100f,
			screenHeightOffsetM = c.screenHeightOffsetCm / 100f,
			glow = c.glowPercent / 100f,
			roomLight = c.roomLightPercent / 100f
		)
	}

	companion object
	{
		// PLE-602's plain screen: 3 m away, 80 degrees of arc (4.19 m) on a cylinder centred
		// on the viewer, centred on the gaze. The defaults reproduce that picture exactly.
		const val SCREEN_DISTANCE_CM_DEFAULT = 300
		const val SCREEN_DISTANCE_CM_MIN = 80
		const val SCREEN_DISTANCE_CM_MAX = 1200
		const val SCREEN_WIDTH_CM_DEFAULT = 419
		const val SCREEN_WIDTH_CM_MIN = 50
		const val SCREEN_WIDTH_CM_MAX = 1600
		const val SCREEN_CURVE_RADIUS_CM_DEFAULT = 300
		const val SCREEN_CURVE_RADIUS_CM_MIN = 50
		const val SCREEN_CURVE_RADIUS_CM_MAX = 1600
		const val SCREEN_HEIGHT_OFFSET_CM_DEFAULT = 0
		const val SCREEN_HEIGHT_OFFSET_CM_MAX = 200
		const val GLOW_PERCENT_DEFAULT = 60
		const val ROOM_LIGHT_PERCENT_DEFAULT = 35
	}
}

data class VrEnvironmentNativeConfig(
	val environment: Int,
	val screenDistanceM: Float,
	val screenWidthM: Float,
	val screenCurveRadiusM: Float,
	val screenHeightOffsetM: Float,
	val glow: Float,
	val roomLight: Float
)

object VrEnvironmentSupport
{
	/** The Oculus Go is the only device with the VrApi activity; nothing else sees the setting. */
	const val GO_DEVICE = "pacific"

	fun offered(device: String? = Build.DEVICE) = device == GO_DEVICE
}

/**
 * JNI binding to src/main/cpp/vr-environment-jni.cpp (library pleikkari-vr-environment).
 * The VrApi activity's native code calls the C API directly; this binding serves the debug
 * preview and tests. All calls need a current GLES 3 context on the calling thread.
 */
object VrEnvironmentNative
{
	const val TEXTURE_2D = 0x0DE1
	const val TEXTURE_EXTERNAL_OES = 0x8D65

	@Volatile
	private var loaded = false

	/** Returns false when the library cannot be loaded (never true on a build without it). */
	fun load(): Boolean
	{
		if(loaded)
			return true
		return try
		{
			System.loadLibrary("pleikkari-vr-environment")
			loaded = true
			true
		}
		catch(e: LinkageError)
		{
			false
		}
	}

	fun create(config: VrEnvironmentNativeConfig, videoTarget: Int): Long = create(
		config.environment, config.screenDistanceM, config.screenWidthM, config.screenCurveRadiusM,
		config.screenHeightOffsetM, config.glow, config.roomLight, videoTarget)

	fun setConfig(handle: Long, config: VrEnvironmentNativeConfig) = setConfig(
		handle, config.environment, config.screenDistanceM, config.screenWidthM, config.screenCurveRadiusM,
		config.screenHeightOffsetM, config.glow, config.roomLight)

	@JvmStatic external fun create(environment: Int, distance: Float, width: Float, radius: Float,
		heightOffset: Float, glow: Float, roomLight: Float, videoTarget: Int): Long
	@JvmStatic external fun setConfig(handle: Long, environment: Int, distance: Float, width: Float, radius: Float,
		heightOffset: Float, glow: Float, roomLight: Float)
	@JvmStatic external fun beginFrame(handle: Long, texture: Int, transform: FloatArray, hasVideo: Boolean, newFrame: Boolean)
	@JvmStatic external fun drawEye(handle: Long, view: FloatArray, projection: FloatArray, texture: Int, transform: FloatArray, hasVideo: Boolean)
	/** out[0] = GPU ns of the last frame (0 without timer queries), out[1] = draw calls, out[2] = triangles, out[3] = room vertex bytes. */
	@JvmStatic external fun stats(handle: Long, out: LongArray)
	/** PLE-650: debug preview only; 0 is the shipped sky dome. */
	@JvmStatic external fun debugSetSkyVariant(handle: Long, variant: Int)
	@JvmStatic external fun destroy(handle: Long)
}
