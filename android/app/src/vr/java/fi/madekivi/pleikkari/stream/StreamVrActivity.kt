// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
package fi.madekivi.pleikkari.stream

import android.content.ActivityNotFoundException
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.graphics.*
import android.opengl.GLES20
import android.opengl.GLUtils
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.os.Process
import android.os.SystemClock
import android.util.Log
import android.view.*
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.core.content.ContextCompat
import androidx.core.content.IntentCompat
import androidx.core.os.BundleCompat
import androidx.lifecycle.ViewModelProvider
import fi.madekivi.pleikkari.BuildConfig
import fi.madekivi.pleikkari.R
import fi.madekivi.pleikkari.common.Preferences
import fi.madekivi.pleikkari.common.ext.viewModelFactory
import fi.madekivi.pleikkari.lib.CinemaFrameLatency
import fi.madekivi.pleikkari.lib.ConnectInfo
import fi.madekivi.pleikkari.lib.ConnectVideoProfile
import fi.madekivi.pleikkari.lib.ControllerState
import fi.madekivi.pleikkari.lib.LatencyProbe
import fi.madekivi.pleikkari.remote.PsnDevice
import fi.madekivi.pleikkari.session.*
import fi.madekivi.pleikkari.stream.vrui.*
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone
import java.util.concurrent.CountDownLatch
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger

