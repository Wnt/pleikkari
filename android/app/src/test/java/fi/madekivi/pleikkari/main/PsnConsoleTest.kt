// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.main

import fi.madekivi.pleikkari.common.MacAddress
import fi.madekivi.pleikkari.common.RegisteredHost
import fi.madekivi.pleikkari.lib.Target
import fi.madekivi.pleikkari.remote.PsnDevice
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class PsnConsoleTest
{
	@Test fun matchesPsnDeviceToRegisteredHostByNicknameIgnoringCase()
	{
		val registered = RegisteredHost(
			target = Target.PS5_1,
			apSsid = null,
			apBssid = null,
			apKey = null,
			apName = null,
			serverMac = MacAddress(1),
			serverNickname = "Living Room PS5",
			rpRegistKey = ByteArray(16),
			rpKeyType = 0,
			rpKey = ByteArray(16)
		)
		val devices = listOf(
			PsnDevice("one", "living room ps5"),
			PsnDevice("two", "Office PS5")
		)

		val result = matchPsnConsoles(devices, listOf(registered))

		assertEquals(registered, result[0].registeredHost)
		assertNull(result[1].registeredHost)
	}

	@Test fun firstLaunchShowsWelcomeUntilPsnIsChosen()
	{
		assertEquals(OnboardingHomeState.WELCOME, onboardingHomeState(0, false))
		assertEquals(OnboardingHomeState.ACCOUNT_CONSOLES, onboardingHomeState(0, true))
	}

	@Test fun consoleFoundOnTheNetworkSkipsWelcome()
	{
		assertEquals(OnboardingHomeState.ACCOUNT_CONSOLES, onboardingHomeState(0, false, 1))
		assertEquals(OnboardingHomeState.WELCOME, onboardingHomeState(0, false, 0))
	}

	@Test fun signInLinksOnlyAnUnlinkedAccountConsoleWithTheTappedName()
	{
		val unlinked = PsnConsole(PsnDevice("one", "PS5-466"), null)
		val other = PsnConsole(PsnDevice("two", "Office PS5"), null)

		assertEquals(unlinked, psnConsoleNamed(listOf(other, unlinked), " ps5-466 "))
		assertNull(psnConsoleNamed(listOf(other), "PS5-466"))
		assertNull(psnConsoleNamed(listOf(unlinked), null))
		assertNull(psnConsoleNamed(listOf(unlinked), ""))
	}

	@Test fun configuredConsoleAlwaysReturnsHome()
	{
		assertEquals(OnboardingHomeState.HOME, onboardingHomeState(1, false))
		assertEquals(OnboardingHomeState.HOME, onboardingHomeState(1, true))
	}

	/** PLE-313: a console on this network is linked with its PIN next; only an unreachable one shows Retry. */
	@Test fun failedPsnLinkToAConsoleOnTheNetworkGoesToThePinLink()
	{
		assertEquals(PsnFailureStep.LINK_WITH_PIN, psnFailureStep(PsnErrorRecovery.RETRY, consoleOnLan = true))
		assertEquals(PsnFailureStep.RETRY, psnFailureStep(PsnErrorRecovery.RETRY, consoleOnLan = false))
		assertEquals(PsnFailureStep.SIGN_IN, psnFailureStep(PsnErrorRecovery.SIGN_IN, consoleOnLan = true))
		assertEquals(PsnFailureStep.SIGN_IN, psnFailureStep(PsnErrorRecovery.SIGN_IN, consoleOnLan = false))
	}

	/** A sign-in after a failed list reloads it instead of leaving the stale "Sign in to PSN" error up. */
	@Test fun enablingPsnReloadsAHiddenOrFailedListOnly()
	{
		assertTrue(shouldReloadPsnConsoleList(PsnConsoleListState.Hidden))
		assertTrue(shouldReloadPsnConsoleList(PsnConsoleListState.Error("Sign in to PSN", PsnErrorRecovery.SIGN_IN)))
		assertTrue(shouldReloadPsnConsoleList(PsnConsoleListState.Error("offline", PsnErrorRecovery.RETRY)))
		assertFalse(shouldReloadPsnConsoleList(PsnConsoleListState.Loading))
		assertFalse(shouldReloadPsnConsoleList(PsnConsoleListState.Ready))
		assertFalse(shouldReloadPsnConsoleList(null))
	}

	@Test
	fun emptyReadyPsnListExplainsItself()
	{
		val one = listOf(PsnConsole(PsnDevice("one", "PS5-466"), null))
		assertTrue(showPsnNoRemotePlayConsoles(PsnConsoleListState.Ready, emptyList()))
		assertFalse(showPsnNoRemotePlayConsoles(PsnConsoleListState.Ready, one))
		assertFalse(showPsnNoRemotePlayConsoles(PsnConsoleListState.Loading, emptyList()))
		assertFalse(showPsnNoRemotePlayConsoles(PsnConsoleListState.Hidden, emptyList()))
		assertFalse(showPsnNoRemotePlayConsoles(PsnConsoleListState.Error("x", PsnErrorRecovery.RETRY), emptyList()))
		assertFalse(showPsnNoRemotePlayConsoles(null, emptyList()))
	}
}
