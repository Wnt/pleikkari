// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.session

import android.content.Context
import android.graphics.SurfaceTexture
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.view.*
import androidx.lifecycle.LiveData
import androidx.lifecycle.MutableLiveData
import fi.madekivi.pleikkari.common.LogManager
import fi.madekivi.pleikkari.lib.*
import fi.madekivi.pleikkari.stream.ConnectionModeClassifier
import fi.madekivi.pleikkari.stream.ConnectionModeSnapshot
import fi.madekivi.pleikkari.stream.displayTimingChanged
import fi.madekivi.pleikkari.stream.useFrameIndexVideoTimestamps

sealed class StreamState
object StreamStateIdle: StreamState()
object StreamStateConnecting: StreamState()
/**
 * Connecting, on a console that has just been linked and is still settling (PLE-335). A separate
 * state only so the screen can say the truth — the console *is* linked — instead of a bare spinner
 * or, as before, a "Session has quit" dialog.
 */
object StreamStateLinkedStarting: StreamState()

object StreamStateConnected: StreamState()
data class StreamStateCreateError(val error: CreateError): StreamState()
data class StreamStateRemoteError(val message: String): StreamState()
data class StreamStateQuit(val reason: QuitReason, val reasonString: String?): StreamState()
data class StreamStateLoginPinRequest(val pinIncorrect: Boolean): StreamState()

private const val HANDOFF_TAG = "StreamHandoff"

