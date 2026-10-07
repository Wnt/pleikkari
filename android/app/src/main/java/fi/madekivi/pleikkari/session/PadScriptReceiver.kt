// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.session

import android.app.Activity
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Handler
import android.os.Looper
import android.util.Log
import androidx.core.content.ContextCompat
import fi.madekivi.pleikkari.lib.ControllerState
import java.util.concurrent.atomic.AtomicBoolean

/**
 * PLE-869: debug builds only. Presses DualSense buttons on the streamed console from adb, so a script
 * can walk PS5 and game menus; `adb shell input` cannot (the D-pad only arrives as a pad's HAT motion,
 * and a tap in a landscape stream lands on the on-screen gamepad, PLE-453):
 *
 *     adb shell am broadcast -a fi.madekivi.pleikkari.PAD --es seq "'down*3 cross wait:600 circle'"
 *
 * The [PadScript] grammar covers taps, repeats, holds, chords, sticks and triggers; `--el hold_ms` and
 * `--el gap_ms` change the defaults, `--ez cancel true` stops what is playing and releases the pad.
 * The broadcast completes when the sequence has been sent, or after [REPLY_WAIT_MS] with the rest
 * still playing, and its result says how it went: `result=-1` with `data="ok ..."` or `data="running
 * ..."`, `result=1` with `data="error: ..."`, and a bare `result=0` when no stream is open to take it.
 *
 * It reaches the console as controller input. The sender must hold android.permission.DUMP, which
 * adb's shell has and an app cannot get, so nothing else on the phone can drive the console.
 */
class PadScriptReceiver(
	private val input: () -> StreamInput?,
	/** Why the stream cannot take input right now, or null while it is connected. */
	private val notStreaming: () -> String?)
{
	companion object
	{
		const val ACTION = "fi.madekivi.pleikkari.PAD"
		const val EXTRA_SEQ = "seq"
		const val EXTRA_HOLD_MS = "hold_ms"
		const val EXTRA_GAP_MS = "gap_ms"
		const val EXTRA_CANCEL = "cancel"
		/** Under the 10 s a foreground broadcast may take; a longer sequence answers "running". */
		private const val REPLY_WAIT_MS = 8_000L
		private const val RESULT_ERROR = 1
		private const val SENDER_PERMISSION = "android.permission.DUMP"
		private const val TAG = "PadScript"
	}

	private val mainHandler = Handler(Looper.getMainLooper())
	private var runner: PadScriptRunner? = null

	private val receiver = object: BroadcastReceiver()
	{
		override fun onReceive(context: Context, intent: Intent)
		{
			val pending = goAsync()
			fun reply(code: Int, data: String)
			{
				Log.i(TAG, data)
				pending.setResult(code, data, null)
				pending.finish()
			}
			val runner = runner ?: return reply(RESULT_ERROR, "error: receiver stopped")
			if(intent.getBooleanExtra(EXTRA_CANCEL, false))
			{
				runner.cancel()
				return reply(Activity.RESULT_OK, "ok cancelled")
			}
			val seq = intent.getStringExtra(EXTRA_SEQ) ?: return reply(RESULT_ERROR, "error: no --es $EXTRA_SEQ")
			notStreaming()?.let { return reply(RESULT_ERROR, "error: not streaming ($it)") }
			val script = try
			{
				PadScript.parse(seq,
					intent.getLongExtra(EXTRA_HOLD_MS, PadScript.DEFAULT_HOLD_MS),
					intent.getLongExtra(EXTRA_GAP_MS, PadScript.DEFAULT_GAP_MS))
			}
			catch(e: PadScriptException)
			{
				return reply(RESULT_ERROR, "error: ${e.message}")
			}
			Log.i(TAG, "play '$seq': ${script.presses} presses, ${script.durationMs} ms")
			val replied = AtomicBoolean()
			val timeout = Runnable {
				if(replied.compareAndSet(false, true))
					reply(Activity.RESULT_OK, "running ${script.presses} presses, ${script.durationMs} ms; sent at most ${REPLY_WAIT_MS} ms of it")
			}
			mainHandler.postDelayed(timeout, REPLY_WAIT_MS)
			runner.submit(script) { outcome ->
				val text = if(outcome.cancelled) "cancelled" else "ok"
				val data = "$text ${outcome.presses}/${script.presses} presses in ${outcome.elapsedMs} ms"
				if(replied.compareAndSet(false, true))
				{
					mainHandler.removeCallbacks(timeout)
					reply(if(outcome.cancelled) RESULT_ERROR else Activity.RESULT_OK, data)
				}
				else
					Log.i(TAG, data)
			}
		}
	}

	fun register(context: Context)
	{
		if(runner != null)
			return
		runner = PadScriptRunner { state -> input()?.scriptedControllerState = state }
		ContextCompat.registerReceiver(context, receiver, IntentFilter(ACTION), SENDER_PERMISSION, null,
			ContextCompat.RECEIVER_EXPORTED)
		Log.i(TAG, "listening for $ACTION")
	}

	/** Stops any script; the pad is released before the session sees its next state. */
	fun unregister(context: Context)
	{
		val runner = runner ?: return
		this.runner = null
		context.unregisterReceiver(receiver)
		runner.shutdown()
		input()?.scriptedControllerState = ControllerState()
	}
}
