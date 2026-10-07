// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.session

import fi.madekivi.pleikkari.lib.ControllerState
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

/**
 * PLE-869: plays [PadScript]s into [apply] on one "PadScript" thread, one script after another in
 * the order they were submitted. Every press ends with [apply] of a released pad, including a
 * cancelled one, so a script never leaves a button down on the console.
 */
class PadScriptRunner(private val apply: (ControllerState) -> Unit)
{
	data class Outcome(val presses: Int, val elapsedMs: Long, val cancelled: Boolean)

	private val lock = Object()
	/** Bumped by [cancel]; a script started under an older value stops at its next step. */
	private var generation = 0
	private val executor: ExecutorService = Executors.newSingleThreadExecutor { runnable ->
		Thread(runnable, "PadScript").apply { isDaemon = true }
	}

	/** Queues [script] behind any running one; [done] runs on the runner thread when it ends. */
	fun submit(script: PadScript, done: (Outcome) -> Unit)
	{
		val submitted = synchronized(lock) { generation }
		executor.execute { done(play(script, submitted)) }
	}

	/** Stops the running script and drops the queued ones; the pad is released. */
	fun cancel() = synchronized(lock)
	{
		generation++
		lock.notifyAll()
	}

	fun shutdown()
	{
		cancel()
		executor.shutdown()
	}

	private fun play(script: PadScript, submitted: Int): Outcome
	{
		val start = System.nanoTime()
		var presses = 0
		fun outcome(cancelled: Boolean) = Outcome(presses, (System.nanoTime() - start) / 1_000_000, cancelled)
		for(step in script.steps)
		{
			when(step)
			{
				is PadStep.Wait -> if(!pause(step.ms, submitted)) return outcome(true)
				is PadStep.Press -> {
					if(!live(submitted))
						return outcome(true)
					apply(step.state)
					val held = pause(step.holdMs, submitted)
					apply(ControllerState())
					presses++
					if(!held || !pause(step.gapMs, submitted))
						return outcome(true)
				}
			}
		}
		return outcome(false)
	}

	private fun live(submitted: Int) = synchronized(lock) { generation == submitted }

	/** Sleeps [ms]; false if [cancel] ran before or during it. */
	private fun pause(ms: Long, submitted: Int): Boolean
	{
		val end = System.nanoTime() + ms * 1_000_000
		synchronized(lock)
		{
			while(generation == submitted)
			{
				val leftNs = end - System.nanoTime()
				if(leftNs <= 0)
					return true
				lock.wait(leftNs / 1_000_000, (leftNs % 1_000_000).toInt())
			}
			return false
		}
	}
}
