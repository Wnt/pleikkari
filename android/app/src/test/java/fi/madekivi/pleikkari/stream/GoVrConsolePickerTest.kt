// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.stream

import fi.madekivi.pleikkari.common.MacAddress
import fi.madekivi.pleikkari.common.ManualHost
import fi.madekivi.pleikkari.common.RegisteredHost
import fi.madekivi.pleikkari.lib.DiscoveryHost
import fi.madekivi.pleikkari.lib.Target
import fi.madekivi.pleikkari.stream.GoVrConsolePicker.Pick
import fi.madekivi.pleikkari.stream.GoVrConsolePicker.Presence
import fi.madekivi.pleikkari.stream.GoVrConsolePicker.Route
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class GoVrConsolePickerTest
{
	private fun registered(id: Long, name: String?, target: Target = Target.PS5_1) = RegisteredHost(
		id = id,
		target = target,
		apSsid = null,
		apBssid = null,
		apKey = null,
		apName = null,
		serverMac = MacAddress(0x10L + id),
		serverNickname = name,
		rpRegistKey = ByteArray(16),
		rpKeyType = 0,
		rpKey = ByteArray(16)
	)

	private fun discovered(console: RegisteredHost, state: DiscoveryHost.State, address: String? = "192.168.1.164", ps5: Boolean = true) =
		DiscoveryHost(
			state, 0U, address, null, if(ps5) "00030010" else "00020020",
			console.serverNickname, null, console.serverMac.toString().replace(":", ""), null, null
		)

	@Test
	fun theOnlyLinkedPs5IsStreamedWithoutAsking()
	{
		val ps5 = registered(1, "PS5-466")
		assertEquals(Pick.Connect(ps5, lastUsed = false), GoVrConsolePicker.pick(listOf(ps5), null))
		// A PS4 registration left over from before PLE-265 is not a choice.
		assertEquals(Pick.Connect(ps5, lastUsed = false),
			GoVrConsolePicker.pick(listOf(registered(2, "Old PS4", Target.PS4_10), ps5), null))
	}

	@Test
	fun theLastUsedConsoleWinsOverTheChooser()
	{
		val living = registered(1, "Living Room")
		val bedroom = registered(2, "Bedroom")
		assertEquals(Pick.Connect(bedroom, lastUsed = true),
			GoVrConsolePicker.pick(listOf(living, bedroom), bedroom.serverMac))
	}

	@Test
	fun severalConsolesWithNoLastUsedOneOpenTheChooserByName()
	{
		val living = registered(1, "living Room")
		val bedroom = registered(2, "Bedroom")
		val gone = MacAddress(0x99L)
		assertEquals(Pick.Choose(listOf(bedroom, living)), GoVrConsolePicker.pick(listOf(living, bedroom), gone))
		assertEquals(Pick.Choose(listOf(bedroom, living)), GoVrConsolePicker.pick(listOf(living, bedroom), null))
	}

	@Test
	fun noLinkedPs5PointsAtLinking()
	{
		assertEquals(Pick.NoConsole, GoVrConsolePicker.pick(emptyList(), null))
		assertEquals(Pick.NoConsole, GoVrConsolePicker.pick(listOf(registered(1, "PS4", Target.PS4_10)), null))
	}

	@Test
	fun aConsoleWithoutNicknameIsNamedByItsMac()
	{
		assertEquals("11:00:00:00:00:00", GoVrConsolePicker.name(registered(1, null)))
		assertEquals("11:00:00:00:00:00", GoVrConsolePicker.name(registered(1, " ")))
	}

	@Test
	fun discoveryDecidesConnectOrWake()
	{
		val ps5 = registered(1, "PS5-466")
		assertEquals(Route.Ready("192.168.1.164"),
			GoVrConsolePicker.route(ps5, listOf(discovered(ps5, DiscoveryHost.State.READY)), emptyList()))
		assertEquals(Route.Standby("192.168.1.164"),
			GoVrConsolePicker.route(ps5, listOf(discovered(ps5, DiscoveryHost.State.STANDBY)), emptyList()))
		assertEquals(Route.Unknown,
			GoVrConsolePicker.route(ps5, listOf(discovered(ps5, DiscoveryHost.State.UNKNOWN)), emptyList()))
	}

	@Test
	fun otherConsolesAndAddresslessAnswersAreIgnored()
	{
		val ps5 = registered(1, "PS5-466")
		val other = registered(2, "Neighbour")
		assertEquals(Route.Unknown, GoVrConsolePicker.route(ps5, listOf(
			discovered(other, DiscoveryHost.State.READY),
			discovered(ps5, DiscoveryHost.State.READY, address = null),
			discovered(ps5, DiscoveryHost.State.READY, ps5 = false)
		), emptyList()))
	}

	@Test
	fun aManualAddressIsTheFallbackWhenDiscoveryIsSilent()
	{
		val ps5 = registered(1, "PS5-466")
		val manual = listOf(ManualHost(1, "10.0.0.5", registeredHost = 2), ManualHost(2, "10.0.0.9", registeredHost = 1))
		assertEquals(Route.Manual("10.0.0.9"), GoVrConsolePicker.route(ps5, emptyList(), manual))
		// Discovery still wins: it knows whether the console is awake.
		assertEquals(Route.Standby("192.168.1.164"),
			GoVrConsolePicker.route(ps5, listOf(discovered(ps5, DiscoveryHost.State.STANDBY)), manual))
	}

	@Test
	fun aHomeCardSaysNotFoundOnlyAfterDiscoveryHadItsTime()
	{
		val ps5 = registered(1, "PS5-466")
		val presence = { discovered: List<DiscoveryHost>, manual: List<ManualHost>, stillLooking: Boolean ->
			GoVrConsolePicker.presence(GoVrConsolePicker.route(ps5, discovered, manual), stillLooking)
		}
		assertEquals(Presence.READY, presence(listOf(discovered(ps5, DiscoveryHost.State.READY)), emptyList(), false))
		assertEquals(Presence.STANDBY, presence(listOf(discovered(ps5, DiscoveryHost.State.STANDBY)), emptyList(), true))
		assertEquals(Presence.MANUAL, presence(emptyList(), listOf(ManualHost(1, "10.0.0.9", registeredHost = 1)), false))
		assertEquals(Presence.LOOKING, presence(emptyList(), emptyList(), true))
		assertEquals(Presence.NOT_FOUND, presence(emptyList(), emptyList(), false))
	}

	@Test
	fun theChooserStartsAtTheLastUsedConsoleAndWrapsThroughTheTvScreenRow()
	{
		val consoles = listOf(registered(1, "A"), registered(2, "B"))
		val chooser = GoVrConsolePicker.Chooser(consoles, consoles[1].serverMac)
		assertEquals(3, chooser.size)
		assertEquals(consoles[1], chooser.selected)
		chooser.move(1)
		assertNull(chooser.selected) // the Oculus TV screen row
		chooser.move(1)
		assertEquals(consoles[0], chooser.selected)
		chooser.move(-1)
		assertNull(chooser.selected)
		assertEquals(consoles[0], GoVrConsolePicker.Chooser(consoles, null).selected)
	}

	@Test
	fun theChooserListsEveryLinkedPs5ByName()
	{
		val living = registered(1, "living Room")
		val bedroom = registered(2, "Bedroom")
		assertEquals(listOf(bedroom, living),
			GoVrConsolePicker.consoles(listOf(living, registered(3, "Old PS4", Target.PS4_10), bedroom)))
	}

	/** After a Disconnect (or with debug.pleikkari.go_entry_choose) even one console gets the chooser, for its Settings row. */
	@Test
	fun aOneConsoleChooserStartsOnItAndReachesTheTvScreenRow()
	{
		val ps5 = registered(1, "PS5-466")
		val chooser = GoVrConsolePicker.Chooser(GoVrConsolePicker.consoles(listOf(ps5)), ps5.serverMac)
		assertEquals(2, chooser.size)
		assertEquals(ps5, chooser.selected)
		chooser.move(1)
		assertNull(chooser.selected)
		chooser.move(1)
		assertEquals(ps5, chooser.selected)
	}

	@Test
	fun theChooserShowsAWindowAroundTheSelection()
	{
		val consoles = (1L..9L).map { registered(it, "PS5 $it") }
		val chooser = GoVrConsolePicker.Chooser(consoles, null)
		assertEquals((0..5).toList(), chooser.window().map { it.first })
		repeat(6) { chooser.move(1) }
		assertEquals((3..8).toList(), chooser.window().map { it.first })
		chooser.move(-7) // wraps to the TV screen row, the tenth
		assertEquals(9, chooser.index)
		assertEquals((4..9).toList(), chooser.window().map { it.first })
		assertNull(chooser.window().last().second)
	}
}
