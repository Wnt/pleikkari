// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.main

import fi.madekivi.pleikkari.common.DiscoveredDisplayHost
import fi.madekivi.pleikkari.common.MacAddress
import fi.madekivi.pleikkari.common.ManualDisplayHost
import fi.madekivi.pleikkari.common.ManualHost
import fi.madekivi.pleikkari.common.RegisteredHost
import fi.madekivi.pleikkari.lib.DiscoveryHost
import fi.madekivi.pleikkari.lib.Target
import fi.madekivi.pleikkari.remote.PsnDevice
import org.junit.Assert.assertEquals
import org.junit.Test

class HomeConsoleTest
{
	private fun registered(id: Long, name: String) = RegisteredHost(
		id = id,
		target = Target.PS5_1,
		apSsid = null,
		apBssid = null,
		apKey = null,
		apName = null,
		serverMac = MacAddress(id),
		serverNickname = name,
		rpRegistKey = ByteArray(16),
		rpKeyType = 0,
		rpKey = ByteArray(16)
	)

	@Test fun mergesMatchingLocalAndPsnConsoleAndUsesLocalPowerState()
	{
		val registration = registered(1, "Living Room")
		val discovered = DiscoveredDisplayHost(
			registration,
			DiscoveryHost(
				DiscoveryHost.State.READY, 0U, "192.168.1.2", null,
				"00030010", "Living Room", null, null, null, "Astro's Playroom"
			)
		)
		val psn = PsnConsole(PsnDevice("duid", "Living Room"), registration)

		val result = mergeHomeConsoles(listOf(discovered), listOf(psn))

		assertEquals(1, result.size)
		assertEquals(HomeConsoleStatus.ON, result.single().status)
		assertEquals("Astro's Playroom", result.single().detail)
		assertEquals(psn, result.single().psnConsole)
	}

	@Test fun exposesRemoteAndRegistrationRequiredStates()
	{
		val remoteRegistration = registered(2, "Office")
		val result = mergeHomeConsoles(
			listOf(ManualDisplayHost(null, ManualHost(host = "10.0.0.8", registeredHost = null))),
			listOf(PsnConsole(PsnDevice("duid", "Office"), remoteRegistration))
		)

		assertEquals(
			listOf(HomeConsoleStatus.REMOTE, HomeConsoleStatus.REGISTRATION_REQUIRED),
			result.map(HomeConsole::status)
		)
	}

	private fun discovered(name: String, hostId: String, registration: RegisteredHost? = null) = DiscoveredDisplayHost(
		registration,
		DiscoveryHost(
			DiscoveryHost.State.READY, 0U, "192.168.1.164", null,
			"00030010", name, null, hostId, null, null
		)
	)

	@Test fun unlinkedNetworkConsoleAndUnlinkedAccountConsoleAreOneRow()
	{
		val psn = PsnConsole(PsnDevice("duid", "PS5-466"), null)

		val result = mergeHomeConsoles(listOf(discovered("PS5-466", "C0151B3DD8BC")), listOf(psn))

		assertEquals(1, result.size)
		assertEquals(HomeConsoleStatus.REGISTRATION_REQUIRED, result.single().status)
		assertEquals(psn, result.single().psnConsole)
		assertEquals("192.168.1.164", result.single().displayHost?.host)
	}

	@Test fun linkedNetworkConsoleIsNotClaimedByAnUnlinkedAccountConsoleOfTheSameName()
	{
		val registration = registered(3, "PS5-466")
		val psn = PsnConsole(PsnDevice("duid", "PS5-466"), null)

		val result = mergeHomeConsoles(listOf(discovered("PS5-466", "000000000003", registration)), listOf(psn))

		assertEquals(2, result.size)
		assertEquals(null, result.first { it.displayHost != null }.psnConsole)
	}

	@Test fun differentlyNamedAccountConsoleStaysItsOwnRow()
	{
		val psn = PsnConsole(PsnDevice("duid", "Office"), null)

		val result = mergeHomeConsoles(listOf(discovered("PS5-466", "C0151B3DD8BC")), listOf(psn))

		assertEquals(2, result.size)
	}

	private fun unlinkedDiscovered(state: DiscoveryHost.State) = DiscoveredDisplayHost(
		null,
		DiscoveryHost(state, 0U, "192.168.1.9", null, "00030010", "Den", null, null, null, null)
	)

	@Test fun unlinkedConsoleInRestModeOffersWakeAndLink()
	{
		val resting = mergeHomeConsoles(listOf(unlinkedDiscovered(DiscoveryHost.State.STANDBY)), emptyList()).single()
		val awake = mergeHomeConsoles(listOf(unlinkedDiscovered(DiscoveryHost.State.READY)), emptyList()).single()

		assertEquals(HomeConsoleStatus.REGISTRATION_REQUIRED, resting.status)
		assertEquals(true, resting.isUnlinkedResting)
		assertEquals(false, awake.isUnlinkedResting)
	}
}
