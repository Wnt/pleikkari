// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.stream

import android.animation.Animator
import android.animation.AnimatorListenerAdapter
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.res.ColorStateList
import android.hardware.display.DisplayManager
import android.content.res.Configuration
import android.graphics.Color
import android.graphics.Matrix
import android.graphics.Rect
import android.graphics.PixelFormat
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.net.wifi.WifiManager
import android.opengl.GLSurfaceView
import android.os.*
import android.util.Log
import android.view.*
import android.widget.CheckedTextView
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.PopupWindow
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.app.AlertDialog
import androidx.core.content.ContextCompat
import androidx.core.content.IntentCompat
import fi.madekivi.pleikkari.lib.QuitEvent
import fi.madekivi.pleikkari.lib.QuitReason
import androidx.lifecycle.lifecycleScope
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.core.view.isGone
import androidx.core.view.isVisible
import androidx.fragment.app.Fragment
import androidx.lifecycle.*
import com.google.android.material.color.MaterialColors
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import fi.madekivi.pleikkari.BuildConfig
import fi.madekivi.pleikkari.remote.ConnectPhase
import fi.madekivi.pleikkari.remote.applyTo
import fi.madekivi.pleikkari.remote.connectProgress
import fi.madekivi.pleikkari.remote.hideConnectBar
import fi.madekivi.pleikkari.remote.streamConnectPhase
import fi.madekivi.pleikkari.R
import fi.madekivi.pleikkari.common.Preferences
import fi.madekivi.pleikkari.common.ext.viewModelFactory
import fi.madekivi.pleikkari.databinding.ActivityStreamBinding
import fi.madekivi.pleikkari.lib.Codec
import fi.madekivi.pleikkari.lib.ConnectInfo
import fi.madekivi.pleikkari.lib.ConnectVideoProfile
import fi.madekivi.pleikkari.remote.PsnDevice
import fi.madekivi.pleikkari.session.*
import fi.madekivi.pleikkari.touchcontrols.DefaultTouchControlsFragment
import fi.madekivi.pleikkari.touchcontrols.TouchControlsFragment
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.launchIn
import kotlinx.coroutines.flow.onEach
import java.util.Locale
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min

private sealed class DialogContents
private object StreamQuitDialog: DialogContents()
private object UserQuitDialog: DialogContents()
private object CreateErrorDialog: DialogContents()
private object PinRequestDialog: DialogContents()

@Suppress("DEPRECATION")
internal fun wifiLockModeForSdk(sdkInt: Int) =
	if(sdkInt >= Build.VERSION_CODES.Q)
		WifiManager.WIFI_MODE_FULL_LOW_LATENCY
	else
		WifiManager.WIFI_MODE_FULL_HIGH_PERF

class StreamActivity : AppCompatActivity()
{
	companion object
	{
		const val EXTRA_CONNECT_INFO = "connect_info"
		const val EXTRA_PSN_DEVICE = "psn_device"
		/** The console was linked moments ago, so it may still refuse the session (PLE-335). */
		const val EXTRA_JUST_LINKED = "just_linked"
		const val EXTRA_DIAGNOSTICS_PREVIEW = "diagnostics_preview"
		const val EXTRA_STREAM_SUMMARY = "stream_summary"
		/**
		 * PLE-422: devtools-only affordance to drive a live session into the real error-quit path
		 * (see [registerQuitReasonInjector]), so the error dialog and Reconnect can be exercised on
		 * demand instead of waiting on a real network failure.
		 */
		// PLE-573: the action keeps its pre-rename name; capture scripts and docs broadcast it.
		const val ACTION_INJECT_QUIT_REASON = "com.metallic.chiaki.debug.INJECT_QUIT_REASON"
		const val EXTRA_QUIT_REASON = "quit_reason"
		private const val HIDE_UI_TIMEOUT_MS = 3500L
		/** How often the connect overlay's second count is redrawn (PLE-337). */
		private const val CONNECT_PROGRESS_TICK_MS = 500L

		internal fun shouldRequestUnbufferedGamepadDispatch(source: Int, sdkInt: Int, enabled: Boolean): Boolean
		{
			if(!enabled || sdkInt < Build.VERSION_CODES.R)
				return false
			return source and InputDevice.SOURCE_CLASS_JOYSTICK == InputDevice.SOURCE_CLASS_JOYSTICK
		}

		internal fun performanceModeDiagnosticFlags(sustainedLive: Boolean, adpfLive: Boolean) =
			buildList {
				if(sustainedLive) add("perf-sustained")
				if(adpfLive) add("perf-adpf")
			}
	}

	private lateinit var viewModel: StreamViewModel
	private lateinit var binding: ActivityStreamBinding
	private lateinit var insetsController: WindowInsetsControllerCompat
	private var originalPreferredDisplayModeId: Int? = null
	private var displayRefreshRateRequest: Pair<Preferences.DisplayRefreshRateMode, Float>? = null
	private var displayRefreshRateDisplayId: Int? = null
	private var lastConfigurationDisplayId: Int? = null
	private var performanceModeRequested = false
	private var sustainedPerformanceModeEnabled = false
	private var sustainedPerformanceModeRefusalLogged = false
	private var wifiLock: WifiManager.WifiLock? = null
	private var diagnosticsOverlay: StreamDiagnosticsOverlay? = null
	private var displayManager: DisplayManager? = null
	private var displayListener: DisplayManager.DisplayListener? = null
	private val summaryAccumulator = StreamSummaryAccumulator()
	private var quitEndReason: StreamEndReason? = null
	private var summaryResultSet = false
	private val networkQualityClassifier = NetworkQualityClassifier()
	private var networkQuality = NetworkQualitySnapshot.UNKNOWN
	private var networkQualityDetailsExpanded = false
	private var connectionMode = ConnectionModeSnapshot.UNKNOWN
	private val stallTracker = StallTracker()
	private var streamTransformMode = TransformMode.FIT
	private var touchControlsFragment: TouchControlsFragment? = null
	private var lastWindowInsets: WindowInsetsCompat? = null
	private var lastLayoutBoundsLog: String? = null
	private var quitReasonInjector: BroadcastReceiver? = null

	private val uiVisibilityHandler = Handler(Looper.getMainLooper())

	// PLE-337/PLE-340: which wait the connect is in, and when this attempt started (not the
	// current phase - see setConnectPhase), so the overlay's bar can fill continuously.
	private var connectPhase: ConnectPhase? = null
	private var connectSessionStartedAt = 0L
	private val connectProgressTick = object: Runnable
	{
		override fun run()
		{
			renderConnectProgress()
			uiVisibilityHandler.postDelayed(this, CONNECT_PROGRESS_TICK_MS)
		}
	}

