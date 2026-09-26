// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.stream

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class VrEnvironmentConfigTest
{
	@Test
	fun defaultsReproducePle602PlainScreen()
	{
		val native = VrEnvironmentConfig().toNative()
		assertEquals(VrEnvironmentKind.PLAIN.nativeValue, native.environment)
		assertEquals(3.0f, native.screenDistanceM, 1e-6f)
		// 80 degrees of arc on a 3 m cylinder: 4.19 m along the surface.
		assertEquals(4.19f, native.screenWidthM, 0.005f)
		assertEquals(3.0f, native.screenCurveRadiusM, 1e-6f)
		assertEquals(0f, native.screenHeightOffsetM, 1e-6f)
	}

	@Test
	fun unknownEnvironmentValueFallsBackToPlain()
	{
		assertEquals(VrEnvironmentKind.PLAIN, VrEnvironmentKind.fromValue(null))
		assertEquals(VrEnvironmentKind.PLAIN, VrEnvironmentKind.fromValue("spaceship"))
		assertEquals(VrEnvironmentKind.CINEMA, VrEnvironmentKind.fromValue("cinema"))
	}

	@Test
	fun nativeValuesMatchTheEnumInTheHeader()
	{
		// vr-environment.h: PLAIN 0, VOID 1, CINEMA 2, TERRACE 3.
		assertEquals(listOf(0, 1, 2, 3), VrEnvironmentKind.values().map { it.nativeValue })
	}

	@Test
	fun clampKeepsGeometryInsideTheRendererRanges()
	{
		val c = VrEnvironmentConfig(
			screenDistanceCm = 5, screenWidthCm = 99999, screenCurveRadiusCm = -3,
			screenHeightOffsetCm = -999, glowPercent = 250, roomLightPercent = -1
		).clamped()
		assertEquals(VrEnvironmentConfig.SCREEN_DISTANCE_CM_MIN, c.screenDistanceCm)
		assertEquals(VrEnvironmentConfig.SCREEN_WIDTH_CM_MAX, c.screenWidthCm)
		assertEquals(0, c.screenCurveRadiusCm) // flat
		assertEquals(-VrEnvironmentConfig.SCREEN_HEIGHT_OFFSET_CM_MAX, c.screenHeightOffsetCm)
		assertEquals(100, c.glowPercent)
		assertEquals(0, c.roomLightPercent)
	}

	@Test
	fun smallPositiveCurveRadiusIsRaisedNotFlattened()
	{
		assertEquals(VrEnvironmentConfig.SCREEN_CURVE_RADIUS_CM_MIN, VrEnvironmentConfig(screenCurveRadiusCm = 1).clamped().screenCurveRadiusCm)
	}

	@Test
	fun offeredOnlyOnTheOculusGo()
	{
		assertTrue(VrEnvironmentSupport.offered("pacific"))
		assertFalse(VrEnvironmentSupport.offered("b0s"))   // SM-S908B
		assertFalse(VrEnvironmentSupport.offered(null))
	}
}
