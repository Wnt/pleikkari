// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.session

import android.app.Activity
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.graphics.Bitmap
import android.os.Handler
import android.os.Looper
import android.util.Log
import androidx.core.content.ContextCompat
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Debug builds only. Saves the current decoded video frame, at the stream's own resolution and without
 * the app's overlays (touch controls, menus), as a PNG an agent can OCR; `adb screencap` only gives the
 * video at its on-screen size:
 *
 *     adb shell am broadcast --receiver-foreground -a fi.madekivi.pleikkari.GRAB -p fi.madekivi.pleikkari
 *
 * The PNG lands at `getExternalFilesDir("grab")/latest.png` (`--es name foo.png` picks another plain
 * `.png` name), written through a temp file and a rename. The result is `result=-1` with
 * `data="ok 1280x720 /sdcard/Android/data/.../grab/latest.png in 41 ms"`, or `result=1` with
 * `data="error: ..."`, and a bare `result=0` when no stream is open to take it.
 *
 * The sender must hold android.permission.DUMP, which adb's shell has and an app cannot get.
 * [capture] does the screen-side work (PixelCopy on the SurfaceView); it runs on the main thread and
 * calls back once, with a bitmap or an error text. PNG compression and the write run on a worker thread.
 */
class FrameGrabReceiver(
	private val context: Context,
	/** Why the stream cannot be grabbed right now, or null while it is connected. */
	private val notStreaming: () -> String?,
	private val capture: (onDone: (Bitmap?, String?) -> Unit) -> Unit)
{
	companion object
	{
		const val ACTION = "fi.madekivi.pleikkari.GRAB"
		const val EXTRA_NAME = "name"
		const val DIR = "grab"
		/** Under the 10 s a foreground broadcast may take. */
		private const val REPLY_WAIT_MS = 8_000L
		private const val RESULT_ERROR = 1
		private const val SENDER_PERMISSION = "android.permission.DUMP"
		private const val TAG = "FrameGrab"
	}

	private val mainHandler = Handler(Looper.getMainLooper())
	private var worker: java.util.concurrent.ExecutorService? = null

	private val receiver = object: BroadcastReceiver()
	{
		override fun onReceive(context: Context, intent: Intent)
		{
			val pending = goAsync()
			val replied = AtomicBoolean()
			fun reply(code: Int, data: String)
			{
				if(!replied.compareAndSet(false, true))
					return
				Log.i(TAG, data)
				pending.setResult(code, data, null)
				pending.finish()
			}
			val worker = worker ?: return reply(RESULT_ERROR, "error: receiver stopped")
			notStreaming()?.let { return reply(RESULT_ERROR, "error: not streaming ($it)") }
			val name = FrameGrabFile.sanitizeName(intent.getStringExtra(EXTRA_NAME))
				?: return reply(RESULT_ERROR, "error: --es $EXTRA_NAME must be a plain file name ending in .png")
			val dir = this@FrameGrabReceiver.context.getExternalFilesDir(DIR)
				?: return reply(RESULT_ERROR, "error: no external files dir")
			val start = System.nanoTime()
			mainHandler.postDelayed({ reply(RESULT_ERROR, "error: frame grab timed out") }, REPLY_WAIT_MS)
			try
			{
				capture { bitmap, error ->
					if(bitmap == null)
						return@capture reply(RESULT_ERROR, "error: ${error ?: "no frame"}")
					try
					{
						worker.execute {
							try
							{
								val file = FrameGrabFile.writeAtomic(dir, name) {
									bitmap.compress(Bitmap.CompressFormat.PNG, 100, it)
								}
								val ms = (System.nanoTime() - start) / 1_000_000
								reply(Activity.RESULT_OK, "ok ${bitmap.width}x${bitmap.height} ${file.absolutePath} in $ms ms")
							}
							catch(e: Exception)
							{
								reply(RESULT_ERROR, "error: write failed: ${e.message}")
							}
							finally
							{
								bitmap.recycle()
							}
						}
					}
					catch(e: java.util.concurrent.RejectedExecutionException)
					{
						reply(RESULT_ERROR, "error: receiver stopped")
					}
				}
			}
			catch(e: Exception)
			{
				reply(RESULT_ERROR, "error: ${e.message ?: e::class.java.simpleName}")
			}
		}
	}

	fun register()
	{
		if(worker != null)
			return
		worker = Executors.newSingleThreadExecutor { Thread(it, "FrameGrab") }
		ContextCompat.registerReceiver(context, receiver, IntentFilter(ACTION), SENDER_PERMISSION, null,
			ContextCompat.RECEIVER_EXPORTED)
		Log.i(TAG, "listening for $ACTION")
	}

	fun unregister()
	{
		val worker = worker ?: return
		this.worker = null
		context.unregisterReceiver(receiver)
		worker.shutdown()
	}
}