/** VrApi owns the display; Android's SurfaceView is only the native-window handoff. */
class StreamVrActivity : ComponentActivity(), SurfaceHolder.Callback {
    private var model: StreamViewModel? = null
    private var cinema: CinemaThread? = null
    private var windowSurface: Surface? = null
    private var resumed = false
    private var failed = false
    private var statusText = ""
    private val main = Handler(Looper.getMainLooper())
    private var preview = false
    /** With the preview, the room its extra asked for, until the VR menu picks another (PLE-722). */
    @Volatile private var previewEnvironment: String? = null
    private var environmentSamples = DEFAULT_ENVIRONMENT_SAMPLES
    /** PLE-690: started by the Go Library through the exported alias (or back in its chooser), not by Connect. */
    private var library = false
    private var libraryFlow: GoVrLibraryFlow? = null
    /** PLE-690: a Library launch before its stream; touchpad clicks drive the chooser, not recentre. */
    @Volatile private var picking = false
    /** PLE-739: debug builds only; VrApi input bits fed by [DEBUG_INPUT_ACTION], OR'd into the next poll. */
    private val debugInput = AtomicInteger(0)
    private var debugInputReceiver: BroadcastReceiver? = null
    /** PLE-722: when the remote's Back went down, as Android saw it (a long press recentres). */
    private var backDownMs = 0L
    /** PLE-730: VR Home's page (the Library flow's, then the connecting sheet); null once the stream shows. */
    private var homeState: VrHomeState? = null
    /** PLE-730: the console a Library launch connects to, for the connecting sheet's title. */
    private var consoleName: String? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val entry = GoVrSupport.isLibraryEntry(intent)
        // PLE-690: anyone can start the exported alias, so a Library launch drops every extra before
        // anything reads one; it only takes back the session it saved itself before being recreated.
        if(entry) intent.replaceExtras(null as Bundle?)
        // PLE-690: a Library launch's Disconnect restarts this (unexported) activity in its chooser.
        val chooser = !entry && intent.getBooleanExtra(EXTRA_LIBRARY_CHOOSER, false)
        library = entry || chooser
        val info = if(library) savedInstanceState?.let { BundleCompat.getParcelable(it, STATE_CONNECT_INFO, ConnectInfo::class.java) }
            else IntentCompat.getParcelableExtra(intent, StreamActivity.EXTRA_CONNECT_INFO, ConnectInfo::class.java)
        // PLE-623: debug builds only. The real VrApi cinema with no console and a synthetic
        // picture, so the Go's cinema and environment cost can be read from adb (README).
        preview = BuildConfig.DEBUG && intent.getBooleanExtra(EXTRA_VR_CINEMA_PREVIEW, false)
        previewEnvironment = if(preview) intent.getStringExtra(EXTRA_ENVIRONMENT) else null
        if(preview) environmentSamples = intent.getIntExtra(EXTRA_ENVIRONMENT_MSAA, DEFAULT_ENVIRONMENT_SAMPLES)
        if(library) {
            // The referrer names the app that started us (com.oculus.vrshell for the Library).
            Log.i(TAG_ENTRY, (if(chooser) "Back in the Library chooser" else "Library VR launch, referrer ${referrer ?: "none"}") +
                if(info != null) ", resuming its session" else "")
            if(!GoVrSupport.available() || !Preferences(this).goVrEnabled) {
                // A stale entry (the setting is off) or a build whose VR libraries do not load.
                GoVrSupport.syncLibraryEntry(this)
                openPanel()
                return
            }
        }
        if(!GoVrSupport.available() || !preview && !library && (!Preferences(this).goVrEnabled || info == null)) {
            finish()
            return
        }
        if(!preview && info != null) createModel(info)
        else if(library) {
            picking = true
            // PLE-690: debug builds only, `adb shell setprop debug.pleikkari.go_entry_choose 1`: a Library
            // launch opens its chooser instead of streaming on its own, so the entry can be proven on the
            // Go without connecting to a console (third_party/ovr_sdk_mobile/README.md).
            val chooseFirst = chooser || BuildConfig.DEBUG && debugProperty(CHOOSE_PROPERTY) == "1"
            // PLE-739: debug builds only, `adb shell setprop debug.pleikkari.go_entry_no_console 1`: the
            // Library flow sees no linked PS5, so its no-console message shows without clearing app data.
            val noConsoles = BuildConfig.DEBUG && debugProperty(NO_CONSOLE_PROPERTY) == "1"
            // PLE-784: debug builds only, `adb shell setprop debug.pleikkari.go_entry_not_found 1`: the
            // first console looked for lands on Home's not-found sheet, so the address pad can be driven headless.
            val notFound = BuildConfig.DEBUG && debugProperty(NOT_FOUND_PROPERTY) == "1"
            libraryFlow = GoVrLibraryFlow(this, chooseFirst, noConsoles, notFound, ::showStatus, ::setHome, ::libraryConnect,
                ::openPanel, ::finish).also { it.start() }
        }
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_FULLSCREEN or
            View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
        setContentView(SurfaceView(this).apply { holder.addCallback(this@StreamVrActivity) })
    }

    private fun createModel(info: ConnectInfo) {
        val device = IntentCompat.getParcelableExtra(intent, StreamActivity.EXTRA_PSN_DEVICE, PsnDevice::class.java)
        val justLinked = intent.getBooleanExtra(StreamActivity.EXTRA_JUST_LINKED, false)
        model = ViewModelProvider(this, viewModelFactory {
            StreamViewModel(application, info, device, justLinked = justLinked)
        })[StreamViewModel::class.java].also { vm ->
            vm.input.observe(this)
            vm.session.state.observe(this) { state ->
                statusText = when(state) {
                    StreamStateConnected -> ""
                    is StreamStateQuit, is StreamStateCreateError, is StreamStateRemoteError -> getString(R.string.go_vr_disconnected)
                    // PLE-731: with the VR UI the PIN pad takes it; without, the old pointer to Oculus TV.
                    is StreamStateLoginPinRequest ->
                        getString(if(Preferences(this).goVrUi) R.string.go_vr_pin_pad else R.string.go_vr_pin)
                    else -> getString(R.string.go_vr_connecting)
                }
                cinema?.status = statusText
                // PLE-730: VR Home's connecting sheet until the stream shows. A PIN, a quit or an error
                // keeps today's text on the strip.
                setHome(when(state) {
                    StreamStateIdle, StreamStateConnecting, StreamStateLinkedStarting -> connectingSheet()
                    else -> null
                })
                pinRequest = state as? StreamStateLoginPinRequest
                updatePinPad()
            }
            // PLE-722: the VR menu's stats overlay, once a second while it is on.
            vm.session.streamStats.observe(this) { stats ->
                val thread = cinema ?: return@observe
                val ui = thread.ui ?: return@observe
                if(ui.statsOpen) ui.stats(VrStatsPanel.lines(stats, thread.refreshHz, false))
            }
        }
    }

    private fun showStatus(text: String) {
        statusText = text
        cinema?.status = text
    }

    /** PLE-730: VR Home's page, for the running VR UI and any the next cinema start makes. */
    private fun setHome(state: VrHomeState?) {
        homeState = state
        cinema?.ui?.home(state)
    }

    private fun connectingSheet(): VrHomeState {
        val name = consoleName
        return VrHomeState.Status(name ?: getString(R.string.go_vr_home_title),
            if(name != null) getString(R.string.go_vr_home_connecting, name) else getString(R.string.go_vr_connecting),
            emptyList(), busy = true, buttons = listOf(VrHomeButton(getString(R.string.go_vr_home_cancel), VrHomeAction.Cancel)),
            back = VrHomeAction.Cancel)
    }

    /**
     * PLE-730: a VR Home button, from the GoVrUi thread. Before the stream the Library flow acts on
     * it; Cancel while a stream connects leaves as Disconnect does.
     */
    private fun homeAction(action: VrHomeAction) = main.post {
        if(isFinishing) return@post
        Log.i(TAG_ENTRY, "Home: $action")
        val flow = libraryFlow
        when(action) {
            is VrHomeAction.Play -> flow?.play(action.id)
            VrHomeAction.Cancel -> if(flow != null) flow.cancel() else leave()
            VrHomeAction.Retry -> flow?.retry()
            VrHomeAction.Consoles -> flow?.consoles()
            VrHomeAction.Address -> flow?.let(::showAddressPad)
            VrHomeAction.OculusTv -> openPanel()
            // PLE-732's Go Settings sheet, on the menu panel over Home.
            VrHomeAction.Settings -> cinema?.ui?.openSettings("Home Settings")
            VrHomeAction.Exit -> finish()
        }
    }

    /** PLE-690: the Library flow picked and found a console; stream it into the running cinema. */
    private fun libraryConnect(info: ConnectInfo, name: String) {
        picking = false
        libraryFlow?.stop()
        libraryFlow = null
        consoleName = name
        createModel(info)
        attachSession()
    }

    /** PLE-690: the 2D app on the Oculus TV screen, for linking a console and Settings; this VR task ends. */
    private fun openPanel() {
        Log.i(TAG_ENTRY, "Opening Pleikkari on the Oculus TV screen")
        try {
            startActivity(GoVrSupport.panelIntent(this))
        } catch(error: ActivityNotFoundException) {
            Log.e(TAG_ENTRY, "No vrshell to host the 2D app", error)
        } catch(error: SecurityException) {
            Log.e(TAG_ENTRY, "vrshell refused to host the 2D app", error)
        }
        finish()
    }

    /**
     * The menu's right third. Before a Library launch streams it exits; a Library launch's Disconnect
     * returns to its chooser, whose last row opens the 2D app for linking and Settings (PLE-690).
     */
    private fun leave() {
        if(library && !picking) {
            Log.i(TAG_ENTRY, "Disconnect: back to the Library chooser")
            startActivity(Intent().setClassName(this, GoVrSupport.ACTIVITY)
                .putExtra(EXTRA_LIBRARY_CHOOSER, true)
                .addFlags(Intent.FLAG_ACTIVITY_NO_ANIMATION))
        }
        finish()
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        // PLE-690: a recreated Library launch keeps its session instead of choosing again.
        if(library) model?.connectInfo?.let { outState.putParcelable(STATE_CONNECT_INFO, it) }
    }

    override fun onResume() {
        super.onResume()
        resumed = true
        libraryFlow?.resume()
        if(BuildConfig.DEBUG) registerDebugInput()
        startCinema()
    }

    /**
     * PLE-739: debug builds only. The Go's `input keyevent` has no source argument, so neither the
     * chooser nor the VrApi-polled menu can be driven over adb. This feeds the Go remote's own input
     * bits into the render loop, the same path as the touchpad and Back button:
     * `adb shell am broadcast -a fi.madekivi.pleikkari.DEBUG_GO_VR_INPUT --es key <back|left|centre|right>`.
     * Back toggles the menu; Disconnect is back then right.
     *
     * PLE-746: `--es pad <cross|circle|square|triangle|up|down|left|right|l1|r1|options|ps> [--ei hold_ms N]`
     * instead presses that DualSense button on the streamed console for N ms (default 120). The Go's
     * `input keyevent` cannot reach the D-pad (a pad sends it as HAT motion), so this is how a script
     * parks the PS5 on a Settings toggle. It reaches the console: use it only in PS5 Settings.
     */
    private fun registerDebugInput() {
        if(debugInputReceiver != null) return
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context, intent: Intent) {
                intent.getStringExtra(EXTRA_DEBUG_PAD)?.let { pad ->
                    val button = DEBUG_PAD_BUTTONS[pad]
                    val input = model?.input
                    if(button == null || input == null) {
                        Log.w(TAG_ENTRY, "Debug pad: ${if(button == null) "unknown button" else "no session for"} $pad")
                        return
                    }
                    val holdMs = intent.getIntExtra(EXTRA_DEBUG_HOLD_MS, 120).toLong().coerceIn(20L, 2000L)
                    Log.i(TAG_ENTRY, "Debug pad: $pad for $holdMs ms")
                    input.debugPress(button, holdMs)
                    return
                }
                val key = intent.getStringExtra(EXTRA_DEBUG_KEY)
                val bits = when(key) {
                    "back" -> MENU
                    "left" -> CLICK
                    "centre", "center" -> CLICK or CENTRE
                    "right" -> CLICK or RIGHT
                    else -> {
                        Log.w(TAG_ENTRY, "Debug input: unknown key $key")
                        return
                    }
                }
                Log.i(TAG_ENTRY, "Debug input: $key")
                debugInput.accumulateAndGet(bits) { a, b -> a or b }
            }
        }
        ContextCompat.registerReceiver(this, receiver, IntentFilter(DEBUG_INPUT_ACTION), ContextCompat.RECEIVER_EXPORTED)
        debugInputReceiver = receiver
    }

    override fun onPause() {
        resumed = false
        libraryFlow?.pause()
        debugInputReceiver?.let { unregisterReceiver(it) }
        debugInputReceiver = null
        stopCinema()
        super.onPause()
    }

    override fun onDestroy() {
        libraryFlow?.stop()
        libraryFlow = null
        super.onDestroy()
    }

    override fun surfaceCreated(holder: SurfaceHolder) = Unit
    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        stopCinema()
        windowSurface = holder.surface
        startCinema()
    }
    override fun surfaceDestroyed(holder: SurfaceHolder) {
        stopCinema() // LeaveVrMode must finish before returning the window to Android.
        windowSurface = null
    }

    private fun startCinema() {
        val vm = model
        if(vm == null && !preview && !library) return
        val surface = windowSurface?.takeIf { it.isValid } ?: return
        if(!resumed || failed || cinema != null) return
        // PLE-690: a Library launch enters the cinema before it has a session; it will stream the
        // profile its ConnectInfo is built from.
        val profile = vm?.connectInfo?.videoProfile ?: if(library) Preferences(this).videoProfile else null
        // PLE-698: per-frame latency is measurement, so only with the stats log on. The preview and a
        // Library launch have no session yet; a Library stream's ConnectInfo takes it from the same setting.
        val frameLatency = vm?.connectInfo?.let { it.feedbackStatsLogIntervalMs > 0 } ?: Preferences(this).feedbackStatsLogEnabled
        // PLE-722: the VR UI toolkit's menu unless the switch keeps PLE-602's strip menu.
        val prefs = Preferences(this)
        val ui = if(prefs.goVrUi) createUi(prefs) else null
        ui?.home(homeState)
        // PLE-746: the input-to-photon probe rides on the same frame join, so it needs the stats log too.
        val latencyProbe = prefs.goVrLatencyProbe
        if(latencyProbe && !frameLatency) Log.w("GoCinema", "Latency probe needs stream_feedback_stats_log; it stays off")
        cinema = CinemaThread(surface, profile, frameLatency, latencyProbe && frameLatency, ui).also { it.status = statusText; it.start() }
        pinPadShown = null
        updatePinPad()
    }

    /** PLE-731: the session's pending login PIN request, if any. */
    private var pinRequest: StreamStateLoginPinRequest? = null
    /** PLE-731: the request the current cinema's PIN pad was shown for. */
    private var pinPadShown: StreamStateLoginPinRequest? = null

    /** PLE-731: shows the VR PIN pad for a login PIN request, and takes it down once the session moves on. */
    private fun updatePinPad() {
        val host = cinema?.ui ?: return
        val request = pinRequest
        if(request === pinPadShown) return
        pinPadShown = request
        if(request == null) {
            host.closeModal("PIN request over")
            return
        }
        val text = VrPinPadText(
            title = getString(R.string.alert_message_login_pin_request).trimEnd(':'),
            titleIncorrect = getString(R.string.go_vr_pin_incorrect),
            connect = getString(R.string.action_login_pin_connect),
            quit = getString(R.string.action_quit_session),
            clear = getString(R.string.go_vr_pin_clear),
            backspace = "\u232B")
        val model = object : VrPinPadModel {
            // Both run on the GoVrUi thread.
            override fun submit(pin: String) {
                Log.i(VrUiHost.TAG, "PIN pad: submitting ${pin.length} digits")
                host.closeModal("PIN submitted")
                main.post { this@StreamVrActivity.model?.session?.setLoginPin(pin) }
            }
            override fun quit() { main.post { if(cinema?.ui === host) finish() } }
        }
        val pad = VrPinPad(model, text, request.pinIncorrect)
        host.showModal(pad.screen, pad::refresh, model::quit)
    }

    /** PLE-733: the address pad over Home's not-found sheet; Save stores it and looks for the console again. */
    private fun showAddressPad(flow: GoVrLibraryFlow) {
        val host = cinema?.ui ?: return
        val initial = flow.address() ?: return
        val text = VrAddressPadText(
            title = getString(R.string.go_vr_address_title),
            save = getString(R.string.go_vr_address_save),
            cancel = getString(R.string.go_vr_home_cancel),
            clear = getString(R.string.go_vr_pin_clear),
            backspace = "\u232B",
            letters = "abc",
            digits = "123")
        val model = object : VrAddressPadModel {
            // Both run on the GoVrUi thread.
            override fun save(address: String) {
                host.closeModal("address saved")
                main.post { if(libraryFlow === flow) flow.saveAddress(address) }
            }
            override fun cancel() { host.closeModal("address cancelled") }
        }
        val pad = VrAddressPad(model, text, initial)
        host.showModal(pad.screen, pad::refresh, model::cancel)
    }

    /** The stored room and screen, or with the preview the room its extra asked for. */
    private fun storedEnvironment(): VrEnvironmentConfig {
        val stored = Preferences(this).vrEnvironmentConfig()
        return previewEnvironment?.let { stored.copy(environment = VrEnvironmentKind.fromValue(it)) } ?: stored
    }

    /** PLE-722: the in-stream VR menu's host; its GoVrUi thread lives as long as one cinema. */
    private fun createUi(prefs: Preferences): VrUiHost {
        val model = MenuModel()
        val text = VrMenuText(
            title = getString(R.string.go_vr_menu),
            resume = getString(R.string.go_vr_resume),
            recentre = getString(R.string.go_vr_recentre),
            disconnect = getString(R.string.go_vr_disconnect),
            exit = getString(R.string.go_vr_exit),
            settings = getString(R.string.go_vr_ui_settings),
            room = getString(R.string.go_vr_ui_room),
            rooms = VrEnvironmentKind.values().associateWith {
                if(it == VrEnvironmentKind.PLAIN) getString(R.string.go_vr_ui_room_plain) else getString(it.title)
            },
            distance = getString(R.string.go_vr_ui_distance),
            size = getString(R.string.go_vr_ui_size),
            roomsOnly = getString(R.string.go_vr_ui_rooms_only),
            match60Hz = getString(R.string.go_vr_ui_match_60hz),
            match60HzDetail = getString(R.string.go_vr_ui_match_60hz_detail),
            statsOverlay = getString(R.string.go_vr_ui_stats))
        // Debug builds only: `adb shell setprop debug.pleikkari.vr_pointer x,y` aims a synthetic ray x degrees
        // right and y up of the open menu's middle, so screencaps can show hover and press (third_party README).
        val debugPointer: (() -> String)? = if(BuildConfig.DEBUG) { { debugProperty(POINTER_PROPERTY) } } else null
        // PLE-732: the Go Settings sheet, the menu panel's second page.
        val settingsText = VrSettingsText(
            title = getString(R.string.go_vr_ui_settings),
            back = getString(R.string.go_vr_ui_back),
            oculusTv = getString(R.string.go_vr_ui_oculus_tv),
            stream = getString(R.string.go_vr_ui_stream),
            nextStream = getString(R.string.go_vr_ui_match_60hz_detail),
            resolutions = Preferences.Resolution.values().associateWith { getString(it.title) },
            fpsValues = Preferences.FPS.values().associateWith { getString(it.title) },
            bitrate = getString(R.string.go_vr_ui_bitrate),
            bitrateAuto = getString(R.string.go_vr_ui_bitrate_auto),
            codecs = Preferences.Codec.values().associateWith { getString(it.title) })
        val host = VrUiHost(this,
            { page -> if(page == VrPage.SETTINGS) VrSettings.build(model, model, text, settingsText) else VrMenu.build(model, text) },
            { page, screen -> if(page == VrPage.SETTINGS) VrSettings.refresh(screen, model) else VrMenu.refresh(screen, model) },
            ::menuChanged,
            prefs.mappingShare, prefs.mappingOptions, debugPointer, ::homeAction)
        model.host = host
        host.showStats(prefs.streamDiagnosticsOverlayEnabled)
        return host
    }

    /** PLE-722: while the VR menu is open the pad drives it, so the console sees it at rest. */
    private fun menuChanged(open: Boolean) {
        if(open) model?.input?.releasePad()
    }

    /** PLE-722: the VR menu's view of the stored Go settings; its setters run on the GoVrUi thread. */
    private inner class MenuModel : VrMenuModel, VrStreamProfileModel {
        @Volatile var host: VrUiHost? = null
        private val prefs = Preferences(this@StreamVrActivity)
        override val leaveIsExit get() = picking
        override fun resume() { host?.close("resume") }
        override fun recentre() { host?.recentre() }
        override fun leave() { main.post { if(cinema?.ui === host) this@StreamVrActivity.leave() } }
        override fun openSettings() { host?.showPage(VrPage.SETTINGS) }
        // PLE-732: the Settings sheet's stream profile, stored for the next stream.
        override fun back() { host?.showPage(VrPage.MENU) }
        override fun openOculusTv() { main.post { if(cinema?.ui === host) openPanel() } }
        override var resolution: Preferences.Resolution
            get() = prefs.resolution
            set(value) { prefs.resolution = value }
        override var fps: Preferences.FPS
            get() = prefs.fps
            set(value) { prefs.fps = value }
        override var bitrateKbps: Int
            get() = prefs.bitrate ?: 0
            set(value) { prefs.bitrate = if(value <= 0) null else value }
        override val bitrateAutoKbps get() = prefs.bitrateAuto
        override var codec: Preferences.Codec
            get() = prefs.codec
            set(value) { prefs.codec = value }
        override var room: VrEnvironmentKind
            get() = previewEnvironment?.let { VrEnvironmentKind.fromValue(it) } ?: prefs.vrEnvironment
            set(value) {
                previewEnvironment = null
                prefs.vrEnvironment = value
                host?.requestEnvironment()
            }
        override var screenDistanceCm: Int
            get() = prefs.vrScreenDistanceCm
            set(value) {
                // A screen curved around the viewer stays curved around the viewer.
                if(prefs.vrScreenCurveRadiusCm == prefs.vrScreenDistanceCm) prefs.vrScreenCurveRadiusCm = value
                prefs.vrScreenDistanceCm = value
                host?.requestEnvironment()
            }
        override var screenWidthCm: Int
            get() = prefs.vrScreenWidthCm
            set(value) {
                prefs.vrScreenWidthCm = value
                host?.requestEnvironment()
            }
        override var match60Hz: Boolean
            get() = prefs.goVrMatch60Hz
            set(value) { prefs.goVrMatch60Hz = value }
        override var statsOverlay: Boolean
            get() = prefs.streamDiagnosticsOverlayEnabled
            set(value) {
                prefs.streamDiagnosticsOverlayEnabled = value
                host?.showStats(value)
            }
    }

    /** Hands the cinema's decoder surface to the session: when the cinema starts, or when a Library launch connects. */
    private fun attachSession() {
        val thread = cinema ?: return
        val output = thread.output ?: return
        val vm = model ?: return
        if(!thread.running.get() || !resumed) return
        vm.session.attachToSurface(output)
        vm.session.updateDisplayTiming(thread.refreshHz.toDouble(), 0L) // PLE-654: the panel's real rate
        vm.resume()
    }

    private fun stopCinema() {
        val thread = cinema ?: return
        thread.ui?.close("cinema stopping")
        // Stop the producer and detach before releasing its SurfaceTexture on the GL thread.
        thread.running.set(false)
        model?.pause()
        model?.session?.detachSurface()
        thread.detached.countDown()
        thread.join()
        cinema = null
    }

    private fun cinemaFailed(error: Throwable) {
        Log.e("GoCinema", "Cannot start/continue VR cinema", error)
        failed = true
        stopCinema()
        // PLE-690: a Library launch has no Oculus TV task behind it; open the 2D app there instead,
        // so a cinema that cannot start never locks the user out of Settings.
        if(library) return openPanel()
        Toast.makeText(this, R.string.go_vr_failed, Toast.LENGTH_LONG).show()
        finish() // Return to the existing Oculus TV task; never relaunch onto display 0.
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        // PLE-746: the key's dispatch in the app, for the probe's atrace capture.
        if(!LatencyProbe.active) return dispatchKey(event)
        android.os.Trace.beginSection("PLE746 input dispatchKeyEvent")
        try {
            return dispatchKey(event)
        } finally {
            android.os.Trace.endSection()
        }
    }

    private fun dispatchKey(event: KeyEvent): Boolean {
        // Bluetooth pads retain the existing StreamInput mapping, including their Back key.
        val gamepad = event.isFromSource(InputDevice.SOURCE_GAMEPAD) || event.isFromSource(InputDevice.SOURCE_JOYSTICK)
        val ui = cinema?.ui
        if(!gamepad && event.keyCode == KeyEvent.KEYCODE_BACK) {
            // The Go remote is polled by VrApi. Do not toggle twice if Android also
            // delivers its Back event; its press edge will open the cinema menu.
            // PLE-722: VrApi reports only short presses; a long one reaching Android recentres (§10.6).
            // PLE-773: the Go stamps the remote's Back with a foreign timebase (held logged ~-27,700,000 ms), so time it here.
            if(ui != null) {
                if(event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0) backDownMs = SystemClock.uptimeMillis()
                if(event.action == KeyEvent.ACTION_UP && backDownMs > 0) {
                    val held = SystemClock.uptimeMillis() - backDownMs
                    backDownMs = 0
                    Log.i(VrUiHost.TAG, "Remote Back held $held ms")
                    if(held >= LONG_BACK_MS) ui.recentre()
                }
            }
            return true
        }
        // PLE-722: the VR menu takes the pad while it is open, and opens on Menu or Share+Options held.
        if(ui?.key(event) == true) return true
        // PLE-690: before a Library launch's stream the pad drives the chooser; no session has its keys yet.
        if(gamepad && libraryFlow?.key(event) == true) return true
        return model?.input?.dispatchKeyEvent(event) == true || super.dispatchKeyEvent(event)
    }

    override fun onGenericMotionEvent(event: MotionEvent): Boolean =
        cinema?.ui?.motion(event) == true || model?.input?.onGenericMotionEvent(event) == true ||
            super.onGenericMotionEvent(event)

    /** All EGL, VrApi and SurfaceTexture consumer calls are confined to this thread. */
    private inner class CinemaThread(private val surface: Surface, private val videoProfile: ConnectVideoProfile?,
                                     private val frameLatency: Boolean, private val latencyProbe: Boolean,
                                     ui: VrUiHost?) : Thread("GoCinema") {
        /** PLE-722: the VR menu, or null for PLE-602's strip menu (or when its panels cannot be made). */
        @Volatile var ui: VrUiHost? = ui
        val running = AtomicBoolean(true)
        val detached = CountDownLatch(1)
        @Volatile var status = ""
        // PLE-636: 60 Hz only when the setting is on and the stream is 60 fps. PLE-698: the
        // preview stands in for a 60 fps stream, so it can time both panel rates.
        private val streamFps = if(preview) 60 else videoProfile?.maxFPS
        val refreshHz = if(Preferences(this@StreamVrActivity).goVrMatch60Hz && streamFps == 60) 60f else 72f
        /** The decoder's target, once the GL thread has made it; read on the main thread. */
        @Volatile var output: Surface? = null

        override fun run() {
            var native = 0L
            var texture: SurfaceTexture? = null
            var decoder: Surface? = null
            var listenerThread: HandlerThread? = null
            var probe = false
            // PLE-698: [frameLatency] times each frame (arrival, decode, latch, submit, predicted photon)
            // for the stats log's "Cinema latency" line. The debug preview has no decoder: its frames
            // time the latch, submit and predicted photon only.
            try {
                native = VrCinemaNative.create(this@StreamVrActivity, surface, refreshHz, environmentSamples)
                check(native != 0L) { "VrApi/EGL initialization or $refreshHz Hz request failed (see GoCinema log)" }
                // PLE-675: debug builds only, `adb shell setprop debug.pleikkari.vr_full_pose 1` before
                // the stream starts; the screen follows the full head pose so a Go on a table shows it.
                if(BuildConfig.DEBUG && debugProperty(FULL_POSE_PROPERTY) == "1") VrCinemaNative.setFullPoseRecentre(native, true)
                // PLE-603: the room around the screen; "plain" (the default) leaves the native path as it was.
                // PLE-652: A/B a higher GPU clock while a room is drawn; off keeps GPU level 2.
                if(Preferences(this@StreamVrActivity).goVrRoomHighGpu) VrCinemaNative.setRoomGpuLevel(native, 4)
                val environment = storedEnvironment()
                // PLE-666: with the preview, `--ei sky_variant N` draws a PLE-650 debug sky in the VrApi cinema.
                if(preview && intent.hasExtra(EXTRA_SKY_VARIANT))
                    VrCinemaNative.debugSetSkyVariant(native, intent.getIntExtra(EXTRA_SKY_VARIANT, 0))
                applyEnvironment(native, environment)
                ui?.let { host ->
                    // PLE-761: debug builds only, `adb shell setprop debug.pleikkari.vr_ui_layers <spec>` before
                    // the stream starts; PLE-735's panel layer switches (VrUiLayerDebug).
                    val layerDebug = if(BuildConfig.DEBUG) VrUiLayerDebug.parse(debugProperty(UI_LAYERS_PROPERTY)) else VrUiLayerDebug.DEFAULT
                    if(!layerDebug.isDefault) {
                        Log.i("GoCinema", "VR UI layer debug: $layerDebug")
                        VrCinemaNative.debugSetUiLayers(native, layerDebug.quad, layerDebug.overlay, layerDebug.texelScale,
                            layerDebug.filterExpensive, layerDebug.maxPanels, layerDebug.reticleLayer)
                        host.texelScale = layerDebug.texelScale
                    }
                    // PLE-722: one Canvas-drawn Surface per panel, latched by the compositor itself.
                    val menuSurface = VrCinemaNative.createPanel(native, VrUiHost.MENU, VrMenu.WIDTH, VrMenu.HEIGHT,
                        VrUi.BORDER, VrUi.PANEL_CORNER, ANCHOR_GAZE)
                    val statsSurface = VrCinemaNative.createPanel(native, VrUiHost.STATS, VrStatsPanel.WIDTH, VrStatsPanel.HEIGHT,
                        VrUi.BORDER, VrUi.PANEL_CORNER, ANCHOR_PICTURE)
                    if(menuSurface == null) {
                        Log.e("GoCinema", "No VR UI panel; keeping the strip menu")
                        ui = null
                        host.shutdown()
                    } else {
                        host.attach(menuSurface, statsSurface)
                        host.stats(VrStatsPanel.lines(null, refreshHz, preview))
                    }
                }
                // PLE-715: when each frame starts relative to VrApi's release. The late start is an A/B
                // setting; a debug build also takes `adb shell setprop debug.pleikkari.vr_pacing <experiments>`
                // (vr-frame-pacing.h). The per-second "Frame pacing" line comes with the stats log.
                val pacingSpec = if(BuildConfig.DEBUG) debugProperty(PACING_PROPERTY) else ""
                VrCinemaNative.setPacing(native, if(Preferences(this@StreamVrActivity).goVrLateStart) 1 else 0,
                    frameLatency, pacingSpec, refreshHz)
                // PLE-755: pay the first-draw cost before the first submit, not inside it.
                if(Preferences(this@StreamVrActivity).goVrWarmUp) VrCinemaNative.warmUp(native)
                val frameReady = AtomicBoolean(false)
                // PLE-673: count every signal, so the window log separates frames the decoder
                // delivered from frames the render loop latched.
                val available = AtomicInteger(0)
                val consumer = SurfaceTexture(VrCinemaNative.videoTexture(native))
                texture = consumer
                consumer.setDefaultBufferSize(videoProfile?.width ?: PREVIEW_WIDTH, videoProfile?.height ?: PREVIEW_HEIGHT)
                // PLE-673: off (the default) keeps the listener on the main looper, as PLE-654 measured.
                val listenerHandler = if(Preferences(this@StreamVrActivity).goVrFrameListenerThread) {
                    val thread = HandlerThread("GoCinemaFrames", Process.THREAD_PRIORITY_DISPLAY).also { it.start() }
                    listenerThread = thread
                    Log.i("GoCinema", "Frame-available listener on its own thread")
                    Handler(thread.looper)
                } else main
                consumer.setOnFrameAvailableListener({ available.incrementAndGet(); frameReady.set(true) }, listenerHandler)
                val output = Surface(consumer)
                decoder = output
                // Before the output is published: a Library launch attaches its session as soon as it is.
                if(frameLatency) CinemaFrameLatency.enable(true)
                // PLE-746: GPU luma of every latched frame, and presses.csv/frames.csv for the input-to-photon rounds.
                if(latencyProbe) probe = startLatencyProbe(native)
                this.output = output
                val picture = if(preview) PreviewPicture(output) else null
                if(picture != null)
                    Log.i("GoCinema", "Debug preview: no console; synthetic ${PREVIEW_WIDTH}x$PREVIEW_HEIGHT picture at 60 fps, environment ${environment.environment.value}")
                main.post { if(cinema === this) attachSession() }
                val transform = FloatArray(16)
                android.opengl.Matrix.setIdentityM(transform, 0)
                var hasFrame = false
                var menu = false
                var previousText: String? = null
                // PLE-654: decoder frames latched vs eye frames submitted, logged per window.
                var windowStartNs = System.nanoTime()
                var latched = 0
                var submitted = 0
                var shown = 0
                var latchedNs = 0L
                val submitTiming = LongArray(2)
                val probeRows = LongArray(PROBE_ROWS * 5)
                var latencyWindowStartNs = windowStartNs
                val uiControl = FloatArray(VrUiHost.CONTROL_SIZE)
                val uiOut = FloatArray(VrUiHost.OUTPUT_SIZE)
                var environmentFrame = 0L
                var frame = 0L
                while(running.get()) {
                    VrCinemaNative.pace(native)
                    frame++
                    val debug = debugInput.getAndSet(0)
                    val input = VrCinemaNative.input(native) or debug
                    val host = ui
                    if(host != null) {
                        // PLE-722: Back opens and closes the VR menu; a click with no menu opens it (the
                        // host sees it in the frame's output), except while a Library launch picks its
                        // console, whose chooser keeps the touchpad thirds.
                        if(input and MENU != 0) host.back()
                        host.clickOpensMenu = !picking
                        // PLE-730: VR Home takes the remote through the pointer; the debug broadcast's
                        // thirds (PLE-739) drive it as the pad's D-pad and A.
                        if(debug and CLICK != 0 && host.homeShown && !host.menuOpen) host.debugKey(when {
                            debug and CENTRE != 0 -> KeyEvent.KEYCODE_BUTTON_A
                            debug and RIGHT != 0 -> KeyEvent.KEYCODE_DPAD_DOWN
                            else -> KeyEvent.KEYCODE_DPAD_UP
                        })
                        if(input and CLICK != 0 && picking && !host.menuOpen && !host.homeShown) {
                            val touch = when {
                                input and CENTRE != 0 -> GoVrLibraryFlow.Touch.CENTRE
                                input and RIGHT != 0 -> GoVrLibraryFlow.Touch.RIGHT
                                else -> GoVrLibraryFlow.Touch.LEFT
                            }
                            main.post { if(cinema === this) libraryFlow?.click(touch) }
                        }
                        if(host.takeRecentre()) VrCinemaNative.recentre(native)
                        // Settings from the menu; a dragged slider rebuilds the room at most ~12 times a second.
                        if(frame - environmentFrame >= ENVIRONMENT_APPLY_FRAMES && host.takeEnvironmentChange()) {
                            environmentFrame = frame
                            applyEnvironment(native, storedEnvironment())
                        }
                        host.control(uiControl)
                    } else {
                        if(input and MENU != 0) menu = !menu
                        if(input and CLICK != 0) {
                            if(!menu && picking) {
                                // PLE-690: a Library launch's chooser and messages take the touchpad until it streams.
                                val touch = when {
                                    input and CENTRE != 0 -> GoVrLibraryFlow.Touch.CENTRE
                                    input and RIGHT != 0 -> GoVrLibraryFlow.Touch.RIGHT
                                    else -> GoVrLibraryFlow.Touch.LEFT
                                }
                                main.post { if(cinema === this) libraryFlow?.click(touch) }
                            } else if(!menu || input and CENTRE != 0) VrCinemaNative.recentre(native)
                            if(menu && input and RIGHT != 0) {
                                main.post { if(cinema === this) leave() }
                            }
                            menu = false
                        }
                    }
                    picture?.post()
                    val newFrame = frameReady.getAndSet(false)
                    if(newFrame) {
                        if(probe) android.os.Trace.beginSection("PLE746 cinema updateTexImage")
                        consumer.updateTexImage()
                        latchedNs = System.nanoTime()
                        if(probe) {
                            android.os.Trace.endSection()
                            VrCinemaNative.probeFrame(native, consumer.timestamp, latchedNs)
                        }
                        consumer.getTransformMatrix(transform)
                        hasFrame = true
                        latched++
                        picture?.consumed()
                    }
                    // PLE-690: before a Library launch streams, the right third leaves the app.
                    val text = if(menu) getString(R.string.go_vr_menu) + "\n\n" +
                        getString(R.string.go_vr_resume) + "     |     " + getString(R.string.go_vr_recentre) +
                        "     |     " + getString(if(picking) R.string.go_vr_exit else R.string.go_vr_disconnect) + "\n\n" +
                        getString(if(picking) R.string.go_vr_menu_help_exit else R.string.go_vr_menu_help)
                    // PLE-730: VR Home carries the words; the strip under it stays blank.
                    else if(host != null && host.homeShown) ""
                    else status
                    if(text != previousText) {
                        uploadText(VrCinemaNative.messageTexture(native), text)
                        previousText = text
                    }
                    val video = hasFrame && text.isEmpty()
                    check(VrCinemaNative.draw(native, transform, video, menu, newFrame,
                            if(host != null) uiControl else null, if(host != null) uiOut else null) >= 0) {
                        "vrapi_SubmitFrame2 failed"
                    }
                    host?.frame(uiOut, System.nanoTime())
                    submitted++
                    if(video) shown++
                    if(probe) {
                        // PLE-746: latches come back once their luma is read, a frame or more later, in order.
                        val rows = VrCinemaNative.probeTake(native, probeRows)
                        for(i in 0 until rows) {
                            CinemaFrameLatency.latched(probeRows[i * 5], probeRows[i * 5 + 1], probeRows[i * 5 + 2],
                                probeRows[i * 5 + 3], probeRows[i * 5 + 4].toInt())
                        }
                    } else if(newFrame && frameLatency) {
                        if(video) VrCinemaNative.submitTiming(native, submitTiming)
                        CinemaFrameLatency.latched(consumer.timestamp, latchedNs,
                            if(video) submitTiming[0] else 0L, if(video) submitTiming[1] else 0L)
                    }
                    val nowNs = System.nanoTime()
                    // PLE-698: a stream logs the latency with its 1 s stats window; the preview has no session.
                    if(preview && frameLatency && nowNs - latencyWindowStartNs >= LATENCY_WINDOW_NS) {
                        CinemaFrameLatency.logWindow()
                        latencyWindowStartNs = nowNs
                    }
                    if(nowNs - windowStartNs >= VIDEO_STATS_WINDOW_NS) {
                        val seconds = (nowNs - windowStartNs) / 1e9
                        Log.i("GoCinema", String.format(Locale.US,
                            "Cinema video: %d decoder frames latched in %.1f s (%.1f fps), %d of %d submitted frames showed video, %d frame signals",
                            latched, seconds, latched / seconds, shown, submitted, available.getAndSet(0)))
                        windowStartNs = nowNs
                        latched = 0
                        submitted = 0
                        shown = 0
                    }
                }
            } catch(error: Exception) {
                main.post { if(cinema === this) cinemaFailed(error) }
            } catch(error: LinkageError) {
                main.post { if(cinema === this) cinemaFailed(error) }
            } finally {
                // An error may reach here while the decoder still holds the output. The main
                // thread stops it before signalling detached; never free a live producer's target.
                detached.await()
                // PLE-722: no Canvas may touch a panel Surface once its swapchain goes with the cinema.
                ui?.shutdown()
                if(probe) LatencyProbe.stop()
                if(frameLatency) CinemaFrameLatency.enable(false)
                listenerThread?.quitSafely()
                decoder?.release()
                texture?.release()
                if(native != 0L) VrCinemaNative.destroy(native)
            }
        }

        /** The room and screen, and (PLE-722) where the VR UI's panels go for them. */
        private fun applyEnvironment(native: Long, environment: VrEnvironmentConfig) {
            environment.toNative().let {
                VrCinemaNative.setEnvironment(native, it.environment, it.screenDistanceM, it.screenWidthM,
                    it.screenCurveRadiusM, it.screenHeightOffsetM, it.glow, it.roomLight)
            }
            ui?.let {
                it.radius = VrUi.panelRadius(environment)
                it.statsAnchor = VrStatsPanel.anchor(environment)
            }
        }

        /**
         * PLE-746: a new directory per cinema, `<external files>/latency-probe/<UTC stamp>/`, readable over adb
         * without run-as. False (and the probe off) when the luma pass or the files cannot be made.
         */
        private fun startLatencyProbe(native: Long): Boolean {
            val stamp = SimpleDateFormat("yyyyMMdd'T'HHmmss'Z'", Locale.US).apply { timeZone = TimeZone.getTimeZone("UTC") }.format(Date())
            val directory = getExternalFilesDir(null)?.let { File(it, "latency-probe/$stamp") }
            if(directory == null || !directory.mkdirs()) {
                Log.e("GoCinema", "Latency probe: no directory for its files ($directory)")
                return false
            }
            if(!VrCinemaNative.probeEnable(native)) return false
            return LatencyProbe.start(directory.path).also {
                if(it) Log.i("GoCinema", "Latency probe: writing ${directory.path}")
            }
        }

        private fun uploadText(texture: Int, text: String) {
            val bitmap = Bitmap.createBitmap(1536, 864, Bitmap.Config.ARGB_8888)
            val canvas = Canvas(bitmap)
            canvas.drawColor(Color.rgb(12, 15, 22))
            val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
                color = Color.WHITE; textSize = 36f; textAlign = Paint.Align.CENTER
            }
            val lines = text.split('\n')
            // PLE-690: the Library chooser can outgrow the old block; only then does it start higher.
            val top = minOf(300f, 864f - 48f - (lines.size - 1) * 64f)
            lines.forEachIndexed { i, line -> canvas.drawText(line, 768f, top + i * 64f, paint) }
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, texture)
            GLUtils.texImage2D(GLES20.GL_TEXTURE_2D, 0, bitmap, 0)
            bitmap.recycle()
        }
    }

    /**
     * PLE-623 debug preview: a moving test picture queued into the decoder's SurfaceTexture at
     * 60 fps, so the environment's glow refresh and picture sampling run as in a stream. Drawn
     * on the cinema thread, one frame in flight at a time, so it never blocks on its consumer.
     */
    private class PreviewPicture(private val output: Surface) {
        private val bars = intArrayOf(Color.WHITE, Color.YELLOW, Color.CYAN, Color.GREEN, Color.MAGENTA, Color.RED, Color.BLUE)
        private val paint = Paint()
        private val text = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.WHITE; textSize = PREVIEW_HEIGHT / 12f }
        private var frames = 0L
        private var deadlineNs = 0L
        private var inFlight = false

        fun post() {
            val now = System.nanoTime()
            // PLE-716: a deadline accumulator, not "skip if under 1/60 s since the last post". Polled
            // from a 72 Hz loop, that check posted every other iteration (36 fps); advancing the
            // deadline by a fixed period posts 60 of every 72. Resync after a stall instead of bursting.
            if(inFlight || now < deadlineNs) return
            deadlineNs = if(now - deadlineNs > PERIOD_NS) now + PERIOD_NS else deadlineNs + PERIOD_NS
            val canvas = output.lockHardwareCanvas()
            try {
                val w = PREVIEW_WIDTH.toFloat()
                val h = PREVIEW_HEIGHT.toFloat()
                val bar = w / bars.size
                bars.forEachIndexed { i, color -> paint.color = color; canvas.drawRect(i * bar, 0f, (i + 1) * bar, h * 0.75f, paint) }
                paint.color = Color.rgb(24, 24, 32)
                canvas.drawRect(0f, h * 0.75f, w, h, paint)
                val x = (frames % 120) / 120f * w
                paint.color = Color.WHITE
                canvas.drawRect(x, h * 0.75f, x + w / 40f, h, paint)
                canvas.drawText("Pleikkari VR cinema preview   frame $frames", w * 0.05f, h * 0.92f, text)
            } finally {
                output.unlockCanvasAndPost(canvas)
            }
            frames++
            inFlight = true
        }

        fun consumed() {
            inFlight = false
        }

        private companion object {
            const val PERIOD_NS = 1_000_000_000L / 60
        }
    }

    companion object {
        // JNI poll result: Back toggles menu; touchpad click recentres or picks its horizontal third.
        private const val MENU = 1
        private const val CLICK = 2
        private const val CENTRE = 4
        private const val RIGHT = 8
        /** PLE-623: debug builds only; see [PreviewPicture] and third_party/ovr_sdk_mobile/README.md. */
        const val EXTRA_VR_CINEMA_PREVIEW = "vr_cinema_preview"
        /** With the preview: plain, void, cinema or terrace instead of the stored setting. */
        const val EXTRA_ENVIRONMENT = "environment"
        /** PLE-666: with the preview, a PLE-650 sky variant (0 is the shipped dome). */
        const val EXTRA_SKY_VARIANT = "sky_variant"
        /** PLE-653: with the preview, the rooms' MSAA sample count (1 turns MSAA off) instead of PLE-615's 4x. */
        const val EXTRA_ENVIRONMENT_MSAA = "environment_msaa"
        private const val DEFAULT_ENVIRONMENT_SAMPLES = 4
        private const val FULL_POSE_PROPERTY = "debug.pleikkari.vr_full_pose"
        /** PLE-715: debug builds only; frame pacing experiments, see vr-frame-pacing.h. */
        private const val PACING_PROPERTY = "debug.pleikkari.vr_pacing"
        /** PLE-722: debug builds only; "x,y" degrees from the open menu's middle for a synthetic pointer. */
        private const val POINTER_PROPERTY = "debug.pleikkari.vr_pointer"
        /** PLE-761: debug builds only; panel layer switches, see VrUiLayerDebug. */
        private const val UI_LAYERS_PROPERTY = "debug.pleikkari.vr_ui_layers"
        /** PLE-722: vr-ui-layers.h's VrUiAnchor*. */
        private const val ANCHOR_GAZE = 0
        private const val ANCHOR_PICTURE = 1
        /** PLE-722: a remote Back held this long recentres (Skybox's 0.75 s). */
        private const val LONG_BACK_MS = 750L
        private const val ENVIRONMENT_APPLY_FRAMES = 6
        /** PLE-690: debug builds only; a Library launch opens its chooser instead of connecting. */
        private const val CHOOSE_PROPERTY = "debug.pleikkari.go_entry_choose"
        /** PLE-739: debug builds only; a Library launch sees no linked console. */
        private const val NO_CONSOLE_PROPERTY = "debug.pleikkari.go_entry_no_console"
        /** PLE-784: debug builds only; a Library launch's first console look ends not found. */
        private const val NOT_FOUND_PROPERTY = "debug.pleikkari.go_entry_not_found"
        /** PLE-739: debug builds only; see [registerDebugInput]. */
        private const val DEBUG_INPUT_ACTION = "fi.madekivi.pleikkari.DEBUG_GO_VR_INPUT"
        private const val EXTRA_DEBUG_KEY = "key"
        /** PLE-746: debug builds only; see [registerDebugInput]. */
        private const val EXTRA_DEBUG_PAD = "pad"
        private const val EXTRA_DEBUG_HOLD_MS = "hold_ms"
        private val DEBUG_PAD_BUTTONS = mapOf(
            "cross" to ControllerState.BUTTON_CROSS, "circle" to ControllerState.BUTTON_MOON,
            "square" to ControllerState.BUTTON_BOX, "triangle" to ControllerState.BUTTON_PYRAMID,
            "up" to ControllerState.BUTTON_DPAD_UP, "down" to ControllerState.BUTTON_DPAD_DOWN,
            "left" to ControllerState.BUTTON_DPAD_LEFT, "right" to ControllerState.BUTTON_DPAD_RIGHT,
            "l1" to ControllerState.BUTTON_L1, "r1" to ControllerState.BUTTON_R1,
            "options" to ControllerState.BUTTON_OPTIONS, "ps" to ControllerState.BUTTON_PS)
        /** PLE-746: probe records taken per render loop; a loop normally has one or none. */
        private const val PROBE_ROWS = 8
        /** PLE-690: set only by [leave]; the activity is not exported. */
        private const val EXTRA_LIBRARY_CHOOSER = "library_chooser"
        private const val TAG_ENTRY = "GoVrEntry"
        private const val STATE_CONNECT_INFO = "library_connect_info"

        /** android.os.SystemProperties is hidden API; a debug-only reader, "" on any failure. */
        private fun debugProperty(name: String): String = try {
            Class.forName("android.os.SystemProperties").getMethod("get", String::class.java).invoke(null, name) as String
        } catch(e: Exception) {
            ""
        }
        private const val PREVIEW_WIDTH = 1280
        private const val PREVIEW_HEIGHT = 720
        private const val VIDEO_STATS_WINDOW_NS = 5_000_000_000L
        /** PLE-698: the preview's "Cinema latency" window, the stats log's 1 s (Preferences.feedbackStatsLogIntervalMs). */
        private const val LATENCY_WINDOW_NS = 1_000_000_000L
    }
}

