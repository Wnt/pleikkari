// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
package fi.madekivi.pleikkari.stream

import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.view.KeyEvent
import androidx.activity.ComponentActivity
import androidx.lifecycle.lifecycleScope
import fi.madekivi.pleikkari.R
import fi.madekivi.pleikkari.common.ManualHost
import fi.madekivi.pleikkari.common.Preferences
import fi.madekivi.pleikkari.common.RegisteredHost
import fi.madekivi.pleikkari.common.getDatabase
import fi.madekivi.pleikkari.discovery.DiscoveryManager
import fi.madekivi.pleikkari.lib.ConnectInfo
import fi.madekivi.pleikkari.lib.DiscoveryHost
import fi.madekivi.pleikkari.stream.GoVrConsolePicker.Presence
import fi.madekivi.pleikkari.stream.GoVrConsolePicker.Route
import fi.madekivi.pleikkari.stream.vrui.ButtonStyle
import fi.madekivi.pleikkari.stream.vrui.VrCardTone
import fi.madekivi.pleikkari.stream.vrui.VrConsoleCard
import fi.madekivi.pleikkari.stream.vrui.VrHomeAction
import fi.madekivi.pleikkari.stream.vrui.VrHomeButton
import fi.madekivi.pleikkari.stream.vrui.VrHomeState
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch

/**
 * PLE-690: a Go Library launch before its stream. Reads the linked consoles, finds the picked one
 * with LAN discovery (waking it from rest mode), and shows the in-VR chooser and messages through
 * [show]; [connect] starts the stream on the same StreamSession path as Connect. Main thread only.
 * With [chooseFirst] (after a Disconnect) it opens the chooser instead of streaming on its own.
 * With [noConsoles] (PLE-739, debug builds only) it ignores every linked console.
 *
 * PLE-730: every screen goes to [show] as today's strip text (the touchpad-thirds fallback) and to
 * [home] as a VR Home page (console cards and status sheets), which the VR UI shows when it runs.
 * [play], [cancel], [retry] and [consoles] are its buttons; [connect] also names the console.
 */
