// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.session

import java.io.File
import java.io.FileOutputStream
import java.io.OutputStream

/** File naming and the atomic write behind [FrameGrabReceiver]; plain JVM so a unit test covers it. */
object FrameGrabFile
{
	const val DEFAULT_NAME = "latest.png"
	private val SAFE_NAME = Regex("[A-Za-z0-9._-]{1,64}")

	/** The file name a broadcast asked for, or [DEFAULT_NAME]; null when it is not a plain `.png` name. */
	fun sanitizeName(requested: String?): String?
	{
		if(requested.isNullOrEmpty())
			return DEFAULT_NAME
		if(!SAFE_NAME.matches(requested) || requested.startsWith(".") || !requested.endsWith(".png"))
			return null
		return requested
	}

	/**
	 * Writes [name] in [dir] through a temp file and a rename, so a reader polling the path never sees a
	 * half-written PNG. Returns the final file.
	 */
	fun writeAtomic(dir: File, name: String, write: (OutputStream) -> Unit): File
	{
		if(!dir.isDirectory && !dir.mkdirs())
			throw java.io.IOException("cannot create $dir")
		val target = File(dir, name)
		val temp = File(dir, "$name.tmp")
		try
		{
			FileOutputStream(temp).use { write(it) }
			if(!temp.renameTo(target))
				throw java.io.IOException("cannot rename $temp to $target")
		}
		finally
		{
			temp.delete()
		}
		return target
	}
}
