// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
package fi.madekivi.pleikkari.main

import fi.madekivi.pleikkari.common.DisplayHost

/**
 * Home's one pending "stream from this console as soon as it is listed": the console a PIN
 * registration just linked, or PLE-659's debug-only `auto_connect_host` extra. It fires once, on
 * the first host list with a registered console at that address, and then nothing is pending.
 */
internal class PendingAutoPlay
{
	data class Play(val host: DisplayHost, val justLinked: Boolean)

	var address: String? = null
		private set
	private var justLinked = false

	fun request(address: String?, justLinked: Boolean = false)
	{
		this.address = address?.takeIf { it.isNotBlank() }
		this.justLinked = justLinked
	}

	/** The registered console at the pending address in [hosts], if any; it is no longer pending then. */
	fun take(hosts: List<DisplayHost>): Play?
	{
		val address = address ?: return null
		val host = hosts.firstOrNull { it.host == address && it.registeredHost != null } ?: return null
		val play = Play(host, justLinked)
		request(null)
		return play
	}
}
