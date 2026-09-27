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
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * PLE-729: PLE-659's `auto_connect_host` extra reaching a connect. MainActivity hands the extra to
 * [PendingAutoPlay] and offers it every console list [joinDisplayHosts] builds; the [PendingAutoPlay.Play]
 * it returns is what `playLocalConsole` streams from.
 */
class PendingAutoPlayTest
{
	private companion object
	{
		const val ADDRESS = "192.168.1.164"
		/** PS5-466's discovery host id, and its registered_host.server_mac as the Oculus Go stores it (PLE-691). */
		const val HOST_ID = "C0151B3DD8BC"
		const val GO_SERVER_MAC = 207636924143040L
	}

	private fun registered(id: Long, mac: Long, name: String, target: Target = Target.PS5_1) = RegisteredHost(
		id = id,
		target = target,
		apSsid = null,
		apBssid = null,
		apKey = null,
		apName = null,
		serverMac = MacAddress(mac),
		serverNickname = name,
		rpRegistKey = ByteArray(16),
		rpKeyType = 0,
		rpKey = ByteArray(16)
	)

	private fun discovered(address: String = ADDRESS, hostId: String = HOST_ID, name: String = "PS5-466") = DiscoveryHost(
		DiscoveryHost.State.READY, 997U, address, "14000039", "00030010", name, "PS5", hostId, null, null
	)

	private val ps5 = registered(1, GO_SERVER_MAC, "PS5-466")

	@Test
	fun theExtraConnectsOnceItsConsoleIsDiscoveredAndRegistered()
	{
		val pending = PendingAutoPlay()
		pending.request(ADDRESS)
		assertEquals(ADDRESS, pending.address)
		// Discovery has not answered yet; the registration alone is not a host to stream from.
		assertNull(pending.take(joinDisplayHosts(emptyList(), listOf(ps5), emptyList())))
		val play = pending.take(joinDisplayHosts(emptyList(), listOf(ps5), listOf(discovered())))
		val host = play!!.host
		assertTrue(host is DiscoveredDisplayHost)
		assertEquals(ADDRESS, host.host)
		assertEquals(ps5, host.registeredHost)
		assertFalse(play.justLinked)
		// Once only: the next discovery update, or the list after the stream, does not connect again.
		assertNull(pending.address)
		assertNull(pending.take(joinDisplayHosts(emptyList(), listOf(ps5), listOf(discovered()))))
	}

	@Test
	fun theGosByteReversedServerMacIsTheDiscoveredConsole()
	{
		val host = joinDisplayHosts(emptyList(), listOf(ps5), listOf(discovered())).single()
		assertEquals(ps5, host.registeredHost)
		assertEquals(MacAddress(GO_SERVER_MAC), MacAddress(byteArrayOf(0xC0.toByte(), 0x15, 0x1B, 0x3D, 0xD8.toByte(), 0xBC.toByte())))
	}

	@Test
	fun anUnregisteredOrOtherConsoleIsNotStreamedAndTheRequestWaits()
	{
		val pending = PendingAutoPlay()
		pending.request(ADDRESS)
		// PS5-466 answers before its registration is loaded: not yet.
		assertNull(pending.take(joinDisplayHosts(emptyList(), emptyList(), listOf(discovered()))))
		// A registered console elsewhere, and an unregistered one at another address: not these.
		val other = registered(2, 0x010203040506L, "Bedroom")
		assertNull(pending.take(joinDisplayHosts(emptyList(), listOf(ps5, other),
			listOf(discovered("192.168.1.99", "060504030201", "Bedroom"), discovered("192.168.1.50", "AABBCCDDEEFF", "PS5-999")))))
		assertEquals(ADDRESS, pending.address)
		assertEquals(ADDRESS, pending.take(joinDisplayHosts(emptyList(), listOf(ps5), listOf(discovered())))!!.host.host)
	}

	@Test
	fun aManualConsoleWithItsRegistrationIsStreamedToo()
	{
		val pending = PendingAutoPlay()
		pending.request(ADDRESS)
		val play = pending.take(joinDisplayHosts(listOf(ManualHost(7, ADDRESS, ps5.id)), listOf(ps5), emptyList()))
		assertTrue(play!!.host is ManualDisplayHost)
		assertEquals(ps5, play.host.registeredHost)
	}

	@Test
	fun aJustLinkedConsoleKeepsItsSettleFlagForOnePlayOnly()
	{
		val pending = PendingAutoPlay()
		pending.request(ADDRESS, justLinked = true)
		assertTrue(pending.take(joinDisplayHosts(emptyList(), listOf(ps5), listOf(discovered())))!!.justLinked)
		pending.request(ADDRESS)
		assertFalse(pending.take(joinDisplayHosts(emptyList(), listOf(ps5), listOf(discovered())))!!.justLinked)
	}

	@Test
	fun aBlankOrMissingAddressIsNothingPending()
	{
		val pending = PendingAutoPlay()
		for(address in listOf(null, "", " "))
		{
			pending.request(address)
			assertNull(pending.address)
			// A discovered host with no address has host "", which a blank request must not match.
			assertNull(pending.take(listOf(DiscoveredDisplayHost(ps5, discovered().copy(hostAddr = null)))))
		}
	}

	@Test
	fun aPsnListedConsoleIsMatchedByNameOrDuid()
	{
		val pending = PendingAutoPlay()
		val device = PsnDevice("DUID123", "PS5-466")
		pending.request("ps5-466")
		// Unregistered on this client: not streamable, keep waiting.
		assertNull(pending.takePsn(listOf(PsnConsole(device, null))))
		assertNull(pending.takePsn(listOf(PsnConsole(PsnDevice("X", "Other"), ps5))))
		val play = pending.takePsn(listOf(PsnConsole(device, ps5)))!!
		assertEquals(device, play.console.device)
		assertNull(pending.address)
		pending.request("duid123")
		assertEquals(device, pending.takePsn(listOf(PsnConsole(device, ps5)))!!.console.device)
		// An IP address never matches a PSN console.
		pending.request(ADDRESS)
		assertNull(pending.takePsn(listOf(PsnConsole(device, ps5))))
	}
}
