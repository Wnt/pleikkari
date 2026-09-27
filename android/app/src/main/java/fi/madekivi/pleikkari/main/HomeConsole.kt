// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.main

import fi.madekivi.pleikkari.common.DiscoveredDisplayHost
import fi.madekivi.pleikkari.common.DisplayHost
import fi.madekivi.pleikkari.common.ManualDisplayHost
import fi.madekivi.pleikkari.common.RegisteredHost
import fi.madekivi.pleikkari.discovery.serverMac
import fi.madekivi.pleikkari.lib.DiscoveryHost

enum class HomeConsoleStatus { ON, STANDBY, REMOTE, REGISTRATION_REQUIRED }

data class HomeConsole(
	val key: String,
	val name: String,
	val detail: String?,
	val status: HomeConsoleStatus,
	val displayHost: DisplayHost? = null,
	val psnConsole: PsnConsole? = null,
	val manualDisplayHost: ManualDisplayHost? = displayHost as? ManualDisplayHost
)

/**
 * PLE-291: a console found on the network in rest mode that is not linked yet. It cannot
 * be woken over the LAN without a registration key, but the PSN link wakes it, so the card
 * offers "Wake and link" and says it is resting instead of a bare "Link".
 */
val HomeConsole.isUnlinkedResting: Boolean
	get() = status == HomeConsoleStatus.REGISTRATION_REQUIRED &&
		(displayHost as? DiscoveredDisplayHost)?.discoveredHost?.state == DiscoveryHost.State.STANDBY

private data class LocalConsoleGroup(
	val hosts: MutableList<DisplayHost>,
	val identities: MutableSet<String>
)

private fun normalizedAddress(address: String): String = address
	.trim()
	.removePrefix("[")
	.removeSuffix("]")
	.trimEnd('.')
	.lowercase()

private fun localIdentities(host: DisplayHost): Set<String> = buildSet {
	host.registeredHost?.also { registration ->
		if(registration.id > 0L)
			add("registration:${registration.id}")
		add("mac:${registration.serverMac.toString().lowercase()}")
	}
	(host as? DiscoveredDisplayHost)?.discoveredHost?.serverMac?.also {
		add("mac:${it.toString().lowercase()}")
	}
	normalizedAddress(host.host).takeIf(String::isNotBlank)?.also { add("address:$it") }
}

private fun registrationsMatch(first: RegisteredHost?, second: RegisteredHost?): Boolean =
	first != null && second != null &&
		((first.id > 0L && second.id > 0L && first.id == second.id) || first.serverMac == second.serverMac)

private fun groupLocalConsoles(hosts: List<DisplayHost>): List<LocalConsoleGroup>
{
	val groups = mutableListOf<LocalConsoleGroup>()
	hosts.forEach { host ->
		val identities = localIdentities(host)
		val matches = groups.filter { group -> group.identities.any(identities::contains) }
		if(matches.isEmpty())
		{
			groups += LocalConsoleGroup(mutableListOf(host), identities.toMutableSet())
			return@forEach
		}

		val destination = matches.first()
		destination.hosts += host
		destination.identities += identities
		matches.drop(1).forEach { duplicate ->
			destination.hosts += duplicate.hosts
			destination.identities += duplicate.identities
			groups.remove(duplicate)
		}
	}
	return groups
}

private fun discoveryStatePriority(host: DiscoveredDisplayHost): Int = when(host.discoveredHost.state)
{
	DiscoveryHost.State.READY -> 0
	DiscoveryHost.State.STANDBY -> 1
	else -> 2
}

