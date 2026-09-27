// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.settings

import fi.madekivi.pleikkari.R
import fi.madekivi.pleikkari.common.Preferences
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import java.io.File
import javax.xml.parsers.DocumentBuilderFactory

/**
 * PLE-829: [CodeSwitchDataStoreTest]'s check for the switches in preferences.xml. Settings stores every
 * one of them through [DataStore], so a key it does not route never keeps a change.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [25])
class XmlSwitchDataStoreTest
{
	@Test
	fun everySwitchInThePreferenceXmlStoresItsChange()
	{
		val document = DocumentBuilderFactory.newInstance().apply { isNamespaceAware = true }
			.newDocumentBuilder().parse(File("src/main/res/xml/preferences.xml"))
		val switches = document.documentElement.getElementsByTagName("SwitchPreference")
		val context = RuntimeEnvironment.getApplication()
		val keys = (0 until switches.length).map { index ->
			val key = switches.item(index).attributes.getNamedItemNS("http://schemas.android.com/apk/res-auto", "key").nodeValue
			if(key.startsWith("@string/"))
				context.getString(R.string::class.java.getField(key.removePrefix("@string/")).getInt(null))
			else
				key
		}
		assertTrue("PLE-829's switch is not in preferences.xml", "stream_pad_input_thread" in keys)
		val preferences = Preferences(context)
		val store = DataStore(preferences)
		val shared = preferences.sharedPreferences
		for(key in keys)
		{
			store.putBoolean(key, true)
			assertTrue("$key: the switch's change is not stored", shared.getBoolean(key, false))
			assertTrue("$key: the switch does not read the stored value", store.getBoolean(key, false))
			store.putBoolean(key, false)
			assertFalse("$key: the switch's change is not stored", shared.getBoolean(key, true))
			assertFalse("$key: the switch does not read the stored value", store.getBoolean(key, true))
		}
	}
}
