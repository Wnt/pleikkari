// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari

import android.app.Application
import com.google.android.material.color.DynamicColors
import fi.madekivi.pleikkari.stream.GoVrSupport

class ChiakiApplication : Application()
{
	override fun onCreate()
	{
		super.onCreate()
		DynamicColors.applyToActivitiesIfAvailable(this)
		// PLE-690: on an Oculus Go only, point the Library's launch at the VR cinema or at Oculus TV.
		GoVrSupport.syncLibraryEntry(this)
	}
}
