// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
package fi.madekivi.pleikkari.stream

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Process
import android.util.Log
import fi.madekivi.pleikkari.BuildConfig
import fi.madekivi.pleikkari.common.Preferences
import fi.madekivi.pleikkari.main.MainActivity

/** Keep optional classes and the legacy runtime out of the phone's class-loading path. */
object GoVrSupport {
    const val ACTIVITY = "fi.madekivi.pleikkari.stream.StreamVrActivity"
    /**
     * PLE-690: the vr overlay's alias of [ACTIVITY] with MAIN + INFO + com.oculus.intent.category.VR.
     * vrshell's PackageUtil.isVrApp() counts that category as a VR app, and its sendLaunchIntent()
     * starts getLaunchIntentForPackage(), which prefers MAIN/INFO over the LAUNCHER MainActivity.
     */
    const val LIBRARY_ENTRY = "fi.madekivi.pleikkari.stream.GoVrLibraryEntry"
    private const val VRSHELL = "com.oculus.vrshell"
    private const val TAG = "GoVrSupport"

    internal fun eligible(device: String, sdk: Int, arm64Process: Boolean, built: Boolean) =
        device == "pacific" && sdk >= 25 && arm64Process && built

    private fun eligibleDevice() = eligible(Build.DEVICE, Build.VERSION.SDK_INT,
        Process.is64Bit() && Build.SUPPORTED_ABIS.contains("arm64-v8a"), BuildConfig.GO_VR_BUILT)

    private val loadable: Boolean by lazy {
        try {
            System.loadLibrary("vrapi")
            System.loadLibrary("pleikkari-vr-environment") // PLE-603: linked by pleikkari-vr
            System.loadLibrary("pleikkari-vr")
            true
        } catch(error: LinkageError) {
            Log.w("GoVrSupport", "Go cinema libraries unavailable", error)
            false
        }
    }

    fun available(): Boolean = eligibleDevice() && loadable

    fun streamIntent(context: Context): Intent =
        if(Preferences(context).goVrEnabled && available()) Intent().setClassName(context, ACTIVITY)
        else Intent(context, StreamActivity::class.java)

    /** PLE-690: started through the Library entry, whose exported alias carries no extras worth trusting. */
    fun isLibraryEntry(intent: Intent?) = intent?.component?.className == LIBRARY_ENTRY

    /**
     * PLE-690: the alias ships disabled, so a phone never sees it. On the Go it follows the native
     * cinema setting: on, the Library opens the VR cinema; off, the Library opens MainActivity in
     * Oculus TV as it did before. The package manager keeps the state across `install -r`.
     */
    fun syncLibraryEntry(context: Context, enabled: Boolean? = null) {
        if(!eligibleDevice())
            return
        val on = enabled ?: Preferences(context).goVrEnabled
        val state = if(on) PackageManager.COMPONENT_ENABLED_STATE_ENABLED else PackageManager.COMPONENT_ENABLED_STATE_DEFAULT
        val component = ComponentName(context, LIBRARY_ENTRY)
        try {
            if(context.packageManager.getComponentEnabledSetting(component) == state)
                return
            context.packageManager.setComponentEnabledSetting(component, state, PackageManager.DONT_KILL_APP)
            Log.i(TAG, if(on) "Library entry on: the Go Library opens the VR cinema"
                else "Library entry off: the Go Library opens Oculus TV")
        } catch(error: IllegalArgumentException) {
            Log.w(TAG, "No Go Library entry in this build", error)
        }
    }

    /**
     * PLE-690: what the Go Library sends for a 2D app (PLE-600's `go.sh launch`): vrshell asks
     * Oculus TV to host MainActivity on its virtual screen, for linking a console and Settings.
     */
    fun panelIntent(context: Context): Intent = Intent()
        .setClassName(VRSHELL, "$VRSHELL.MainActivity")
        .setData(Uri.parse("apk://com.oculus.tv"))
        .putExtra("uri", ComponentName(context, MainActivity::class.java).flattenToShortString())
        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
}
