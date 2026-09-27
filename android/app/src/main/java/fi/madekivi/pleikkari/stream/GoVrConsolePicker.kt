// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
package fi.madekivi.pleikkari.stream

import fi.madekivi.pleikkari.common.MacAddress
import fi.madekivi.pleikkari.common.ManualHost
import fi.madekivi.pleikkari.common.RegisteredHost
import fi.madekivi.pleikkari.discovery.serverMac
import fi.madekivi.pleikkari.lib.DiscoveryHost

/**
 * PLE-690: which registered console a Library launch on the Oculus Go streams from, and how to
 * reach it. Pure, so the rules are unit-tested; StreamVrActivity's Library flow drives it.
 */
object GoVrConsolePicker {
    sealed class Pick {
        /** No PS5 is linked: point at linking in the 2D app. */
        object NoConsole : Pick()
        data class Connect(val console: RegisteredHost, val lastUsed: Boolean) : Pick()
        /** Several PS5s and none streamed last: the in-VR chooser. */
        data class Choose(val consoles: List<RegisteredHost>) : Pick()
    }

    /** The last used console, or the only one; otherwise the chooser, or nothing to stream from. */
    fun pick(registered: List<RegisteredHost>, lastUsed: MacAddress?): Pick {
        val consoles = consoles(registered)
        consoles.firstOrNull { it.serverMac == lastUsed }?.let { return Pick.Connect(it, lastUsed = true) }
        return when(consoles.size) {
            0 -> Pick.NoConsole
            1 -> Pick.Connect(consoles.single(), lastUsed = false)
            else -> Pick.Choose(consoles)
        }
    }

    /** The chooser's consoles: every linked PS5, by name. */
    fun consoles(registered: List<RegisteredHost>) = registered.filter { it.target.isPS5 }.sortedBy { name(it).lowercase() }

    fun name(console: RegisteredHost) = console.serverNickname?.takeIf { it.isNotBlank() } ?: console.serverMac.toString()

    sealed class Route {
        /** Awake on this network: connect. */
        data class Ready(val address: String) : Route()
        /** In rest mode on this network: send a wake-up and keep looking. */
        data class Standby(val address: String) : Route()
        /** Not discovered, but the user gave it an address (Home's manual console): try that. */
        data class Manual(val address: String) : Route()
        /** Not seen yet: keep looking. */
        object Unknown : Route()
    }

    fun route(console: RegisteredHost, discovered: List<DiscoveryHost>, manual: List<ManualHost>): Route {
        val found = discovered.firstOrNull { it.isPS5 && it.serverMac == console.serverMac && !it.hostAddr.isNullOrBlank() }
        val address = found?.hostAddr
        if(found != null && address != null) when(found.state) {
            DiscoveryHost.State.READY -> return Route.Ready(address)
            DiscoveryHost.State.STANDBY -> return Route.Standby(address)
            DiscoveryHost.State.UNKNOWN -> Unit
        }
        return manual.firstOrNull { it.registeredHost == console.id && it.host.isNotBlank() }
            ?.let { Route.Manual(it.host) } ?: Route.Unknown
    }

    /**
     * The in-VR list: every console, then one row that opens the 2D app on the Oculus TV screen.
     * Touchpad left/right thirds (or the pad's D-pad) move, the centre (or A) chooses.
     */
    class Chooser(val consoles: List<RegisteredHost>, lastUsed: MacAddress?) {
        val size get() = consoles.size + 1
        var index = consoles.indexOfFirst { it.serverMac == lastUsed }.coerceAtLeast(0)
            private set
        /** Null on the last row, the Oculus TV screen. */
        val selected: RegisteredHost? get() = consoles.getOrNull(index)

        fun move(delta: Int) {
            index = Math.floorMod(index + delta, size)
        }

        /** At most [rows] rows around the selection, as (row index, console or null for the TV-screen row). */
        fun window(rows: Int = 6): List<Pair<Int, RegisteredHost?>> {
            val from = (index - rows / 2).coerceIn(0, (size - rows).coerceAtLeast(0))
            return (from until minOf(size, from + rows)).map { it to consoles.getOrNull(it) }
        }
    }
}
