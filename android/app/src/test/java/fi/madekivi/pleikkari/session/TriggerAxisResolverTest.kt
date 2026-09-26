// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.session

import android.view.MotionEvent
import org.junit.Assert.assertEquals
import org.junit.Test

class TriggerAxisResolverTest
{
	@Test
	fun keepsTriggerAxesWhenTheirRangesAreNonZero()
	{
		val resolver = resolverWith(
			MotionEvent.AXIS_LTRIGGER to 1.0f,
			MotionEvent.AXIS_RTRIGGER to 1.0f,
			MotionEvent.AXIS_BRAKE to 1.0f,
			MotionEvent.AXIS_GAS to 1.0f
		)

		assertEquals(
			TriggerAxes(MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_RTRIGGER),
			resolver.axesFor(7)
		)
	}

	@Test
	fun usesBrakeAndGasWhenTriggerRangesAreMissingOrZero()
	{
		val resolver = resolverWith(
			MotionEvent.AXIS_RTRIGGER to 0.0f,
			MotionEvent.AXIS_BRAKE to 1.0f,
			MotionEvent.AXIS_GAS to 1.0f
		)

		assertEquals(
			TriggerAxes(MotionEvent.AXIS_BRAKE, MotionEvent.AXIS_GAS),
			resolver.axesFor(8)
		)
	}

	@Test
	fun keepsTriggerAxisWhenNeitherRangeIsUsable()
	{
		val resolver = resolverWith(
			MotionEvent.AXIS_LTRIGGER to 0.0f,
			MotionEvent.AXIS_BRAKE to 0.0f
		)

		assertEquals(MotionEvent.AXIS_LTRIGGER, resolver.axesFor(9).l2)
		assertEquals(MotionEvent.AXIS_RTRIGGER, resolver.axesFor(9).r2)
	}

	@Test
	fun readsRangesOnlyOncePerDeviceId()
	{
		var reads = 0
		val resolver = TriggerAxisResolver({
			reads++
			mapOf(MotionEvent.AXIS_BRAKE to 1.0f, MotionEvent.AXIS_GAS to 1.0f)
		})

		resolver.axesFor(10)
		resolver.axesFor(10)
		resolver.axesFor(11)

		assertEquals(2, reads)
	}

	@Test
	fun usesBrakeAndGasWithoutTriggerRangesRegardlessOfPreference()
	{
		for(preference in listOf(false, true))
		{
			val resolver = TriggerAxisResolver(
				{ mapOf(MotionEvent.AXIS_BRAKE to 1.0f, MotionEvent.AXIS_GAS to 1.0f) },
				fallbackForReportedRanges = preference
			)
			assertEquals(
				TriggerAxes(MotionEvent.AXIS_BRAKE, MotionEvent.AXIS_GAS),
				resolver.axesFor(12)
			)
		}
	}

	@Test
	fun keepsReportedTriggerAxesWhenPreferenceIsOff()
	{
		val resolver = TriggerAxisResolver(
			{
				mapOf(
					MotionEvent.AXIS_LTRIGGER to 0.0f,
					MotionEvent.AXIS_RTRIGGER to 1.0f,
					MotionEvent.AXIS_BRAKE to 1.0f,
					MotionEvent.AXIS_GAS to 1.0f
				)
			},
			fallbackForReportedRanges = false
		)

		assertEquals(
			TriggerAxes(MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_RTRIGGER),
			resolver.axesFor(13)
		)
	}

	@Test
	fun fallsBackFromZeroReportedRangeWhenPreferenceIsOn()
	{
		val resolver = TriggerAxisResolver(
			{
				mapOf(
					MotionEvent.AXIS_LTRIGGER to 0.0f,
					MotionEvent.AXIS_RTRIGGER to 1.0f,
					MotionEvent.AXIS_BRAKE to 1.0f,
					MotionEvent.AXIS_GAS to 1.0f
				)
			},
			fallbackForReportedRanges = true
		)

		assertEquals(
			TriggerAxes(MotionEvent.AXIS_BRAKE, MotionEvent.AXIS_RTRIGGER),
			resolver.axesFor(14)
		)
	}

	private fun resolverWith(vararg ranges: Pair<Int, Float>) =
		TriggerAxisResolver({ ranges.toMap() })
}