internal object VrCinemaNative {
    external fun create(activity: android.app.Activity, surface: Surface, refreshHz: Float, environmentSamples: Int): Long
    external fun videoTexture(handle: Long): Int
    external fun messageTexture(handle: Long): Int
    external fun input(handle: Long): Int
    /** PLE-715: the top of every loop iteration, before input and the latch; sleeps when pacing asks. */
    external fun pace(handle: Long)
    /** PLE-715: mode 0 VrApi's release (default), 1 the late start; log the per-second pacing line. */
    external fun setPacing(handle: Long, mode: Int, log: Boolean, spec: String, refreshHz: Float)
    external fun warmUp(handle: Long)
    external fun recentre(handle: Long)
    external fun setFullPoseRecentre(handle: Long, enabled: Boolean)
    /** PLE-722: [uiControl] null keeps the strip menu; otherwise VrUiHost's control slots in, its output slots in [uiOut]. */
    external fun draw(handle: Long, transform: FloatArray, video: Boolean, menu: Boolean, newFrame: Boolean,
        uiControl: FloatArray?, uiOut: FloatArray?): Int
    /** PLE-722: a VR UI panel's Surface (vr-ui-layers.h), or null when the UI cannot run. */
    external fun createPanel(handle: Long, index: Int, width: Int, height: Int, inset: Float, corner: Float, anchor: Int): Surface?
    /** PLE-698: the last draw's {vrapi_SubmitFrame2 call, predicted display time}, CLOCK_MONOTONIC ns. */
    external fun submitTiming(handle: Long, out: LongArray)
    /** PLE-761: debug builds only, before createPanel; vr-ui-layers.h's VrUiDebug. */
    external fun debugSetUiLayers(handle: Long, quad: Boolean, overlay: Boolean, texelScale: Float,
        filterExpensive: Boolean, maxPanels: Int, reticleLayer: Boolean)
    /** PLE-746: the input-to-photon probe's GPU luma pass; see vr-cinema.cpp's LumaProbe. */
    external fun probeEnable(handle: Long): Boolean
    external fun probeFrame(handle: Long, bufferTimestampNs: Long, latchedNs: Long)
    /** Fills {buffer timestamp, latched, submitted, predicted photon, luma} records; returns how many. */
    external fun probeTake(handle: Long, out: LongArray): Int
    /** PLE-603: [VrEnvironmentNativeConfig] fields; environment 0 (plain) removes the room. */
    external fun setRoomGpuLevel(handle: Long, level: Int)
    external fun setEnvironment(handle: Long, environment: Int, distance: Float, width: Float, radius: Float,
        heightOffset: Float, glow: Float, roomLight: Float)
    external fun debugSetSkyVariant(handle: Long, variant: Int)
    external fun destroy(handle: Long)
}