	override fun onCreate(savedInstanceState: Bundle?)
	{
		super.onCreate(savedInstanceState)

		val diagnosticsPreview = BuildConfig.DEBUG && intent.getBooleanExtra(EXTRA_DIAGNOSTICS_PREVIEW, false)
		val connectInfo = IntentCompat.getParcelableExtra(intent, EXTRA_CONNECT_INFO, ConnectInfo::class.java)
			?: if(diagnosticsPreview) diagnosticsPreviewConnectInfo() else null
		val psnDevice = IntentCompat.getParcelableExtra(intent, EXTRA_PSN_DEVICE, PsnDevice::class.java)
		val justLinked = intent.getBooleanExtra(EXTRA_JUST_LINKED, false)
		if(connectInfo == null)
		{
			finish()
			return
		}

		viewModel = ViewModelProvider(this, viewModelFactory {
			StreamViewModel(application, connectInfo, psnDevice, diagnosticsPreview, justLinked)
		})[StreamViewModel::class.java]

		viewModel.input.observe(this)
		viewModel.setTouchscreenAvailable(hasTouchscreen(resources.configuration))

		binding = ActivityStreamBinding.inflate(layoutInflater)
		setContentView(binding.root)
		prepareWindowTouchLayout()
		performanceModeRequested = connectInfo.performanceModeEnabled

		val preferences = Preferences(this)
		WindowCompat.setDecorFitsSystemWindows(window, false)
		if(preferences.streamWindowOptimizationsEnabled)
			configureWindowOptimizations()
		val displayRefreshRateMode = effectiveDisplayRefreshRateMode(
			preferences.displayRefreshRateMode,
			preferences.realVideoTimestamps,
			preferences.videoPacingEnabled
		)
		configureDisplayRefreshRate(displayRefreshRateMode, connectInfo.videoProfile.maxFPS.toFloat())
		streamDisplay().let { display ->
			lastConfigurationDisplayId = display.displayId
			Log.i("StreamActivity", "Stream window created: ${describeWindow(display, resources.configuration)}")
		}
		insetsController = WindowCompat.getInsetsController(window, window.decorView)
		insetsController.systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE

		ViewCompat.setOnApplyWindowInsetsListener(window.decorView) { _, insets ->
			lastWindowInsets = insets
			applyOverlayInsets(insets)
			val systemBars = insets.isVisible(WindowInsetsCompat.Type.systemBars())
			if(systemBars)
				showOverlay()
			insets
		}
		binding.root.addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ ->
			applyWindowTouchLayout()
		}

		viewModel.onScreenControlsEnabled.observe(this, Observer {
			if(binding.controllerButton.isChecked != it)
				binding.controllerButton.isChecked = it
			binding.root.post(::applyWindowTouchLayout)
		})
		binding.controllerButton.addOnCheckedChangeListener { _, isChecked ->
			viewModel.setOnScreenControlsEnabled(isChecked)
			showOverlay()
		}
		binding.streamMenuButton.setOnClickListener { showDisplayModeMenu() }
		binding.networkQualityChip.setOnClickListener {
			if(networkQuality.level != NetworkQualityLevel.UNKNOWN)
				networkQualityDetailsExpanded = !networkQualityDetailsExpanded
			updateNetworkQualityChip()
			showOverlay()
		}
		binding.quitButton.setOnClickListener { showQuitConfirmation() }
		binding.aspectRatioLayout.setOnClickListener { showOverlay() }

// Setup video output based on debanding preference
		setupVideoOutput()
		
		val prefs = Preferences(this)
		if (prefs.touchscreenTouchpadEnabled) {
			binding.streamTouchpadView.visibility = View.VISIBLE
			binding.streamTouchpadView.controllerState
				.onEach { streamTouchpadState.value = it }
				.launchIn(lifecycleScope)
		}

		viewModel.session.state.observe(this, Observer { this.stateChanged(it) })
		viewModel.remoteConnectPhase.observe(this, Observer {
			remoteConnectPhase = it
			updateConnectOverlay()
		})
		if(BuildConfig.DEBUG)
			quitReasonInjector = registerQuitReasonInjector()
		if(diagnosticsPreview || preferences.streamDiagnosticsOverlayEnabled)
		{
			val overlay = StreamDiagnosticsOverlay(this) {
				diagnosticsUiState(preferences, connectInfo)
			}
			diagnosticsOverlay = overlay
			binding.root.post { overlay.show(binding.root) }
			Log.i("StreamActivity", "Stream diagnostics overlay enabled; redraw interval 1000 ms")
		}
		else
			Log.i("StreamActivity", "Stream diagnostics overlay disabled")
		// PLE-366: the badge's verdict has to be auditable as a series, not as a screenshot per
		// phase -- a stall the tail arm catches lasts 8 s, less than a screenshot cadence. This
		// rides the same 1 Hz debug log the captures already parse.
		val qualityLogEnabled = preferences.feedbackStatsLogEnabled
		viewModel.session.streamStats.observe(this) { stats ->
			val link = diagnosticsNetworkLink()
			summaryAccumulator.add(stats, link)
			networkQuality = networkQualityClassifier.update(stats, link)
			// PLE-473: the classifier's own stallMillis is a window max, so it stays elevated for
			// up to FAST_WINDOW_SECONDS after the event -- fine for the badge, but it would make
			// "N s ago" drift forward every tick the event is still in-window. Reading the raw
			// per-second gap here instead (same fields PLE-464 plumbed, same computation as
			// NetworkQuality.kt's per-sample stallMillis) timestamps the actual event once.
			stallTracker.update(
				max(stats.takionMaxReceiveGapMillis, stats.takionSilenceMillis).toDouble(),
				SystemClock.elapsedRealtime()
			)
			if(qualityLogEnabled)
				Log.i("NetworkQuality", String.format(Locale.US,
					"Quality badge: level %s cause %s | median rtt_ms %.2f jitter_ms %.2f loss_pct %.2f" +
						" | tail_rate jitter %.2f loss %.2f cut %.2f" +
						" | stall_ms %.0f cut %.0f poor %.0f",
					networkQuality.level.name, networkQuality.cause.name,
					networkQuality.fastRttMillis, networkQuality.fastJitterMillis,
					networkQuality.fastLossPercent, networkQuality.tailJitterRate,
					networkQuality.tailLossRate, NetworkQualityThresholds.TAIL_RATE_CUT,
					networkQuality.stallMillis, NetworkQualityThresholds.STALL_MS,
					NetworkQualityThresholds.POOR_STALL_MS))
			updateNetworkQualityChip()
			diagnosticsOverlay?.update(stats)
		}
		viewModel.session.connectionMode.observe(this) { mode ->
			// PLE-509: ConnectedEvent (and so this observer) fires once per session, so no extra
			// per-session guard is needed against repeating the toast.
			if(!mode.measured && mode.mode != ConnectionMode.UNKNOWN && preferences.senkushaFallbackNoticeEnabled)
				Toast.makeText(this, R.string.stream_senkusha_fallback_notice, Toast.LENGTH_LONG).show()
			connectionMode = mode
		}
		updateNetworkQualityChip()
		adjustStreamViewAspect()