private fun homeConsole(
	group: LocalConsoleGroup,
	psnConsole: PsnConsole?
): HomeConsole
{
	val discovered = group.hosts.filterIsInstance<DiscoveredDisplayHost>()
		.minByOrNull(::discoveryStatePriority)
	val manual = group.hosts.filterIsInstance<ManualDisplayHost>().firstOrNull()
	val registration = discovered?.registeredHost
		?: manual?.registeredHost
		?: group.hosts.firstNotNullOfOrNull(DisplayHost::registeredHost)
		?: psnConsole?.registeredHost
	val displayHost = discovered?.takeIf { it.registeredHost != null }
		?: manual?.takeIf { it.registeredHost != null }
		?: discovered
		?: manual
		?: group.hosts.first()
	val address = discovered?.host?.takeIf(String::isNotBlank)
		?: manual?.host?.takeIf(String::isNotBlank)
		?: displayHost.host.takeIf(String::isNotBlank)
	val status = when
	{
		registration == null -> HomeConsoleStatus.REGISTRATION_REQUIRED
		discovered?.discoveredHost?.state == DiscoveryHost.State.READY -> HomeConsoleStatus.ON
		discovered?.discoveredHost?.state == DiscoveryHost.State.STANDBY -> HomeConsoleStatus.STANDBY
		else -> HomeConsoleStatus.REMOTE
	}
	val key = registration?.serverMac?.toString()?.lowercase()
		?: group.identities.firstOrNull { it.startsWith("mac:") }
		?: group.identities.firstOrNull { it.startsWith("address:") }
		?: "local:${displayHost.id.orEmpty()}:${displayHost.name.orEmpty().trim().lowercase()}"
	return HomeConsole(
		key = key,
		name = discovered?.name?.takeIf(String::isNotBlank)
			?: registration?.serverNickname?.takeIf(String::isNotBlank)
			?: psnConsole?.device?.name?.takeIf(String::isNotBlank)
			?: displayHost.name?.takeIf(String::isNotBlank)
			?: address.orEmpty(),
		detail = discovered?.discoveredHost?.runningAppName?.takeIf(String::isNotBlank) ?: address,
		status = status,
		displayHost = displayHost,
		psnConsole = psnConsole,
		manualDisplayHost = manual
	)
}

/**
 * A console found on the network that is not linked yet is the same console as an unlinked one on
 * the signed-in account when the names match; the row then links it over PSN, without a PIN.
 */
private fun unlinkedPsnConsoleFor(
	group: LocalConsoleGroup,
	registrations: List<RegisteredHost>,
	psnConsoles: List<PsnConsole>
): PsnConsole?
{
	if(registrations.isNotEmpty())
		return null
	val names = group.hosts.filterIsInstance<DiscoveredDisplayHost>()
		.mapNotNull { it.name?.trim()?.takeIf(String::isNotEmpty)?.lowercase() }
		.toSet()
	return psnConsoles.firstOrNull { console ->
		console.registeredHost == null && console.device.name.trim().lowercase() in names
	}
}

internal fun mergeHomeConsoles(
	hosts: List<DisplayHost>,
	psnConsoles: List<PsnConsole>
): List<HomeConsole>
{
	val remainingPsn = psnConsoles.toMutableList()
	val local = groupLocalConsoles(hosts).map { group ->
		val registrations = group.hosts.mapNotNull(DisplayHost::registeredHost)
		val psn = (remainingPsn.firstOrNull { remote ->
			registrations.any { localRegistration ->
				registrationsMatch(localRegistration, remote.registeredHost)
			}
		} ?: unlinkedPsnConsoleFor(group, registrations, remainingPsn))?.also(remainingPsn::remove)
		homeConsole(group, psn)
	}
	val remoteOnly = remainingPsn.map { console ->
		HomeConsole(
			key = "psn:${console.device.duid}",
			name = console.device.name,
			detail = null,
			status = if(console.registeredHost == null)
				HomeConsoleStatus.REGISTRATION_REQUIRED else HomeConsoleStatus.REMOTE,
			psnConsole = console
		)
	}
	return (local + remoteOnly).sortedWith(
		compareBy<HomeConsole> { it.status.ordinal }.thenBy(String.CASE_INSENSITIVE_ORDER) { it.name }
	)
}
