// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.settings

import fi.madekivi.pleikkari.common.Preferences
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
 * Settings adds the Go switches in code, and every one of them stores through [DataStore]. A key the
 * DataStore does not route reads its default and drops the change, so the switch never sticks: PLE-801
 * found `stream_go_vr_late_start` and `stream_go_vr_warm_up` that way.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [25])
class CodeSwitchDataStoreTest
{
	@Test
	fun everySwitchAddedInCodeStoresItsChange()
	{
		val source = File("src/main/java/fi/madekivi/pleikkari/settings/SettingsFragment.kt").readText()
		val properties = Regex("""SwitchPreferenceCompat\(context\)\.apply \{\s*key = preferences\.(\w+Key)""")
			.findAll(source).map { it.groupValues[1] }.toList()
		assertTrue("no code-added switch found in SettingsFragment.kt", "goVrLatchOnSignalKey" in properties)
		val context = RuntimeEnvironment.getApplication()
		val preferences = Preferences(context)
		val store = DataStore(preferences)
		val shared = preferences.sharedPreferences
		for(property in properties)
		{
			val key = Preferences::class.java.getMethod("get" + property.replaceFirstChar { it.uppercase() })
				.invoke(preferences) as String
			store.putBoolean(key, true)
			assertTrue("$key: the switch's change is not stored", shared.getBoolean(key, false))
			assertTrue("$key: the switch does not read the stored value", store.getBoolean(key, false))
			store.putBoolean(key, false)
			assertFalse("$key: the switch's change is not stored", shared.getBoolean(key, true))
			assertFalse("$key: the switch does not read the stored value", store.getBoolean(key, true))
		}
		assertEquals(properties.size, properties.toSet().size)
	}
}
