// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.main

import android.os.SystemClock
import android.util.Log
import androidx.lifecycle.ViewModel
import androidx.lifecycle.LiveData
import androidx.lifecycle.MutableLiveData
import androidx.lifecycle.asLiveData
import androidx.lifecycle.viewModelScope
import fi.madekivi.pleikkari.common.*
import fi.madekivi.pleikkari.discovery.DiscoveryManager
import fi.madekivi.pleikkari.discovery.serverMac
import fi.madekivi.pleikkari.lib.ConnectInfo
import fi.madekivi.pleikkari.remote.AndroidPsnRemoteClient
import fi.madekivi.pleikkari.remote.AndroidPsnRemoteNativeBridge
import fi.madekivi.pleikkari.remote.ConnectPhase
import fi.madekivi.pleikkari.remote.ConnectProgress
import fi.madekivi.pleikkari.remote.PsnDevice
import fi.madekivi.pleikkari.remote.connectPhaseOf
import fi.madekivi.pleikkari.remote.connectProgress
import fi.madekivi.pleikkari.remote.PsnRemoteAuthenticationException
import fi.madekivi.pleikkari.remote.PsnRemoteController
import fi.madekivi.pleikkari.remote.PsnRemoteHttpException
import fi.madekivi.pleikkari.remote.PsnRemoteState
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class MainViewModel(
	val database: AppDatabase,
	val preferences: Preferences,
	private val logManager: LogManager,
	private val psnClient: AndroidPsnRemoteClient
): ViewModel()
{
	val discoveryManager = DiscoveryManager().also {
		it.active = preferences.discoveryEnabled
		viewModelScope.launch {
			it.discoveryActive.collect { active ->
				preferences.discoveryEnabled = active
			}
		}
	}

	val displayHosts by lazy {
		combine(
			database.manualHostDao().getAll(),
			database.registeredHostDao().getAll(),
			discoveryManager.discoveredHosts
		) { manualHosts, registeredHosts, discoveredHosts ->
			val macRegisteredHosts = registeredHosts.associateBy { it.serverMac }
			val idRegisteredHosts = registeredHosts.associateBy { it.id }
			discoveredHosts.map {
				DiscoveredDisplayHost(it.serverMac?.let { mac -> macRegisteredHosts[mac] }, it)
			} +
			manualHosts.map {
				ManualDisplayHost(it.registeredHost?.let { id -> idRegisteredHosts[id] }, it)
			}
		}.asLiveData()
	}

	val discoveryActive by lazy {
		discoveryManager.discoveryActive.asLiveData()
	}

	val configuredConsoleCount by lazy {
		combine(
			database.manualHostDao().getAll(),
			database.registeredHostDao().getAll()
		) { manualHosts, registeredHosts -> manualHosts.size + registeredHosts.size }
			.asLiveData()
	}

	private val psnDevices = MutableStateFlow<List<PsnDevice>>(emptyList())
	private val _psnListState = MutableLiveData<PsnConsoleListState>(PsnConsoleListState.Hidden)
	val psnListState: LiveData<PsnConsoleListState> get() = _psnListState
	private val _psnAction = MutableLiveData<PsnConsoleActionState?>(null)
	val psnAction: LiveData<PsnConsoleActionState?> get() = _psnAction
	private val _psnError = MutableLiveData<PsnActionError?>(null)
	val psnError: LiveData<PsnActionError?> get() = _psnError
	private val _psnPlayRequest = MutableLiveData<PsnPlayRequest?>(null)
	val psnPlayRequest: LiveData<PsnPlayRequest?> get() = _psnPlayRequest
	/** Plain progress for the console card while a PSN connect runs (PLE-337). */
	private val _psnProgress = MutableLiveData<ConnectProgress?>(null)
	val psnProgress: LiveData<ConnectProgress?> get() = _psnProgress
	private var psnLoadJob: Job? = null
	private var psnActionJob: Job? = null
	private var actionController: PsnRemoteController? = null
	var lastFailedPsnConsole: PsnConsole? = null
		private set

	val psnConsoles = combine(psnDevices, database.registeredHostDao().getAll(), ::matchPsnConsoles).asLiveData()

	fun setPsnEnabled(enabled: Boolean, signedIn: Boolean = true)
	{
		if(!enabled || !signedIn)
		{
			psnLoadJob?.cancel()
			psnDevices.value = emptyList()
			_psnListState.value = PsnConsoleListState.Hidden
			return
		}
		if(shouldReloadPsnConsoleList(_psnListState.value))
			loadPsnConsoles()
	}

	fun loadPsnConsoles()
	{
		if(psnLoadJob?.isActive == true)
			return
		psnLoadJob = viewModelScope.launch {
			_psnListState.value = PsnConsoleListState.Loading
			try
			{
				val listing = psnClient.listDeviceListing()
				Log.i(PSN_LIST_TAG, "PSN console list: ${listing.clientCount} PS5 client(s) on the account, " +
					"${listing.devices.size} with Remote Play enabled")
				psnDevices.value = listing.devices
				_psnListState.value = PsnConsoleListState.Ready
			}
			catch(cancelled: CancellationException)
			{
				throw cancelled
			}
			catch(error: Throwable)
			{
				Log.w(PSN_LIST_TAG, "PSN console list failed: ${describePsnFailure(error)}", error)
				_psnListState.value = PsnConsoleListState.Error(
					error.message ?: "Unable to list consoles on your PSN account",
					if(error is PsnRemoteAuthenticationException) PsnErrorRecovery.SIGN_IN else PsnErrorRecovery.RETRY
				)
			}
		}
	}

	/**
	 * The account's consoles as of now. [psnConsoles] reaches observers after [psnListState] turns
	 * Ready, so code reacting to Ready reads the list here.
	 */
	fun currentPsnConsoles(): List<PsnConsole> = psnDevices.value.map { PsnConsole(it, null) }

	internal fun showPsnPreview(devices: List<PsnDevice>)
	{
		psnLoadJob?.cancel()
		psnDevices.value = devices
		_psnListState.value = PsnConsoleListState.Ready
	}

	fun connectInfo(host: RegisteredHost?, autoRegister: Boolean = false): ConnectInfo = ConnectInfo(
		ps5 = true,
		host = "",
		registKey = host?.rpRegistKey ?: ByteArray(16),
		morning = host?.rpKey ?: ByteArray(16),
		videoProfile = preferences.videoProfile,
		decoderLowLatencyEnabled = preferences.decoderLowLatencyEnabled,
		threadPriorityBoostEnabled = preferences.threadPriorityBoostEnabled,
		decoderLateFrameRecoveryEnabled = preferences.decoderLateFrameRecoveryEnabled,
		packetLossMax = preferences.packetLossMax,
		adaptiveLossReport = preferences.adaptiveLossReport,
		takionVideoPacketReorderingDisabled = preferences.takionVideoPacketReorderingDisabled,
		feedbackStateMinIntervalMs = if(preferences.feedbackReducedIntervalEnabled) 4 else 0,
		feedbackStatsLogIntervalMs = preferences.feedbackStatsLogIntervalMs,
		audioBufferBursts = preferences.audioBufferBursts,
		audioFifoMs = preferences.audioFifoMs,
		autoRegister = autoRegister,
		performanceModeEnabled = preferences.performanceModeEnabled,
		decoderOperatingRate = preferences.decoderOperatingRate,
		decoderOperatingRateDefault = preferences.decoderOperatingRateDefault,
		decoderOperatingRateAuto = preferences.decoderOperatingRateAuto,
		decoderRealtimePriority = preferences.decoderRealtimePriority,
		decoderQcomProfileOperatingRate = GoDecoderProfile.operatingRate(preferences),
		videoTimestampRateHz = preferences.videoTimestampRateHz,
		// The Home session summary consumes the same 1 Hz counters as the optional overlay.
		streamDiagnosticsEnabled = true,
		videoPresenterConfig = preferences.videoPresenterConfig
	)

	fun playPsnConsole(console: PsnConsole)
	{
		val registered = console.registeredHost
		if(registered != null)
		{
			_psnError.value = null
			lastFailedPsnConsole = null
			_psnPlayRequest.value = PsnPlayRequest(console)
			return
		}
		runPsnAction(console, PsnConsoleAction.PLAY) {
			val bridge = AndroidPsnRemoteNativeBridge(
				connectInfo(null, autoRegister = true),
				logManager.createNewFile().file.absolutePath,
				preferences.logVerbose,
				preferences.realVideoTimestamps,
				preferences.decoderInputThreadEnabled
			)
			val controller = psnClient.controller(bridge).also { actionController = it }
			val progressJob = viewModelScope.launch { trackPsnProgress(controller) }
			try
			{
				controller.connect(console.device)
				val registHost = (controller.state.value as? PsnRemoteState.Registered)?.host
					?: error("PSN registration did not return console credentials")
				val savedHost = withContext(Dispatchers.IO) {
					val host = RegisteredHost(registHost)
					database.registeredHostDao().deleteByMac(host.serverMac)
					host.copy(id = database.registeredHostDao().insert(host))
				}
				PsnPlayRequest(console.copy(registeredHost = savedHost), justLinked = true)
			}
			finally
			{
				progressJob.cancel()
				_psnProgress.value = null
			}
		}
	}

	/**
	 * PLE-337/PLE-340: turn the control-plane states into something a waiting user can read. The
	 * card must keep moving on its own even while one phase sits still, so [ConnectProgress] is
	 * driven by [sessionStartedAt] - set once, here, when this run of tracking begins - never by
	 * how long the current phase alone has run; that would restart the bar at every phase change,
	 * which is the "bar per phase" PLE-340 replaced.
	 *
	 * Polls rather than collects because the elapsed time has to advance between state changes.
	 */
	private suspend fun trackPsnProgress(controller: PsnRemoteController)
	{
		var phase = ConnectPhase.REACHING_NETWORK
		val sessionStartedAt = SystemClock.elapsedRealtime()
		var shown: ConnectProgress? = null
		while(true)
		{
			val now = SystemClock.elapsedRealtime()
			val current = connectPhaseOf(controller.state.value)
			if(current != null)
				phase = current
			val progress = connectProgress(phase, now - sessionStartedAt)
			if(progress != shown)
			{
				shown = progress
				_psnProgress.value = progress
			}
			delay(PSN_PROGRESS_TICK_MS)
		}
	}

	fun retryLastPsnAction()
	{
		val console = lastFailedPsnConsole ?: return
		_psnError.value = null
		playPsnConsole(console)
	}

	private fun runPsnAction(
		console: PsnConsole,
		action: PsnConsoleAction,
		block: suspend () -> PsnPlayRequest
	)
	{
		if(psnActionJob?.isActive == true)
			return
		psnActionJob = viewModelScope.launch {
			_psnError.value = null
			_psnAction.value = PsnConsoleActionState(console.device.duid, action)
			try
			{
				_psnPlayRequest.value = block()
				lastFailedPsnConsole = null
			}
			catch(cancelled: CancellationException)
			{
				throw cancelled
			}
			catch(error: Throwable)
			{
				Log.w(PSN_LIST_TAG, "PSN ${action.name.lowercase()} failed: ${describePsnFailure(error)}", error)
				lastFailedPsnConsole = console
				_psnError.value = PsnActionError(
					error.message ?: "Could not start Remote Play",
					if(error is PsnRemoteAuthenticationException) PsnErrorRecovery.SIGN_IN else PsnErrorRecovery.RETRY
				)
			}
			finally
			{
				actionController?.close()
				actionController = null
				_psnAction.value = null
			}
		}
	}

	fun clearPsnError()
	{
		_psnError.value = null
	}

	fun clearPsnPlayRequest()
	{
		_psnPlayRequest.value = null
	}

	fun deleteManualHost(manualHost: ManualHost)
	{
		viewModelScope.launch(Dispatchers.IO) {
			try {
				database.manualHostDao().delete(manualHost)
			} catch(_: Exception) {}
		}
	}

	fun deleteRegisteredHost(registeredHost: RegisteredHost)
	{
		viewModelScope.launch(Dispatchers.IO) {
			try {
				database.registeredHostDao().delete(registeredHost)
			} catch(_: Exception) {}
		}
	}

	override fun onCleared()
	{
		super.onCleared()
		psnLoadJob?.cancel()
		psnActionJob?.cancel()
		actionController?.close()
		discoveryManager.dispose()
	}
}

private const val PSN_LIST_TAG = "PsnConsoles"

/** How often the console card's elapsed count is refreshed; a quarter second reads as smooth. */
private const val PSN_PROGRESS_TICK_MS = 250L

/** One support-readable line: exception type, message, HTTP status and PSN's error excerpt. Never tokens. */
internal fun describePsnFailure(error: Throwable): String = buildString {
	append(error.javaClass.simpleName)
	error.message?.let { append(": ").append(it) }
	// PLE-296: the messages above are now user-safe and never name the status, so it is spelled out here
	// explicitly -- this function, not the on-screen text, is where the raw HTTP code belongs.
	val httpCode = when(error)
	{
		is PsnRemoteHttpException -> error.httpCode
		is PsnRemoteAuthenticationException -> error.httpCode
		else -> null
	}
	httpCode?.let { append(" | HTTP ").append(it) }
	val detail = when(error)
	{
		is PsnRemoteHttpException -> error.detail
		is PsnRemoteAuthenticationException -> error.detail
		else -> null
	}
	detail?.let { append(" | PSN said: ").append(it) }
	error.cause?.let { append(" | cause ").append(it.javaClass.simpleName).append(": ").append(it.message) }
}
