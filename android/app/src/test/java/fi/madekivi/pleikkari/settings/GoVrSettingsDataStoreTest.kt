// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.settings

import fi.madekivi.pleikkari.common.Preferences
import org.junit.Assert.assertEquals
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config

/**
 * PLE-753: the Go-only switches are added to the settings screen in code and read and written
 * through [DataStore]; a key missing there shows a switch that never stores a change, as
 * PLE-715's late start and PLE-755's warm-up did.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [25])
class GoVrSettingsDataStoreTest
{
	@Test
	fun everyGoVrSwitchStoresAChange()
	{
		val preferences = Preferences(RuntimeEnvironment.getApplication())
		val store = DataStore(preferences)
		val switches = listOf<Pair<String, () -> Boolean>>(
			preferences.goVrEnabledKey to { preferences.goVrEnabled },
			preferences.goVrUiKey to { preferences.goVrUi },
			preferences.goVrMatch60HzKey to { preferences.goVrMatch60Hz },
			preferences.goVrRoomHighGpuKey to { preferences.goVrRoomHighGpu },
			preferences.goVrFrameListenerThreadKey to { preferences.goVrFrameListenerThread },
			preferences.goVrLateStartKey to { preferences.goVrLateStart },
			preferences.goVrHoldDrainKey to { preferences.goVrHoldDrain },
			preferences.goVrWarmUpKey to { preferences.goVrWarmUp },
			preferences.goVrFlushEyesKey to { preferences.goVrFlushEyes },
		)
		for((key, read) in switches)
		{
			val before = read()
			store.putBoolean(key, !before)
			assertEquals(key, !before, read())
			assertEquals(key, !before, store.getBoolean(key, before))
			store.putBoolean(key, before)
			assertEquals(key, before, read())
		}
	}

	@Test
	fun roomMsaaStoresAChoiceAndIgnoresAnythingElse()
	{
		val preferences = Preferences(RuntimeEnvironment.getApplication())
		val store = DataStore(preferences)
		val key = preferences.goVrRoomMsaaKey
		assertEquals(4, preferences.goVrRoomMsaa)
		assertEquals("4", store.getString(key, null))
		store.putString(key, "2")
		assertEquals(2, preferences.goVrRoomMsaa)
		assertEquals("2", store.getString(key, null))
		store.putString(key, "1")
		assertEquals(1, preferences.goVrRoomMsaa)
		store.putString(key, "3")
		assertEquals(1, preferences.goVrRoomMsaa)
		// A stored value that is not on offer reads as the default.
		preferences.sharedPreferences.edit().putString(key, "8").commit()
		assertEquals(4, preferences.goVrRoomMsaa)
	}
}