internal class GoVrLibraryFlow(
    private val activity: ComponentActivity,
    private val chooseFirst: Boolean,
    private val noConsoles: Boolean,
    private val show: (String) -> Unit,
    private val home: (VrHomeState) -> Unit,
    private val connect: (ConnectInfo, String) -> Unit,
    private val openPanel: () -> Unit,
    private val exit: () -> Unit
) {
    enum class Touch { LEFT, CENTRE, RIGHT }

    private sealed class Screen {
        object Loading : Screen()
        object NoConsole : Screen()
        class Choosing(val chooser: GoVrConsolePicker.Chooser) : Screen()
        class Finding(val console: RegisteredHost, val sinceMs: Long) : Screen() {
            var wakeSentMs: Long? = null
        }
        class NotFound(val console: RegisteredHost) : Screen()
        object Connected : Screen()
    }

    private val preferences = Preferences(activity)
    private val discovery = DiscoveryManager()
    private val handler = Handler(Looper.getMainLooper())
    private val tick = object : Runnable {
        override fun run() {
            update()
            if(screen is Screen.Finding)
                handler.postDelayed(this, TICK_MS)
        }
    }
    private val jobs = mutableListOf<Job>()
    private var registered = emptyList<RegisteredHost>()
    private var manual = emptyList<ManualHost>()
    private var discovered = emptyList<DiscoveryHost>()
    private var screen: Screen = Screen.Loading
    /** PLE-730: when discovery started; a card says "not found" only after [FIND_TIMEOUT_MS] of it. */
    private var lookingSinceMs = 0L
    private val lookedLongEnough = Runnable { if(screen is Screen.Choosing) render() }

    fun start() {
        lookingSinceMs = SystemClock.elapsedRealtime()
        handler.postDelayed(lookedLongEnough, FIND_TIMEOUT_MS)
        render()
        discovery.active = true
        jobs += activity.lifecycleScope.launch {
            val database = getDatabase(activity)
            registered = if(noConsoles) emptyList() else database.registeredHostDao().getAll().first()
            manual = database.manualHostDao().getAll().first()
            begin()
        }
        jobs += activity.lifecycleScope.launch {
            discovery.discoveredHosts.collect {
                discovered = it
                update()
                // PLE-730: the cards show what discovery sees.
                if(screen is Screen.Choosing) render()
            }
        }
    }

    fun pause() = discovery.pause()

    fun resume() = discovery.resume()

    fun stop() {
        jobs.forEach { it.cancel() }
        jobs.clear()
        handler.removeCallbacksAndMessages(null)
        discovery.dispose()
    }

    /** A touchpad click, by third, while no stream runs. */
    fun click(touch: Touch) {
        when(val current = screen) {
            Screen.NoConsole -> openPanel()
            is Screen.Choosing -> when(touch) {
                Touch.LEFT -> current.chooser.move(-1)
                Touch.RIGHT -> current.chooser.move(1)
                Touch.CENTRE -> current.chooser.selected?.let(::find) ?: openPanel()
            }
            // The chooser has this console again, the others and the Oculus TV screen for Settings.
            is Screen.Finding -> choose(current.console)
            is Screen.NotFound -> choose(current.console)
            else -> return
        }
        render()
    }

    /** PLE-730: VR Home's Play on a console card: find it (waking it if it rests) and connect. */
    fun play(id: String) {
        if(screen !is Screen.Choosing) return
        val console = GoVrConsolePicker.consoles(registered).firstOrNull { it.serverMac.toString() == id } ?: return
        find(console)
        render()
    }

    /** PLE-730: Cancel on the finding or waking sheet: back to the cards. */
    fun cancel() {
        val finding = screen as? Screen.Finding ?: return
        Log.i(TAG, "Stopped looking for ${GoVrConsolePicker.name(finding.console)}")
        choose(finding.console)
        render()
    }

    /** PLE-730: Try again on the not-found sheet. */
    fun retry() {
        val notFound = screen as? Screen.NotFound ?: return
        find(notFound.console)
        render()
    }

    /** PLE-730: Consoles on the not-found sheet: back to the cards. */
    fun consoles() {
        val notFound = screen as? Screen.NotFound ?: return
        choose(notFound.console)
        render()
    }

    /** The Bluetooth pad before the stream: D-pad moves, A chooses, B leaves. */
    fun key(event: KeyEvent): Boolean {
        val touch = when(event.keyCode) {
            KeyEvent.KEYCODE_DPAD_LEFT, KeyEvent.KEYCODE_DPAD_UP -> Touch.LEFT
            KeyEvent.KEYCODE_DPAD_RIGHT, KeyEvent.KEYCODE_DPAD_DOWN -> Touch.RIGHT
            KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_DPAD_CENTER, KeyEvent.KEYCODE_ENTER -> Touch.CENTRE
            KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_BACK -> null
            else -> return false
        }
        if(event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0) {
            if(touch == null) exit() else click(touch)
        }
        return true
    }

    private fun begin() {
        when(val pick = GoVrConsolePicker.pick(registered, preferences.lastConsoleMac)) {
            GoVrConsolePicker.Pick.NoConsole -> {
                Log.i(TAG, "No PS5 linked (${registered.size} registered console(s)): pointing at linking on the Oculus TV screen")
                screen = Screen.NoConsole
            }
            is GoVrConsolePicker.Pick.Choose -> {
                Log.i(TAG, "${pick.consoles.size} PS5s linked, none streamed last: in-VR chooser")
                screen = Screen.Choosing(GoVrConsolePicker.Chooser(pick.consoles, null))
            }
            is GoVrConsolePicker.Pick.Connect -> {
                Log.i(TAG, "Picked ${GoVrConsolePicker.name(pick.console)}: " +
                    (if(pick.lastUsed) "the last used" else "the only") + " of ${registered.size} linked console(s)" +
                    if(chooseFirst) "; in-VR chooser first, no connect until one is chosen" else "")
                if(chooseFirst) choose(pick.console) else find(pick.console)
            }
        }
        render()
    }

    private fun choose(selected: RegisteredHost) {
        handler.removeCallbacks(tick)
        screen = Screen.Choosing(GoVrConsolePicker.Chooser(GoVrConsolePicker.consoles(registered), selected.serverMac))
    }

    private fun find(console: RegisteredHost) {
        screen = Screen.Finding(console, SystemClock.elapsedRealtime())
        handler.removeCallbacks(tick)
        handler.postDelayed(tick, TICK_MS)
        update()
    }

    private fun update() {
        val finding = screen as? Screen.Finding ?: return
        val console = finding.console
        val name = GoVrConsolePicker.name(console)
        val now = SystemClock.elapsedRealtime()
        val waking = finding.wakeSentMs != null
        when(val route = GoVrConsolePicker.route(console, discovered, manual)) {
            is Route.Ready -> return connectTo(console, route.address, "awake")
            is Route.Manual -> if(now - finding.sinceMs >= MANUAL_GRACE_MS)
                return connectTo(console, route.address, "not discovered, its manual address")
            is Route.Standby -> {
                val sent = finding.wakeSentMs
                if(sent == null || now - sent >= WAKE_REPEAT_MS) {
                    discovery.sendWakeup(route.address, console.rpRegistKey, true)
                    finding.wakeSentMs = now
                    Log.i(TAG, "$name is in rest mode at ${route.address}: wake-up sent")
                    if(!waking)
                        render()
                }
            }
            Route.Unknown -> Unit
        }
        if(now - finding.sinceMs >= if(waking) WAKE_TIMEOUT_MS else FIND_TIMEOUT_MS) {
            Log.i(TAG, "$name not ready on this network after ${(now - finding.sinceMs) / 1000} s" +
                if(waking) " (woken from rest mode)" else "")
            screen = Screen.NotFound(console)
            handler.removeCallbacks(tick)
            render()
        }
    }

    private fun connectTo(console: RegisteredHost, address: String, how: String) {
        Log.i(TAG, "Connecting to ${GoVrConsolePicker.name(console)} at $address ($how)")
        screen = Screen.Connected
        stop()
        preferences.lastConsoleMac = console.serverMac
        show(activity.getString(R.string.go_vr_connecting))
        connect(preferences.connectInfo(address, console), GoVrConsolePicker.name(console))
    }

    private fun render() {
        homeState()?.let(home)
        show(when(val current = screen) {
            Screen.Connected -> return // The session's own state text takes over.
            Screen.Loading -> activity.getString(R.string.go_vr_entry_loading)
            Screen.NoConsole -> activity.getString(R.string.go_vr_entry_no_console)
            is Screen.Choosing -> chooserText(current.chooser)
            is Screen.Finding -> activity.getString(
                if(current.wakeSentMs != null) R.string.go_vr_entry_waking else R.string.go_vr_entry_finding,
                GoVrConsolePicker.name(current.console)) + "\n\n" + activity.getString(R.string.go_vr_entry_finding_help)
            is Screen.NotFound -> activity.getString(R.string.go_vr_entry_not_found, GoVrConsolePicker.name(current.console)) +
                "\n\n" + activity.getString(R.string.go_vr_entry_not_found_help)
        })
    }

    /** PLE-730: the VR Home page for the current screen; null once connected (the activity's connecting sheet). */
    private fun homeState(): VrHomeState? {
        val cancel = VrHomeButton(activity.getString(R.string.go_vr_home_cancel), VrHomeAction.Cancel)
        val exit = VrHomeButton(activity.getString(R.string.go_vr_exit), VrHomeAction.Exit)
        val title = activity.getString(R.string.go_vr_home_title)
        return when(val current = screen) {
            Screen.Connected -> null
            Screen.Loading -> VrHomeState.Status(title, activity.getString(R.string.go_vr_home_loading), emptyList(),
                busy = true, buttons = listOf(exit), back = null)
            Screen.NoConsole -> VrHomeState.Status(title, activity.getString(R.string.go_vr_home_no_console),
                listOf(activity.getString(R.string.go_vr_home_no_console_detail)), busy = false,
                buttons = listOf(exit, VrHomeButton(activity.getString(R.string.go_vr_home_oculus_tv), VrHomeAction.OculusTv, ButtonStyle.PRIMARY)),
                back = null)
            is Screen.Choosing -> {
                val stillLooking = SystemClock.elapsedRealtime() - lookingSinceMs < FIND_TIMEOUT_MS
                val lastUsed = preferences.lastConsoleMac
                VrHomeState.Consoles(activity.getString(R.string.go_vr_entry_choose),
                    current.chooser.consoles.map { card(it, stillLooking, it.serverMac == lastUsed) },
                    current.chooser.selected?.serverMac?.toString(), activity.getString(R.string.go_vr_home_play),
                    activity.getString(R.string.go_vr_entry_panel),
                    listOf(VrHomeButton(activity.getString(R.string.go_vr_home_settings), VrHomeAction.Settings),
                        exit.copy(style = ButtonStyle.DESTRUCTIVE)))
            }
            is Screen.Finding -> {
                val name = GoVrConsolePicker.name(current.console)
                val waking = current.wakeSentMs != null
                VrHomeState.Status(name,
                    activity.getString(if(waking) R.string.go_vr_entry_waking else R.string.go_vr_entry_finding, name),
                    listOf(activity.getString(if(waking) R.string.go_vr_home_waking_detail else R.string.go_vr_home_finding_detail)),
                    busy = true, buttons = listOf(cancel), back = VrHomeAction.Cancel)
            }
            is Screen.NotFound -> {
                val name = GoVrConsolePicker.name(current.console)
                VrHomeState.Status(name, activity.getString(R.string.go_vr_home_not_found_message, name),
                    listOf(activity.getString(R.string.go_vr_home_not_found_detail)), busy = false,
                    buttons = listOf(VrHomeButton(activity.getString(R.string.go_vr_home_consoles), VrHomeAction.Consoles),
                        VrHomeButton(activity.getString(R.string.go_vr_home_retry), VrHomeAction.Retry, ButtonStyle.PRIMARY)),
                    back = VrHomeAction.Consoles)
            }
        }
    }

    private fun card(console: RegisteredHost, stillLooking: Boolean, lastUsed: Boolean): VrConsoleCard {
        val presence = GoVrConsolePicker.presence(GoVrConsolePicker.route(console, discovered, manual), stillLooking)
        val state = activity.getString(when(presence) {
            Presence.READY -> R.string.go_vr_home_ready
            Presence.STANDBY -> R.string.go_vr_home_rest
            Presence.MANUAL -> R.string.go_vr_home_manual
            Presence.LOOKING -> R.string.go_vr_home_looking
            Presence.NOT_FOUND -> R.string.go_vr_home_not_found
        })
        val tone = when(presence) {
            Presence.READY -> VrCardTone.READY
            Presence.STANDBY -> VrCardTone.ASLEEP
            else -> VrCardTone.UNKNOWN
        }
        return VrConsoleCard(console.serverMac.toString(), GoVrConsolePicker.name(console),
            if(lastUsed) activity.getString(R.string.go_vr_home_last_played, state) else state, tone)
    }

    private fun chooserText(chooser: GoVrConsolePicker.Chooser): String {
        val rows = chooser.window().joinToString("\n") { (row, console) ->
            val label = console?.let(GoVrConsolePicker::name) ?: activity.getString(R.string.go_vr_entry_panel)
            if(row == chooser.index) "›   $label   ‹" else label
        }
        return activity.getString(R.string.go_vr_entry_choose) + "\n\n" + rows + "\n\n" +
            activity.getString(R.string.go_vr_entry_choose_help)
    }

    private companion object {
        const val TAG = "GoVrEntry"
        const val TICK_MS = 1_000L
        /** Home's manual console: give discovery this long to find it awake first. */
        const val MANUAL_GRACE_MS = 3_000L
        const val FIND_TIMEOUT_MS = 15_000L
        /** A PS5 takes about 20 s from rest mode to answering Remote Play. */
        const val WAKE_TIMEOUT_MS = 60_000L
        const val WAKE_REPEAT_MS = 15_000L
    }
}