		if(Preferences(this).rumbleEnabled)
		{
			val vibrator = getSystemService(VIBRATOR_SERVICE) as Vibrator
			viewModel.session.rumbleState.observe(this, Observer {
				val amplitude = min(255, (it.left.toInt() + it.right.toInt()) / 2)
				vibrator.cancel()
				if(amplitude == 0)
					return@Observer
				if(Build.VERSION.SDK_INT >= Build.VERSION_CODES.O)
					vibrator.vibrate(VibrationEffect.createOneShot(1000, amplitude))
				else
					vibrator.vibrate(1000)
			})
		}
	}

	private fun diagnosticsPreviewConnectInfo() = ConnectInfo(
		ps5 = true,
		host = "diagnostics-preview",
		registKey = byteArrayOf(),
		morning = byteArrayOf(),
		videoProfile = ConnectVideoProfile(1920, 1080, 60, 15_000, Codec.CODEC_H265),
		decoderLowLatencyEnabled = false,
		threadPriorityBoostEnabled = false,
		decoderLateFrameRecoveryEnabled = false,
		packetLossMax = 0.0,
		takionVideoPacketReorderingDisabled = false,
		streamDiagnosticsEnabled = true
	)

	private var controlsJob: Job? = null
	private var debandRenderer: DebandRenderer? = null
	private var eglRenderer: EglRenderer? = null
	private val streamTouchpadState = MutableStateFlow(fi.madekivi.pleikkari.lib.ControllerState())

	private fun setupVideoOutput()
	{
		val prefs = Preferences(this)
		viewModel.session.detachSurface()

		if(prefs.debandingEnabled)
		{
			if(BuildConfig.CHIAKI_ANDROID_EGL_RENDERER && Build.VERSION.SDK_INT >= Build.VERSION_CODES.O)
				setupEglDebandOutput(prefs)
			else
				setupGlSurfaceViewDebandOutput(prefs)
		}
		else
		{
			// Straight to SurfaceView, no shader stage
			binding.surfaceView.visibility = View.VISIBLE
			binding.debandSurfaceView.visibility = View.GONE
			val frameRate = if(prefs.displayRefreshRateMode == Preferences.DisplayRefreshRateMode.MATCH_STREAM)
				viewModel.connectInfo.videoProfile.maxFPS.toFloat()
			else
				null
			viewModel.session.attachToSurfaceView(binding.surfaceView, frameRate)
		}
	}

	private fun setupEglDebandOutput(prefs: Preferences)
	{
		binding.surfaceView.visibility = View.VISIBLE
		binding.debandSurfaceView.visibility = View.GONE
		binding.surfaceView.holder.setFormat(PixelFormat.RGBA_8888)
		val renderer = EglRenderer(
			binding.surfaceView,
			viewModel.connectInfo.videoProfile.width,
			viewModel.connectInfo.videoProfile.height,
			prefs.sharpnessIntensity,
			onDecoderSurfaceReady = { surface ->
				viewModel.session.attachToSurface(surface, binding.surfaceView.display)
			},
			onDecoderSurfaceDestroyed = { viewModel.session.detachSurface() },
			onUnavailable = { failedRenderer ->
				if(eglRenderer === failedRenderer)
				{
					failedRenderer.release()
					eglRenderer = null
					setupGlSurfaceViewDebandOutput(prefs)
				}
			}
		)
		eglRenderer = renderer
		renderer.start()
	}

	private fun setupGlSurfaceViewDebandOutput(prefs: Preferences)
	{
		val renderWhenDirty = prefs.debandRenderWhenDirtyEnabled
		// Decode into a SurfaceTexture consumed by the established deband/RCAS renderer.
		binding.surfaceView.visibility = View.GONE
		binding.debandSurfaceView.visibility = View.VISIBLE
		debandRenderer = DebandRenderer(
			onSurfaceReady = { surface ->
				viewModel.session.attachToSurface(surface, binding.debandSurfaceView.display)
			},
			onRequestRender = { binding.debandSurfaceView.requestRender() },
			renderWhenDirty = renderWhenDirty
		)
		binding.debandSurfaceView.setEGLContextClientVersion(3)
		binding.debandSurfaceView.setEGLConfigChooser(8, 8, 8, 8, 0, 0)
		binding.debandSurfaceView.holder.setFormat(PixelFormat.RGBA_8888)
		binding.debandSurfaceView.setRenderer(debandRenderer)
		debandRenderer?.sharpness = prefs.sharpnessIntensity
		// Default (flag off) keeps continuous rendering; the flag lets an A/B test measure
		// GPU/power savings from rendering only when the decoder delivers a new frame.
		binding.debandSurfaceView.renderMode = if(renderWhenDirty)
			GLSurfaceView.RENDERMODE_WHEN_DIRTY
		else
			GLSurfaceView.RENDERMODE_CONTINUOUSLY
	}

	@Suppress("DEPRECATION")
	private fun configureWindowOptimizations()
	{
		val attributes = window.attributes
		if(Build.VERSION.SDK_INT >= Build.VERSION_CODES.P)
			attributes.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
		if(Build.VERSION.SDK_INT >= Build.VERSION_CODES.R)
			attributes.preferMinimalPostProcessing = true
		window.attributes = attributes
	}

	private fun configurePerformanceMode(enabled: Boolean)
	{
		if(!enabled)
		{
			if(sustainedPerformanceModeEnabled && Build.VERSION.SDK_INT >= Build.VERSION_CODES.N)
				window.setSustainedPerformanceMode(false)
			sustainedPerformanceModeEnabled = false
			viewModel.session.setSustainedPerformanceModeLive(false)
			return
		}
		if(sustainedPerformanceModeEnabled)
			return
		if(Build.VERSION.SDK_INT >= Build.VERSION_CODES.N
			&& getSystemService(PowerManager::class.java).isSustainedPerformanceModeSupported)
		{
			window.setSustainedPerformanceMode(true)
			sustainedPerformanceModeEnabled = true
			viewModel.session.setSustainedPerformanceModeLive(true)
			Log.i("StreamActivity", "Sustained performance mode enabled")
		}
		else
		{
			val reason = if(Build.VERSION.SDK_INT < Build.VERSION_CODES.N)
				"requires Android 7 (API 24)"
			else
				"PowerManager reports unsupported"
			if(!sustainedPerformanceModeRefusalLogged)
			{
				sustainedPerformanceModeRefusalLogged = true
				Log.i("StreamActivity", "Sustained performance mode refused: $reason")
			}
		}
	}

	@Suppress("DEPRECATION")
	private fun configureWifiLock(enabled: Boolean)
	{
		if(!enabled)
		{
			wifiLock?.let { lock ->
				if(lock.isHeld)
					lock.release()
			}
			wifiLock = null
			return
		}
		if(wifiLock?.isHeld == true)
			return

		try
		{
			val mode = wifiLockModeForSdk(Build.VERSION.SDK_INT)
			val lock = getSystemService(WifiManager::class.java)
				.createWifiLock(mode, "$packageName:StreamWifiLowLatency")
			lock.setReferenceCounted(false)
			lock.acquire()
			wifiLock = lock
			Log.i("StreamActivity", "Wi-Fi lock acquired in mode $mode")
		}
		catch(e: RuntimeException)
		{
			Log.e("StreamActivity", "Failed to acquire Wi-Fi lock", e)
		}
	}

	private fun configureDisplayRefreshRate(mode: Preferences.DisplayRefreshRateMode, streamFrameRate: Float)
	{
		displayRefreshRateRequest = mode to streamFrameRate
		if(mode == Preferences.DisplayRefreshRateMode.SYSTEM_DEFAULT)
			return

		val display = streamDisplay()
		displayRefreshRateDisplayId = display.displayId
		val currentMode = display.mode.toCandidate()
		val targetMode = selectDisplayMode(mode, streamFrameRate, currentMode,
			display.supportedModes.map { it.toCandidate() })
		if(targetMode == null)
		{
			Log.w("StreamActivity", "No display mode for $mode at ${currentMode.width}x${currentMode.height} on display ${display.displayId}")
			return
		}

		val attributes = window.attributes
		if(originalPreferredDisplayModeId == null)
			originalPreferredDisplayModeId = attributes.preferredDisplayModeId
		attributes.preferredDisplayModeId = targetMode.modeId
		window.attributes = attributes
		Log.i("StreamActivity", "Requested display mode ${targetMode.modeId}: ${targetMode.width}x${targetMode.height}@${targetMode.refreshRate} on display ${display.displayId}")
	}

	/**
	 * A mode id belongs to one display, so a window that moved (DeX starting or stopping) must ask
	 * again on the display it is on now; the old id means nothing there (PLE-384).
	 */
	private fun reconfigureDisplayRefreshRateIfMoved(displayId: Int)
	{
		val (mode, streamFrameRate) = displayRefreshRateRequest ?: return
		if(mode == Preferences.DisplayRefreshRateMode.SYSTEM_DEFAULT || displayId == displayRefreshRateDisplayId)
			return
		configureDisplayRefreshRate(mode, streamFrameRate)
	}

	/** The display the stream window is on; see [streamDisplayOf]. */
	@Suppress("DEPRECATION")
	private fun streamDisplay(): Display = streamDisplayOf(
		if(Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) display else null,
		binding.root.display,
		windowManager.defaultDisplay
	)

	private fun Display.Mode.toCandidate() =
		DisplayModeCandidate(modeId, physicalWidth, physicalHeight, refreshRate)

	private fun restoreDisplayRefreshRate()
	{
		val modeId = originalPreferredDisplayModeId ?: return
		val attributes = window.attributes
		attributes.preferredDisplayModeId = modeId
		window.attributes = attributes
		originalPreferredDisplayModeId = null
	}

	override fun onAttachFragment(fragment: Fragment)
	{
		super.onAttachFragment(fragment)
		if(fragment is TouchControlsFragment)
		{
			controlsJob?.cancel()
			controlsJob = combine(fragment.controllerState, streamTouchpadState) { a, b -> a or b }
				.onEach { viewModel.input.touchControllerState = it }
				.launchIn(lifecycleScope)
			fragment.onScreenControlsEnabled = viewModel.onScreenControlsEnabled
			fragment.overlayRevealRequested = ::showOverlay
			touchControlsFragment = fragment
		}
	}

	override fun onResume()
	{
		super.onResume()
		configurePerformanceMode(performanceModeRequested)
		configureWifiLock(Preferences(this).wifiLowLatencyLockEnabled)
		hideSystemUI()
		if(debandRenderer != null) {
			binding.debandSurfaceView.onResume()
			// In RENDERMODE_WHEN_DIRTY, onResume() alone won't redraw the last frame; force one
			// so the surface isn't left blank until the next decoded frame arrives.
			binding.debandSurfaceView.requestRender()
		}
		viewModel.resume()
		registerDisplayListener()
	}

	override fun onPause()
	{
		unregisterDisplayListener()
		configureWifiLock(false)
		super.onPause()
		configurePerformanceMode(false)
		if(debandRenderer != null) {
			binding.debandSurfaceView.onPause()
		}
		// A configuration change this activity does not handle in place (the density step when DeX
		// moves the window between the phone and the TV) recreates it; the view model and its
		// session outlive that, so keep streaming instead of ending the session (PLE-384).
		if(isChangingConfigurations)
			Log.i("StreamActivity", "Recreating the stream window for a configuration change; the session continues")
		else
			viewModel.pause()
	}

	private fun registerDisplayListener()
	{
		val preferences = Preferences(this)
		if(Build.VERSION.SDK_INT < Build.VERSION_CODES.N
				|| !presenterDisplayTimingUpdatesEnabled(
					preferences.displayRefreshRateMode,
					preferences.videoPacingEnabled
				)
				|| displayListener != null)
			return

		val manager = getSystemService(DisplayManager::class.java)
		val listener = object: DisplayManager.DisplayListener
		{
			override fun onDisplayAdded(displayId: Int) = Unit
			override fun onDisplayRemoved(displayId: Int) = Unit
			override fun onDisplayChanged(displayId: Int)
			{
				val streamDisplay = streamDisplay()
				if(displayId != streamDisplay.displayId)
					return
				manager.getDisplay(displayId)?.let(::updatePresenterDisplayTiming)
			}
		}
		displayManager = manager
		displayListener = listener
		manager.registerDisplayListener(listener, uiVisibilityHandler)
		val streamDisplay = streamDisplay()
		updatePresenterDisplayTiming(manager.getDisplay(streamDisplay.displayId) ?: streamDisplay)
	}

	private fun unregisterDisplayListener()
	{
		displayListener?.let { displayManager?.unregisterDisplayListener(it) }
		displayListener = null
		displayManager = null
	}

	private fun updatePresenterDisplayTiming(display: Display)
	{
		val refreshHz = display.mode.refreshRate.toDouble()
		if(viewModel.session.updateDisplayTiming(refreshHz, display.appVsyncOffsetNanos))
			Log.i("StreamActivity", "Display timing changed: ${"%.2f".format(Locale.US, refreshHz)} Hz")
	}

	override fun onConfigurationChanged(newConfig: Configuration)
	{
		super.onConfigurationChanged(newConfig)
		viewModel.input.refreshDisplayRotation()
		viewModel.setTouchscreenAvailable(hasTouchscreen(newConfig))
		// PLE-384: DeX window resizes and the move between the phone and an external display land
		// here instead of recreating the activity, which would end the session.
		val display = streamDisplay()
		Log.i("StreamActivity", "Configuration changed in place: ${describeWindow(display, newConfig)}")
		if(display.displayId != lastConfigurationDisplayId)
		{
			lastConfigurationDisplayId = display.displayId
			reconfigureDisplayRefreshRateIfMoved(display.displayId)
			updatePresenterDisplayTiming(display)
		}
		binding.root.post(::applyWindowTouchLayout)
	}

	private fun describeWindow(display: Display, configuration: Configuration) =
		"display ${display.displayId} (${"%.2f".format(Locale.US, display.mode.refreshRate)} Hz), " +
			"window ${configuration.screenWidthDp}x${configuration.screenHeightDp} dp, " +
			"${configuration.densityDpi} dpi, touchscreen=${hasTouchscreen(configuration)}"

	override fun onDestroy()
	{
		quitReasonInjector?.let { unregisterReceiver(it) }
		quitReasonInjector = null
		uiVisibilityHandler.removeCallbacks(connectProgressTick)
		diagnosticsOverlay?.destroy()
		diagnosticsOverlay = null
		configureWifiLock(false)
		configurePerformanceMode(false)
		restoreDisplayRefreshRate()
		super.onDestroy()
		controlsJob?.cancel()
		debandRenderer?.let { renderer ->
			// GL teardown must happen on the GL thread, while the context is current
			binding.debandSurfaceView.queueEvent { renderer.releaseGl() }
			renderer.release()
		}
		debandRenderer = null
		eglRenderer?.release()
		eglRenderer = null
	}

	@Suppress("DEPRECATION")
	private fun diagnosticsUiState(preferences: Preferences, connectInfo: ConnectInfo): StreamDiagnosticsUiState
	{
		val display = streamDisplay()
		val mode = display.mode
		val effectiveRefreshRateMode = effectiveDisplayRefreshRateMode(
			preferences.displayRefreshRateMode,
			preferences.realVideoTimestamps,
			preferences.videoPacingEnabled
		)
		val flags = buildList {
			if(connectInfo.decoderLowLatencyEnabled) add("lowlat")
			if(connectInfo.decoderQcomVtLowLatency) add("qcom-vt")
			if(preferences.wifiLowLatencyLockEnabled) add("wifi-lowlat")
			if(preferences.realVideoTimestamps) add("pts")
			if(preferences.decoderInputThreadEnabled) add("in-thread")
			if(connectInfo.decoderLateFrameRecoveryEnabled) add("late-drop")
			if(preferences.videoPacingEnabled)
			{
				add("pacing")
				add(if(preferences.videoPacingHighRefreshEnabled) "hi-hz-on" else "hi-hz-gate")
				if(preferences.videoPacingMode != Preferences.VideoPacingMode.LOWEST_LATENCY
					&& !preferences.videoPacingHighRefreshEnabled && mode.refreshRate >= 119.0f)
					add("immediate-release")
			}
			when(effectiveRefreshRateMode)
			{
				Preferences.DisplayRefreshRateMode.MATCH_STREAM -> add("hz-match")
				Preferences.DisplayRefreshRateMode.HIGHEST -> add("hz-max")
				Preferences.DisplayRefreshRateMode.SYSTEM_DEFAULT -> add("hz-system")
			}
			if(preferences.videoPacingEnabled && preferences.videoPacingBoundedAgeEnabled)
				add("age-${preferences.videoPacingMaxFrameAgePeriods}f")
			if(preferences.videoPacingEnabled
				&& preferences.videoRecoveryStrategy == Preferences.VideoRecoveryStrategy.FLUSH)
				add("flush-rec")
			if(connectInfo.takionVideoPacketReorderingDisabled) add("reorder-off")
			if(connectInfo.idrOnFecFailureEnabled) add("idr-fec")
			if(preferences.feedbackReducedIntervalEnabled) add("fb4ms")
			if(preferences.feedbackStatsLogEnabled) add("fb-log")
			if(connectInfo.threadPriorityBoostEnabled) add("prio")
			addAll(performanceModeDiagnosticFlags(
				sustainedPerformanceModeEnabled,
				viewModel.session.session?.adpfPerformanceModeLive == true
			))
			if(preferences.debandingEnabled) add("deband")
			if(preferences.debandRenderWhenDirtyEnabled) add("dirty")
			if(preferences.streamWindowOptimizationsEnabled) add("window")
			if(preferences.controllerInputCoalescingEnabled) add("input-coal")
			if(preferences.gamepadUnbufferedDispatchEnabled) add("gamepad-unbuf")
		}
		return StreamDiagnosticsUiState(
			display = StreamDiagnosticsDisplay(
				width = mode.physicalWidth,
				height = mode.physicalHeight,
				refreshRate = mode.refreshRate,
				modeId = mode.modeId
			),
			viewMode = streamTransformMode.name.lowercase(Locale.US),
			flags = flags,
			presenterMode = if(preferences.videoPacingEnabled) preferences.videoPacingMode.value else null,
			networkLink = diagnosticsNetworkLink()
		)
	}

	@Suppress("DEPRECATION")
	private fun diagnosticsNetworkLink(): NetworkLinkSample
	{
		val connectivity = getSystemService(ConnectivityManager::class.java)
		val capabilities = connectivity.getNetworkCapabilities(connectivity.activeNetwork)
		if(capabilities?.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) != true)
			return NetworkLinkSample(
				if(capabilities == null) NetworkLinkType.UNKNOWN else NetworkLinkType.OTHER
			)
		return runCatching {
			val info = getSystemService(WifiManager::class.java).connectionInfo
			NetworkLinkSample(
				type = NetworkLinkType.WIFI,
				rssiDbm = info.rssi.takeIf { it in -126..-1 },
				linkSpeedMbps = info.linkSpeed.takeIf { it > 0 }
			)
		}.getOrDefault(NetworkLinkSample(NetworkLinkType.WIFI))
	}

	private fun reconnect()
	{
		viewModel.reconnect()
	}

	/**
	 * PLE-422: delivers an `adb shell am broadcast` payload straight into the live
	 * [fi.madekivi.pleikkari.session.StreamSession] as a [QuitEvent], through the same
	 * [fi.madekivi.pleikkari.session.StreamSession.remoteEvent] entry point the PSN remote-session
	 * path already uses for events that did not originate in this process. From here on it is the
	 * same code a real error takes: [fi.madekivi.pleikkari.session.StreamSession] posts
	 * `StreamStateQuit`, and [stateChanged] shows the real error dialog with Reconnect wired to the
	 * real [reconnect]. There is no shortcut that shows the dialog without also driving the state
	 * machine underneath it.
	 *
	 * Only registered when [BuildConfig.DEBUG]; see `scripts/dev/flag-gate-allowlist.txt` and
	 * `docs/devtools-quit-injection.md`.
	 */
	private fun registerQuitReasonInjector(): BroadcastReceiver
	{
		val receiver = object: BroadcastReceiver()
		{
			override fun onReceive(context: Context, intent: Intent)
			{
				val reasonValue = intent.getIntExtra(EXTRA_QUIT_REASON, -1)
				if(reasonValue < 0)
				{
					Log.w("StreamActivity", "PLE-422 devtools: ignoring broadcast with no $EXTRA_QUIT_REASON")
					return
				}
				val reason = QuitReason(reasonValue)
				Log.w("StreamActivity", "PLE-422 devtools: injecting QuitEvent(reason=$reason, isError=${reason.isError})")
				viewModel.session.remoteEvent(QuitEvent(reason, "PLE-422 devtools injection"))
			}
		}
		ContextCompat.registerReceiver(
			this, receiver, IntentFilter(ACTION_INJECT_QUIT_REASON), ContextCompat.RECEIVER_EXPORTED
		)
		return receiver
	}

	private val hideSystemUIRunnable = Runnable {
		hideOverlay()
		hideSystemUI()
	}

	private fun applyOverlayInsets(insets: WindowInsetsCompat)
	{
		val safe = insets.getInsetsIgnoringVisibility(
			WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout()
		)
		val gestures = insets.getInsets(WindowInsetsCompat.Type.mandatorySystemGestures())
		val baseMargin = (12 * resources.displayMetrics.density).toInt()
		val controlsTop = portraitControlsTop()
		val topMargin = baseMargin + if(controlsTop > 0) controlsTop else maxOf(safe.top, gestures.top)
		val marginEnd = baseMargin + maxOf(safe.right, gestures.right)
		val params = binding.streamControlDock.layoutParams as ViewGroup.MarginLayoutParams
		// PLE-513: updateLayoutParams() always calls requestLayout(), even with unchanged values.
		// applyOverlayInsets() is reached from a root layout-change listener, so an unconditional
		// requestLayout() here re-triggers that listener every frame.
		if(params.topMargin == topMargin && params.marginEnd == marginEnd)
			return
		params.topMargin = topMargin
		params.marginEnd = marginEnd
		// This runs from the root's layout-change listener, i.e. inside a layout pass. A
		// requestLayout() made there is deferred, and dropped outright for a view under a GONE
		// parent - which the dock is whenever the overlay is hidden. The dock then kept a stale
		// 0x0 measure, so the overlay opened with nothing in it and the stream menu (and with it
		// the on-screen controls toggle) could not be reached. Apply the change after the pass.
		binding.streamControlDock.post { binding.streamControlDock.layoutParams = params }
	}

	private fun prepareWindowTouchLayout()
	{
		if(binding.streamTouchpadView.parent === binding.root)
			return
		(binding.streamTouchpadView.parent as ViewGroup).removeView(binding.streamTouchpadView)
		val videoIndex = binding.root.indexOfChild(binding.aspectRatioLayout)
		binding.root.addView(
			binding.streamTouchpadView,
			videoIndex + 1,
			FrameLayout.LayoutParams(
				ViewGroup.LayoutParams.MATCH_PARENT,
				ViewGroup.LayoutParams.MATCH_PARENT
			)
		)
	}

	private fun portraitControlsTop(): Int
	{
		if(binding.root.width <= 0 || binding.root.height <= binding.root.width)
			return 0
		// A tall DeX window has no touchscreen and, unless the user asked for them, no controls to
		// put under the video, so the video stays centred (PLE-384).
		if(!hasTouchscreen(resources.configuration) && viewModel.onScreenControlsEnabled.value != true)
			return 0
		val ratio = binding.aspectRatioLayout.aspectRatio
		return if(ratio > 0f) (binding.root.width / ratio).toInt().coerceAtMost(binding.root.height) else 0
	}

	private fun applyWindowTouchLayout()
	{
		if(binding.root.width <= 0)
			return
		val controlsTop = portraitControlsTop()
		val portrait = controlsTop > 0
		val videoGravity = if(portrait) Gravity.TOP or Gravity.CENTER_HORIZONTAL else Gravity.CENTER
		val videoParams = binding.aspectRatioLayout.layoutParams as FrameLayout.LayoutParams
		if(videoParams.gravity != videoGravity)
		{
			videoParams.gravity = videoGravity
			binding.aspectRatioLayout.layoutParams = videoParams
		}
		updateWindowTouchRegion(binding.streamTouchpadView, controlsTop)
		findViewById<View>(R.id.controlsFragment)?.let { updateWindowTouchRegion(it, controlsTop) }
		touchControlsFragment?.controlsBelowVideo = portrait
		lastWindowInsets?.let(::applyOverlayInsets)
		binding.root.post(::logWindowTouchBounds)
	}

	private fun updateWindowTouchRegion(view: View, top: Int)
	{
		val params = view.layoutParams as FrameLayout.LayoutParams
		if(params.width == ViewGroup.LayoutParams.MATCH_PARENT
			&& params.height == ViewGroup.LayoutParams.MATCH_PARENT
			&& params.topMargin == top
			&& params.bottomMargin == 0
			&& params.gravity == Gravity.TOP)
			return
		params.width = ViewGroup.LayoutParams.MATCH_PARENT
		params.height = ViewGroup.LayoutParams.MATCH_PARENT
		params.topMargin = top
		params.bottomMargin = 0
		params.gravity = Gravity.TOP
		view.layoutParams = params
	}

	private fun logWindowTouchBounds()
	{
		fun View.boundsString(): String
		{
			val bounds = Rect()
			getGlobalVisibleRect(bounds)
			return "[${bounds.left},${bounds.top}][${bounds.right},${bounds.bottom}]"
		}
		val controls = findViewById<View>(R.id.controlsFragment) ?: return
		val message = "PLE-244 bounds orientation=${if(binding.root.height > binding.root.width) "portrait" else "landscape"} " +
			"window=${binding.root.boundsString()} video=${binding.aspectRatioLayout.boundsString()} " +
			"controls=${controls.boundsString()}"
		if(message != lastLayoutBoundsLog)
		{
			lastLayoutBoundsLog = message
			Log.i("StreamActivity", message)
		}
	}

	internal fun showOverlay()
	{
		binding.overlay.animate().setListener(null)
		binding.overlay.animate().cancel()
		binding.overlay.isVisible = true
		binding.overlay.animate()
			.alpha(1.0f)
			.setListener(object: AnimatorListenerAdapter()
			{
				override fun onAnimationEnd(animation: Animator)
				{
					binding.overlay.alpha = 1.0f
				}
			})
		uiVisibilityHandler.removeCallbacks(hideSystemUIRunnable)
		uiVisibilityHandler.postDelayed(hideSystemUIRunnable, HIDE_UI_TIMEOUT_MS)
	}

	private fun hideOverlay()
	{
		if(!binding.overlay.isVisible)
			return
		networkQualityDetailsExpanded = false
		updateNetworkQualityChip()
		binding.overlay.animate().setListener(null)
		binding.overlay.animate().cancel()
		binding.overlay.animate()
			.alpha(0.0f)
			.setListener(object: AnimatorListenerAdapter()
			{
				override fun onAnimationEnd(animation: Animator)
				{
					binding.overlay.isGone = true
				}
			})
	}

	// PLE-371: the network-quality chip is a fixed-height (40dp), wrap-content-width assist chip
	// packed into a 3-button horizontal dock anchored to the screen's top-right corner -- there is
	// no room in it for mode + peer address + MTU alongside the existing RTT/jitter/loss line
	// without the dock's card overflowing off-screen at phone width in portrait. This menu is the
	// ticket's named alternative surface: each fact gets its own row, which wraps and lays out
	// safely regardless of phone width or orientation.
	//
	// PLE-483: a stock android.widget.PopupMenu has no res/layout of its own -- its item height
	// and positioning are fixed by the system's dropdown-list style, and it silently turned
	// scrollable once PLE-473 added a seventh row, with no visible cue that a row was off-screen
	// in landscape (measured: the six-row popup already used 1080 of the ~1089 px available below
	// the anchor in landscape; a seventh row cannot fit there at all). This builds the same rows
	// into a custom PopupWindow over popup_stream_menu.xml instead, so the container can be sized
	// to the space actually available and, if the row count ever outgrows that, scrolls with a
	// scrollbar that never fades rather than clipping invisibly.
	private fun showDisplayModeMenu()
	{
		val anchor = binding.streamMenuButton
		val content = layoutInflater.inflate(R.layout.popup_stream_menu, null)
		val rows = content.findViewById<LinearLayout>(R.id.streamMenuRows)

		val popup = PopupWindow(content, ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, true)
		popup.isOutsideTouchable = true
		popup.isFocusable = true

		val modeRows = mutableListOf<CheckedTextView>()
		fun addToggleRow(labelRes: Int, mode: TransformMode)
		{
			val row = layoutInflater.inflate(R.layout.popup_stream_menu_toggle_row, rows, false) as CheckedTextView
			row.text = getString(labelRes)
			row.isChecked = streamTransformMode == mode
			row.setOnClickListener {
				streamTransformMode = mode
				modeRows.forEach { it.isChecked = false }
				row.isChecked = true
				adjustStreamViewAspect()
				showOverlay()
				popup.dismiss()
			}
			modeRows += row
			rows.addView(row)
		}
		addToggleRow(R.string.stream_display_fit, TransformMode.FIT)
		addToggleRow(R.string.stream_display_zoom, TransformMode.ZOOM)
		addToggleRow(R.string.stream_display_stretch, TransformMode.STRETCH)
		addConnectionInfoRows(rows)

		// Cap the popup at the room actually available below the anchor so the ScrollView inside
		// popup_stream_menu.xml engages instead of the window overflowing past the screen edge.
		content.measure(View.MeasureSpec.UNSPECIFIED, View.MeasureSpec.UNSPECIFIED)
		val anchorLocation = IntArray(2)
		anchor.getLocationOnScreen(anchorLocation)
		val bottomMargin = resources.getDimensionPixelSize(R.dimen.stream_menu_bottom_margin)
		val available = resources.displayMetrics.heightPixels - (anchorLocation[1] + anchor.height) - bottomMargin
		popup.height = minOf(content.measuredHeight, available)

		popup.showAsDropDown(anchor)
		showOverlay()
	}

	// PLE-456: PopupMenu's stock disabled-item styling dimmed the title to the theme's
	// disabled-text colour (measured #616268 on this popup's #121318 background, 3.05:1 --
	// fails WCAG AA's 4.5:1). These rows are read-only, not disabled, so they get an explicit
	// colorOnSurface text colour to match the value text's colour, which already passes.
	private fun addConnectionInfoRows(rows: LinearLayout)
	{
		val readOnlyLabelColor = MaterialColors.getColor(this, com.google.android.material.R.attr.colorOnSurface, Color.WHITE)
		val snapshot = connectionMode
		val lines = connectionInfoLines(snapshot) + listOfNotNull(stallInfoLine())
		for(line in lines)
		{
			val row = layoutInflater.inflate(R.layout.popup_stream_menu_info_row, rows, false) as TextView
			row.text = line
			row.setTextColor(readOnlyLabelColor)
			rows.addView(row)
		}
	}

	private fun connectionInfoLines(snapshot: ConnectionModeSnapshot): List<String>
	{
		val modeLabel = getString(when(snapshot.mode)
		{
			ConnectionMode.DIRECT -> R.string.connection_mode_direct
			ConnectionMode.VPN -> R.string.connection_mode_vpn
			ConnectionMode.RELAY -> R.string.connection_mode_relay
			ConnectionMode.UNKNOWN -> R.string.connection_mode_unknown
		})
		if(snapshot.mode == ConnectionMode.UNKNOWN)
			return listOf(getString(R.string.stream_connection_mode, modeLabel))
		val lines = mutableListOf(
			if(snapshot.peerHost.isBlank())
				getString(R.string.stream_connection_mode, modeLabel)
			else
				getString(R.string.stream_connection_peer, modeLabel, snapshot.peerHost, snapshot.peerPort)
		)
		lines += getString(
			if(snapshot.measured) R.string.stream_connection_mtu_measured else R.string.stream_connection_mtu_fallback,
			snapshot.mtuIn
		)
		lines += getString(
			if(snapshot.measured) R.string.stream_connection_rtt_measured else R.string.stream_connection_rtt_fallback,
			snapshot.rttUs / 1000
		)
		return lines
	}

	// PLE-473: absent (null) on a clean session -- a permanent "0 ms" row would be noise, and
	// the operator's rule is that no feature needs an instruction to be discoverable, so an
	// always-present row reading zero would just be a question mark for a reader who never saw
	// a stall. Present only once a stall has actually happened, and it names how bad and how
	// recent -- see StallInfo.kt for why those two numbers rather than a live or session-worst one.
	private fun stallInfoLine(): String? =
		StallInfoPresenter.display(stallTracker.snapshot(), SystemClock.elapsedRealtime())?.let {
			getString(R.string.stream_connection_stall, it.stallSeconds, StreamSummaryFormatter.duration(it.agoMillis))
		}

	private fun updateNetworkQualityChip()
	{
		val content = NetworkQualityChipPresenter.content(networkQuality, networkQualityDetailsExpanded)
		val label = getString(when(content.level)
		{
			NetworkQualityLevel.GOOD -> R.string.network_quality_good
			NetworkQualityLevel.CONSTRAINED -> R.string.network_quality_fair
			NetworkQualityLevel.POOR -> R.string.network_quality_poor
			NetworkQualityLevel.UNKNOWN -> R.string.network_quality_unknown
		})
		val color = when(content.level)
		{
			NetworkQualityLevel.GOOD -> R.color.stream_quality_good
			NetworkQualityLevel.CONSTRAINED -> R.color.stream_quality_fair
			NetworkQualityLevel.POOR -> R.color.stream_quality_poor
			NetworkQualityLevel.UNKNOWN -> R.color.stream_quality_unknown
		}
		binding.networkQualityChip.chipBackgroundColor = ColorStateList.valueOf(
			ContextCompat.getColor(this, color)
		)
		val foreground = if(content.level == NetworkQualityLevel.CONSTRAINED)
			ContextCompat.getColor(this, android.R.color.black)
		else ContextCompat.getColor(this, R.color.stream_text)
		binding.networkQualityChip.setTextColor(foreground)
		binding.networkQualityChip.chipIconTint = ColorStateList.valueOf(foreground)
		binding.networkQualityChip.text = content.metrics?.let { metrics ->
			getString(
				R.string.stream_quality_details,
				metrics.rttMillis,
				metrics.jitterMillis,
				metrics.lossPercent
			)
		} ?: label
		binding.networkQualityChip.contentDescription = if(content.metrics == null)
			label else "$label, ${binding.networkQualityChip.text}"
	}

	private fun showQuitConfirmation()
	{
		if(dialogContents == UserQuitDialog)
			return
		dialog?.dismiss()
		val confirmation = MaterialAlertDialogBuilder(this)
			.setTitle(R.string.stream_quit_title)
			.setNegativeButton(android.R.string.cancel) { _, _ -> dialog = null }
			.setPositiveButton(R.string.action_quit_session) { _, _ ->
				dialog = null
				finish()
			}
			.setOnCancelListener { dialog = null }
			.create()
		dialogContents = UserQuitDialog
		dialog = confirmation
		confirmation.show()
	}

	/**
	 * PLE-337/PLE-340: the connect overlay. [phase] changing updates the label but never restarts
	 * the bar's clock - only going from no phase to a phase (a fresh attempt) does that; the same
	 * phase repeating, which is exactly what a handoff retry does, and a later phase taking over
	 * both leave [connectSessionStartedAt] alone, so the bar keeps filling across every poll of
	 * the console instead of resetting at each named step.
	 */
	private fun setConnectPhase(phase: ConnectPhase?)
	{
		if(phase != null && connectPhase == null)
			connectSessionStartedAt = SystemClock.elapsedRealtime()
		connectPhase = phase
		uiVisibilityHandler.removeCallbacks(connectProgressTick)
		renderConnectProgress()
		if(phase != null)
			uiVisibilityHandler.postDelayed(connectProgressTick, CONNECT_PROGRESS_TICK_MS)
	}

	private fun renderConnectProgress()
	{
		val phase = connectPhase
		if(phase == null)
		{
			binding.connectingStatusText.visibility = View.GONE
			hideConnectBar(binding.connectingBar, binding.connectingOvertimeText)
			return
		}
		val progress = connectProgress(phase, SystemClock.elapsedRealtime() - connectSessionStartedAt)
		binding.connectingStatusText.visibility = View.VISIBLE
		binding.connectingStatusText.setText(phase.labelRes)
		progress.applyTo(binding.connectingBar, binding.connectingOvertimeText)
	}

	override fun onWindowFocusChanged(hasFocus: Boolean)
	{
		super.onWindowFocusChanged(hasFocus)
		if(hasFocus)
			hideSystemUI()
	}

	private fun hideSystemUI()
	{
		insetsController.hide(WindowInsetsCompat.Type.systemBars())
	}

	private var dialogContents: DialogContents? = null
	private var dialog: AlertDialog? = null
		set(value)
		{
			field = value
			if(value == null)
				dialogContents = null
		}

	private var streamState: StreamState = StreamStateIdle
	private var remoteConnectPhase: ConnectPhase? = null

	/**
	 * PLE-335/PLE-337: a console that has just been linked needs a moment before it accepts the
	 * stream. Name that wait and keep a clock on it, rather than leaving a bare spinner or - as
	 * before - raising "Session has quit". Before the session starts, a PSN remote connect's
	 * control-plane steps are named the same way (see [streamConnectPhase]).
	 */
	private fun updateConnectOverlay()
	{
		val sessionPhase = when(streamState)
		{
			StreamStateLinkedStarting -> ConnectPhase.CONSOLE_NOT_READY
			StreamStateConnecting -> ConnectPhase.STARTING_STREAM
			else -> null
		}
		val phase = streamConnectPhase(sessionPhase, streamState == StreamStateIdle, remoteConnectPhase)
		binding.progressBar.visibility = if(phase != null) View.VISIBLE else View.GONE
		if(phase != connectPhase)
			setConnectPhase(phase)
	}

	private fun stateChanged(state: StreamState)
	{
		streamState = state
		updateConnectOverlay()
		if(state == StreamStateConnected)
			summaryAccumulator.connected(SystemClock.elapsedRealtime())

		when(state)
		{
			is StreamStateQuit ->
			{
				summaryAccumulator.ended(SystemClock.elapsedRealtime())
				quitEndReason = StreamEndReason(state.reason.value, state.reasonString,
					viewModel.session.connectInfo.host, viewModel.session.connectInfo.ps5)
				if(dialogContents != StreamQuitDialog)
				{
					if(state.reason.isError)
					{
						dialog?.dismiss()
						val reasonStr = state.reasonString
						val dialog = MaterialAlertDialogBuilder(this)
							.setMessage(getString(R.string.alert_message_session_quit, state.reason.toString())
									+ (if(reasonStr != null) "\n$reasonStr" else ""))
							.setPositiveButton(R.string.action_reconnect) { _, _ ->
								dialog = null
								reconnect()
							}
							.setOnCancelListener {
								dialog = null
								finish()
							}
							.setNegativeButton(R.string.action_quit_session) { _, _ ->
								dialog = null
								finish()
							}
							.create()
						dialogContents = StreamQuitDialog
						dialog.show()
					}
					else
						finish()
				}
			}

			is StreamStateCreateError ->
			{
				if(dialogContents != CreateErrorDialog)
				{
					dialog?.dismiss()
					val dialog = MaterialAlertDialogBuilder(this)
						.setMessage(getString(R.string.alert_message_session_create_error, state.error.errorCode.toString()))
						.setOnDismissListener {
							dialog = null
							finish()
						}
						.setNegativeButton(R.string.action_quit_session) { _, _ -> }
						.create()
					dialogContents = CreateErrorDialog
					dialog.show()
				}
			}

			is StreamStateRemoteError ->
			{
				if(dialogContents != CreateErrorDialog)
				{
					dialog?.dismiss()
					val dialog = MaterialAlertDialogBuilder(this)
						.setMessage(getString(R.string.alert_message_psn_remote_error, state.message))
						.setOnDismissListener {
							dialog = null
							finish()
						}
						.setNegativeButton(R.string.action_quit_session) { _, _ -> }
						.create()
					dialogContents = CreateErrorDialog
					dialog.show()
				}
			}

			is StreamStateLoginPinRequest ->
			{
				if(dialogContents != PinRequestDialog)
				{
					dialog?.dismiss()

					val view = layoutInflater.inflate(R.layout.dialog_login_pin, null)
					val pinEditText = view.findViewById<EditText>(R.id.pinEditText)

					val dialog = MaterialAlertDialogBuilder(this)
						.setMessage(
							if(state.pinIncorrect)
								R.string.alert_message_login_pin_request_incorrect
							else
								R.string.alert_message_login_pin_request)
						.setView(view)
						.setPositiveButton(R.string.action_login_pin_connect) { _, _ ->
							dialog = null
							viewModel.session.setLoginPin(pinEditText.text.toString())
						}
						.setOnCancelListener {
							dialog = null
							finish()
						}
						.setNegativeButton(R.string.action_quit_session) { _, _ ->
							dialog = null
							finish()
						}
						.create()
					dialogContents = PinRequestDialog
					dialog.show()
				}
			}
			else ->{}
		}
	}

	override fun finish()
	{
		if(!summaryResultSet)
		{
			summaryAccumulator.build(SystemClock.elapsedRealtime())?.let { summary ->
				setResult(RESULT_OK, Intent().putExtra(EXTRA_STREAM_SUMMARY, summary.copy(endReason = quitEndReason)))
				summaryResultSet = true
			}
		}
		super.finish()
	}

	private fun adjustTextureViewAspect(textureView: TextureView)
	{
		val trans = TextureViewTransform(viewModel.session.connectInfo.videoProfile, textureView)
		val resolution = trans.resolutionFor(streamTransformMode)
		Matrix().also {
			textureView.getTransform(it)
			it.setScale(resolution.width / trans.viewWidth, resolution.height / trans.viewHeight)
			it.postTranslate((trans.viewWidth - resolution.width) * 0.5f, (trans.viewHeight - resolution.height) * 0.5f)
			textureView.setTransform(it)
		}
	}

	private fun adjustSurfaceViewAspect()
	{
		val videoProfile = viewModel.session.connectInfo.videoProfile
		binding.aspectRatioLayout.aspectRatio = videoProfile.width.toFloat() / videoProfile.height.toFloat()
		binding.aspectRatioLayout.mode = streamTransformMode
	}

	private fun adjustStreamViewAspect() = adjustSurfaceViewAspect()

	override fun dispatchKeyEvent(event: KeyEvent) = viewModel.input.dispatchKeyEvent(event) || super.dispatchKeyEvent(event)

	override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean
	{
		requestUnbufferedGamepadDispatchIfEnabled(event)
		return super.dispatchGenericMotionEvent(event)
	}

	override fun onGenericMotionEvent(event: MotionEvent) = viewModel.input.onGenericMotionEvent(event) || super.onGenericMotionEvent(event)

	// PLE-91: on the buffered path Android holds a joystick axis change for up to one input-batch
	// interval (8-16 ms) before dispatchGenericMotionEvent sees it; requesting unbuffered dispatch
	// removes that wait. The MotionEvent overload of requestUnbufferedDispatch is documented for
	// touch events only (it has existed since API 21), so joystick/gamepad sources need the
	// source-class overload, which the platform added only in API 30 (confirmed against
	// android-35's api-versions.xml, not the API 26/31 levels quoted in earlier notes).
	private fun requestUnbufferedGamepadDispatchIfEnabled(event: MotionEvent)
	{
		val shouldRequest = shouldRequestUnbufferedGamepadDispatch(
			source = event.source,
			sdkInt = Build.VERSION.SDK_INT,
			enabled = Preferences(this).gamepadUnbufferedDispatchEnabled
		)
		if(shouldRequest)
			window.decorView.requestUnbufferedDispatch(InputDevice.SOURCE_CLASS_JOYSTICK)
	}
}