class StreamSession(private val context: Context, val connectInfo: ConnectInfo, val logManager: LogManager, val logVerbose: Boolean, val realVideoTimestamps: Boolean,
		val decoderInputThread: Boolean,
		val input: StreamInput, private val externallyManaged: Boolean = false,
		/** This stream was started straight off a successful registration — see [SessionHandoffRetry]. */
		justLinked: Boolean = false)
{
	/** Volatile: PLE-802's GoPadInput thread sends controller states to it (the PSN path already set it off main). */
	@Volatile var session: Session? = null
		private set

	private val handoffRetry = SessionHandoffRetry(clock = SystemClock::elapsedRealtime)
		.also { if(justLinked && !externallyManaged) it.arm() }
	private val handoffHandler = Handler(Looper.getMainLooper())

	private val _state = MutableLiveData<StreamState>(StreamStateIdle)
	val state: LiveData<StreamState> get() = _state
	private val _rumbleState = MutableLiveData<RumbleEvent>(RumbleEvent(0U, 0U))
	val rumbleState: LiveData<RumbleEvent> get() = _rumbleState
	private val _streamStats = MutableLiveData<StreamStatsEvent>()
	val streamStats: LiveData<StreamStatsEvent> get() = _streamStats
	private val _connectionMode = MutableLiveData(ConnectionModeSnapshot.UNKNOWN)
	val connectionMode: LiveData<ConnectionModeSnapshot> get() = _connectionMode

	private var surfaceTexture: SurfaceTexture? = null
	private var surface: Surface? = null
	private var surfaceRefreshHz = 0.0
	private var surfaceVsyncOffsetNanos = 0L
	private var sustainedPerformanceModeLive = false

	init
	{
		input.controllerStateChangedCallback = {
			session?.setControllerState(it)
		}
	}

	fun shutdown()
	{
		handoffHandler.removeCallbacksAndMessages(null)
		session?.stop()
		session?.dispose()
		session = null
		// Keep the surface: it belongs to the view, whose callbacks (and detachSurface) clear it when
		// it goes away. Clearing it here left a Reconnect with no surface to decode into, so the
		// restarted session received video and decoded none (PLE-384).
		_state.value = StreamStateIdle
		//surfaceTexture?.release()
	}

	fun pause()
	{
		shutdown()
	}

	/**
	 * The user tapped Reconnect. Arms the same settling-retry a just-linked console uses (PLE-428):
	 * measured on device, the console can refuse the very next session request for several seconds
	 * after it quit ours, and without this a Reconnect that lands in that window fails immediately
	 * with a message that reads as "someone else is using your console" when nobody is. If the
	 * console really is in use by someone else, [SessionHandoffRetry]'s own budget for
	 * `rp_in_use` still expires and the real error reaches the user, just a few seconds later.
	 * A no-op for an externally managed (PSN remote) session: that path has its own reconnect
	 * handling and never arms this retry at all.
	 */
	fun armForReconnect()
	{
		if(!externallyManaged)
			handoffRetry.armForReconnect()
	}

	fun resume()
	{
		if(externallyManaged)
			return
		if(session != null)
			return
		try
		{
			val session = Session(connectInfo, logManager.createNewFile().file.absolutePath, logVerbose,
				useFrameIndexVideoTimestamps(realVideoTimestamps,
					connectInfo.videoPresenterConfig.pacingEnabled),
				decoderInputThread, context = context)
			session.setSustainedPerformanceModeLive(sustainedPerformanceModeLive)
			_state.value = if(handoffRetry.handoffInProgress) StreamStateLinkedStarting else StreamStateConnecting
			session.eventCallback = this::eventCallback
			session.start()
			val surface = surface?.takeIf { it.isValid }
			if(surface != null)
				session.setSurface(surface, connectInfo.videoProfile.maxFPS, surfaceRefreshHz,
					surfaceVsyncOffsetNanos)
			this.session = session
		}
		catch(e: CreateError)
		{
			_state.value = StreamStateCreateError(e)
		}
	}

	/** Called from the PSN controller on its IO dispatcher (PLE-312), so the state is posted, not set. */
	fun attachRemoteSession(remoteSession: Session)
	{
		session = remoteSession
		remoteSession.setSustainedPerformanceModeLive(sustainedPerformanceModeLive)
		_state.postValue(StreamStateConnecting)
		val currentSurface = surface?.takeIf { it.isValid }
		if(currentSurface != null)
			remoteSession.setSurface(currentSurface, connectInfo.videoProfile.maxFPS, surfaceRefreshHz,
				surfaceVsyncOffsetNanos)
	}

	fun setSustainedPerformanceModeLive(live: Boolean)
	{
		sustainedPerformanceModeLive = live
		session?.setSustainedPerformanceModeLive(live)
	}

	fun detachRemoteSession()
	{
		session = null
		_state.postValue(StreamStateIdle)
	}

	fun remoteEvent(event: Event) = eventCallback(event)

	fun remoteConnectionFailed(error: Throwable)
	{
		session = null
		_state.value = StreamStateRemoteError(error.message ?: "PSN remote connection failed")
	}

	/**
	 * A console that has just been linked refuses the local session for a moment (PLE-335). Absorb
	 * that refusal, keep the screen honest — linked, starting — and try again shortly.
	 *
	 * Returns true when the quit was absorbed, i.e. the caller must not report it. Runs on the
	 * native session thread; the restart itself is posted to the main thread, so the session that
	 * raised the event has returned from its callback before it is disposed.
	 */
	private fun absorbHandoffQuit(event: QuitEvent): Boolean
	{
		val decision = handoffRetry.onQuit(event.reason.value)
		if(decision !is HandoffDecision.Retry)
			return false
		Log.i(HANDOFF_TAG, "Console is linked but not ready yet (${event.reason}); " +
			"starting the stream again in ${decision.delayMs} ms, attempt ${decision.attempt}")
		_state.postValue(StreamStateLinkedStarting)
		handoffHandler.postDelayed({ restartAfterHandoff() }, decision.delayMs)
		return true
	}

	private fun restartAfterHandoff()
	{
		session?.stop()
		session?.dispose()
		session = null
		_state.value = StreamStateLinkedStarting
		resume()
	}

	private fun isVpnActive(): Boolean
	{
		val connectivity = context.getSystemService(ConnectivityManager::class.java) ?: return false
		val capabilities = connectivity.getNetworkCapabilities(connectivity.activeNetwork) ?: return false
		return capabilities.hasTransport(NetworkCapabilities.TRANSPORT_VPN)
	}

	private fun eventCallback(event: Event)
	{
		when(event)
		{
			is ConnectedEvent ->
			{
				handoffRetry.onConnected()
				_connectionMode.postValue(ConnectionModeClassifier.classify(event, isVpnActive()))
				_state.postValue(StreamStateConnected)
			}
			is QuitEvent -> if(!absorbHandoffQuit(event))
				_state.postValue(
					StreamStateQuit(
						event.reason,
						event.reasonString
					)
				)
			is LoginPinRequestEvent -> _state.postValue(
				StreamStateLoginPinRequest(
					event.pinIncorrect
				)
			)
			is RumbleEvent -> _rumbleState.postValue(event)
			is RemoteDataSocketNeededEvent -> Unit // handled by the PSN control-plane bridge
			is RegistrationEvent -> Unit // handled by the PSN control-plane bridge
			is StreamStatsEvent -> _streamStats.postValue(event)
		}
	}

	fun attachToSurfaceView(surfaceView: SurfaceView, frameRate: Float?)
	{
		surfaceView.holder.addCallback(object: SurfaceHolder.Callback {
			override fun surfaceCreated(holder: SurfaceHolder)
			{
				val surface = holder.surface
				applyFrameRate(surface, frameRate)
				setSurface(surface, surfaceView.display)
			}

			override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) { }

			override fun surfaceDestroyed(holder: SurfaceHolder)
			{
				clearFrameRate(holder.surface, frameRate)
				this@StreamSession.surface = null
				session?.setSurface(null, connectInfo.videoProfile.maxFPS, surfaceRefreshHz,
					surfaceVsyncOffsetNanos)
			}
		})
		
		val surface = surfaceView.holder.surface
		if (surface?.isValid == true) {
			applyFrameRate(surface, frameRate)
			setSurface(surface, surfaceView.display)
		}
	}

	private fun applyFrameRate(surface: Surface, frameRate: Float?)
	{
		if(frameRate == null || Build.VERSION.SDK_INT < Build.VERSION_CODES.R)
			return
		try
		{
			if(Build.VERSION.SDK_INT >= Build.VERSION_CODES.S)
				surface.setFrameRate(
					frameRate,
					Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE,
					Surface.CHANGE_FRAME_RATE_ALWAYS
				)
			else
				surface.setFrameRate(frameRate, Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE)
		}
		catch(e: RuntimeException)
		{
			Log.w("StreamSession", "Failed to request surface frame rate $frameRate", e)
		}
	}

	private fun clearFrameRate(surface: Surface, frameRate: Float?)
	{
		if(frameRate == null || Build.VERSION.SDK_INT < Build.VERSION_CODES.R)
			return
		try
		{
			if(Build.VERSION.SDK_INT >= Build.VERSION_CODES.S)
				surface.setFrameRate(
					0f,
					Surface.FRAME_RATE_COMPATIBILITY_DEFAULT,
					Surface.CHANGE_FRAME_RATE_ALWAYS
				)
			else
				surface.setFrameRate(0f, Surface.FRAME_RATE_COMPATIBILITY_DEFAULT)
		}
		catch(e: RuntimeException)
		{
			Log.w("StreamSession", "Failed to clear surface frame rate", e)
		}
	}

	/**
	 * Attach to a custom Surface (e.g., from GLSurfaceView with debanding)
	 */
	fun attachToSurface(surface: Surface, display: Display? = null)
	{
		setSurface(surface, display)
	}

	private fun setSurface(surface: Surface, display: Display?)
	{
		this.surface = surface
		surfaceRefreshHz = display?.mode?.refreshRate?.toDouble() ?: 0.0
		surfaceVsyncOffsetNanos = display?.appVsyncOffsetNanos ?: 0L
		session?.setSurface(surface, connectInfo.videoProfile.maxFPS, surfaceRefreshHz,
			surfaceVsyncOffsetNanos)
	}

	fun updateDisplayTiming(refreshHz: Double, appVsyncOffsetNanos: Long): Boolean
	{
		if(!displayTimingChanged(surfaceRefreshHz, surfaceVsyncOffsetNanos,
				refreshHz, appVsyncOffsetNanos))
			return false
		surfaceRefreshHz = refreshHz
		surfaceVsyncOffsetNanos = appVsyncOffsetNanos
		if(surface != null)
			session?.setTiming(connectInfo.videoProfile.maxFPS, surfaceRefreshHz,
				surfaceVsyncOffsetNanos)
		return true
	}

	fun detachSurface()
	{
		this.surface = null
		session?.setSurface(null, connectInfo.videoProfile.maxFPS, surfaceRefreshHz,
			surfaceVsyncOffsetNanos)
	}

	fun attachToTextureView(textureView: TextureView)
	{
		textureView.surfaceTextureListener = object: TextureView.SurfaceTextureListener {
			override fun onSurfaceTextureAvailable(surface: SurfaceTexture, width: Int, height: Int)
			{
				if(surfaceTexture != null)
					return
				surfaceTexture = surface
				setSurface(Surface(surface), textureView.display)
			}

			override fun onSurfaceTextureDestroyed(surface: SurfaceTexture): Boolean
			{
				// return false if we want to keep the surface texture
				return surfaceTexture == null
			}

			override fun onSurfaceTextureSizeChanged(surface: SurfaceTexture, width: Int, height: Int) { }
			override fun onSurfaceTextureUpdated(surface: SurfaceTexture) {}
		}

		val surfaceTexture = surfaceTexture
		if(surfaceTexture != null)
			textureView.setSurfaceTexture(surfaceTexture)
	}

	fun setLoginPin(pin: String)
	{
		session?.setLoginPin(pin)
	}
}
