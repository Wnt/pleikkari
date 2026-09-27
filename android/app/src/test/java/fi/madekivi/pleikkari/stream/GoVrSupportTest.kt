package fi.madekivi.pleikkari.stream

import android.content.ComponentName
import android.content.Intent
import android.content.pm.PackageManager
import org.robolectric.RuntimeEnvironment
import fi.madekivi.pleikkari.common.MacAddress
import fi.madekivi.pleikkari.common.Preferences
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.w3c.dom.Element
import java.io.File
import javax.xml.parsers.DocumentBuilderFactory

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [25])
class GoVrSupportTest {
    @Test fun onlyArm64GoWithSdkCanLoadRuntime() {
        assertTrue(GoVrSupport.eligible("pacific", 25, true, true))
        assertFalse(GoVrSupport.eligible("b0s", 35, true, true))
        assertFalse(GoVrSupport.eligible("pacific", 24, true, true))
        assertFalse(GoVrSupport.eligible("pacific", 25, false, true))
        assertFalse(GoVrSupport.eligible("pacific", 25, true, false))
    }

    @Test fun freshInstallAndImportedOptInBothUseNormalStreamOnPhone() {
        val context = RuntimeEnvironment.getApplication()
        val preferences = Preferences(context)
        assertFalse(preferences.goVrEnabled)
        assertEquals(StreamActivity::class.java.name, GoVrSupport.streamIntent(context).component?.className)
        preferences.goVrEnabled = true
        assertEquals(StreamActivity::class.java.name, GoVrSupport.streamIntent(context).component?.className)
        preferences.goVrEnabled = false
    }

    @Test fun libraryEntryIsNeverTouchedOnAPhone() {
        val context = RuntimeEnvironment.getApplication()
        GoVrSupport.syncLibraryEntry(context, true)
        assertEquals(PackageManager.COMPONENT_ENABLED_STATE_DEFAULT,
            context.packageManager.getComponentEnabledSetting(ComponentName(context, GoVrSupport.LIBRARY_ENTRY)))
    }

    @Test fun onlyTheAliasCountsAsALibraryLaunch() {
        val context = RuntimeEnvironment.getApplication()
        assertTrue(GoVrSupport.isLibraryEntry(Intent().setClassName(context, GoVrSupport.LIBRARY_ENTRY)))
        assertFalse(GoVrSupport.isLibraryEntry(Intent().setClassName(context, GoVrSupport.ACTIVITY)))
        assertFalse(GoVrSupport.isLibraryEntry(Intent(Intent.ACTION_MAIN)))
        assertFalse(GoVrSupport.isLibraryEntry(null))
    }

    @Test fun panelIntentIsTheLibrarysOculusTvLaunchOfMainActivity() {
        val intent = GoVrSupport.panelIntent(RuntimeEnvironment.getApplication())
        assertEquals("com.oculus.vrshell/com.oculus.vrshell.MainActivity", intent.component?.flattenToString())
        assertEquals("apk://com.oculus.tv", intent.dataString)
        assertEquals("fi.madekivi.pleikkari/.main.MainActivity", intent.getStringExtra("uri"))
        assertTrue(intent.flags and Intent.FLAG_ACTIVITY_NEW_TASK != 0)
    }

    /** PLE-690: the vr overlay's Library entry must ship disabled, or a phone's getLaunchIntentForPackage finds it. */
    @Test fun vrOverlayDeclaresTheLibraryEntryDisabledWithTheVrLaunchCategories() {
        val android = "http://schemas.android.com/apk/res/android"
        val document = DocumentBuilderFactory.newInstance().apply { isNamespaceAware = true }
            .newDocumentBuilder().parse(File("src/vr/AndroidManifest.xml"))
        val aliases = document.getElementsByTagName("activity-alias")
        assertEquals(1, aliases.length)
        val alias = aliases.item(0) as Element
        assertEquals(GoVrSupport.LIBRARY_ENTRY, "fi.madekivi.pleikkari" + alias.getAttributeNS(android, "name"))
        assertEquals(GoVrSupport.ACTIVITY, "fi.madekivi.pleikkari" + alias.getAttributeNS(android, "targetActivity"))
        assertEquals("false", alias.getAttributeNS(android, "enabled"))
        assertEquals("true", alias.getAttributeNS(android, "exported"))
        val names = { tag: String ->
            val nodes = alias.getElementsByTagName(tag)
            (0 until nodes.length).map { (nodes.item(it) as Element).getAttributeNS(android, "name") }.toSet()
        }
        assertEquals(setOf("android.intent.action.MAIN"), names("action"))
        // INFO outranks MainActivity's LAUNCHER in getLaunchIntentForPackage; VR is vrshell's isVrApp() test.
        assertEquals(setOf("android.intent.category.INFO", "com.oculus.intent.category.VR"), names("category"))
        assertFalse(names("category").contains("android.intent.category.LAUNCHER"))
    }

    @Test fun lastUsedConsoleRoundTrips() {
        val preferences = Preferences(RuntimeEnvironment.getApplication())
        assertNull(preferences.lastConsoleMac)
        preferences.lastConsoleMac = MacAddress(0x1234L)
        assertEquals(MacAddress(0x1234L), preferences.lastConsoleMac)
        preferences.lastConsoleMac = null
        assertNull(preferences.lastConsoleMac)
    }
}
