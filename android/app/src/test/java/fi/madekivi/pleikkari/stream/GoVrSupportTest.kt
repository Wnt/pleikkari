package fi.madekivi.pleikkari.stream

import org.robolectric.RuntimeEnvironment
import fi.madekivi.pleikkari.common.Preferences
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

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
}