enum class TransformMode
{
	FIT,
	STRETCH,
	ZOOM
}

class TextureViewTransform(private val videoProfile: ConnectVideoProfile, private val textureView: TextureView)
{
	private val contentWidth : Float get() = videoProfile.width.toFloat()
	private val contentHeight : Float get() = videoProfile.height.toFloat()
	val viewWidth : Float get() = textureView.width.toFloat()
	val viewHeight : Float get() = textureView.height.toFloat()
	private val contentAspect : Float get() =  contentHeight / contentWidth

	fun resolutionFor(mode: TransformMode): Resolution
		= when(mode)
		{
			TransformMode.STRETCH -> strechedResolution
			TransformMode.ZOOM -> zoomedResolution
			TransformMode.FIT -> normalResolution
		}

	private val strechedResolution get() = Resolution(viewWidth, viewHeight)

	private val zoomedResolution get() =
		if(viewHeight > viewWidth * contentAspect)
		{
			val zoomFactor = viewHeight / contentHeight
			Resolution(contentWidth * zoomFactor, viewHeight)
		}
		else
		{
			val zoomFactor = viewWidth / contentWidth
			Resolution(viewWidth, contentHeight * zoomFactor)
		}

	private val normalResolution get() =
		if(viewHeight > viewWidth * contentAspect)
			Resolution(viewWidth, viewWidth * contentAspect)
		else
			Resolution(viewHeight / contentAspect, viewHeight)
}


data class Resolution(val width: Float, val height: Float)
