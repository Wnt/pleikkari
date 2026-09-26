// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
package fi.madekivi.pleikkari.stream

import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.Process
import android.util.Log
import fi.madekivi.pleikkari.BuildConfig
import fi.madekivi.pleikkari.common.Preferences

/** Keep optional classes and the legacy runtime out of the phone's class-loading path. */
object GoVrSupport {
    const val ACTIVITY = "fi.madekivi.pleikkari.stream.StreamVrActivity"

    internal fun eligible(device: String, sdk: Int, arm64Process: Boolean, built: Boolean) =
        device == "pacific" && sdk >= 25 && arm64Process && built

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

    fun available(): Boolean = eligible(Build.DEVICE, Build.VERSION.SDK_INT,
        Process.is64Bit() && Build.SUPPORTED_ABIS.contains("arm64-v8a"), BuildConfig.GO_VR_BUILT) && loadable

    fun streamIntent(context: Context): Intent =
        if(Preferences(context).goVrEnabled && available()) Intent().setClassName(context, ACTIVITY)
        else Intent(context, StreamActivity::class.java)
}
