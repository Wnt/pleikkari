// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.stream.vrui

import fi.madekivi.pleikkari.common.Preferences
import fi.madekivi.pleikkari.settings.DataStore
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import java.io.File

/**
 * PLE-844: the Go Settings sheet's latency experiments use the same keys as [Preferences], store
 * through Settings' [DataStore], default off, and are the flag gate's allowlisted A/B switches.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [25])
class VrSettingsKeysTest
{
	private val preferences = Preferences(RuntimeEnvironment.getApplication())

	@Test
	fun everyExperimentIsAPreferencesKey()
	{
		assertEquals(listOf(
			preferences.goVrLateStartKey,
			preferences.goVrLatchOnSignalKey,
			preferences.goVrInputThreadKey,
			preferences.goVrInputThreadFlushKey,
			preferences.goVrFrameListenerThreadKey,
			preferences.goVrMatch60HzKey,
			preferences.decoderQcomVtLowLatencyKey,
			preferences.goVrWarmUpKey,
			preferences.goVrHoldDrainKey,
			preferences.goVrFlushEyesKey,
			preferences.goVrRoomHighGpuKey), VrSettings.EXPERIMENTS)
		assertEquals(preferences.goVrRoomMsaaKey, VrSettings.ROOM_MSAA_KEY)
		assertTrue(VrSettings.ROOM_MSAA_CHOICES.all { it in Preferences.goVrRoomMsaaChoices })
		assertTrue(Preferences.GO_VR_ROOM_MSAA_DEFAULT in VrSettings.ROOM_MSAA_CHOICES)
	}

	@Test
	fun everyExperimentDefaultsOffAndStores()
	{
		val store = DataStore(preferences)
		for(key in VrSettings.EXPERIMENTS)
		{
			assertFalse(key, store.getBoolean(key, true))
			store.putBoolean(key, true)
			assertTrue(key, preferences.sharedPreferences.getBoolean(key, false))
			assertTrue(key, store.getBoolean(key, false))
		}
	}

	@Test
	fun everyExperimentIsAnAllowlistedDefaultOffPref()
	{
		val allowlisted = File("../flag-gate-allowlist.txt").readLines()
			.map { it.split("|").map(String::trim) }
			.filter { it.size >= 2 && it[0] == "default-off-pref" }
			.map { it[1] }
		for(key in VrSettings.EXPERIMENTS)
			assertTrue("$key is not in android/flag-gate-allowlist.txt", key in allowlisted)
	}
}
