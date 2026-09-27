// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package fi.madekivi.pleikkari.main

import fi.madekivi.pleikkari.common.RegisteredHost
import fi.madekivi.pleikkari.remote.PsnDevice

data class PsnConsole(
	val device: PsnDevice,
	val registeredHost: RegisteredHost?
)

internal fun matchPsnConsoles(
	devices: List<PsnDevice>,
	registeredHosts: List<RegisteredHost>
): List<PsnConsole>
{
	val registeredByName = registeredHosts
		.filter { !it.serverNickname.isNullOrBlank() }
		.associateBy { it.serverNickname!!.lowercase() }
	return devices.map { device ->
		PsnConsole(device, registeredByName[device.name.lowercase()])
	}
}

enum class PsnConsoleAction { PLAY }

data class PsnConsoleActionState(val duid: String, val action: PsnConsoleAction)

/**
 * @param justLinked this console was registered moments ago, so it may refuse the first session
 *   request while it settles (PLE-335).
 */
data class PsnPlayRequest(val console: PsnConsole, val justLinked: Boolean = false)

enum class PsnErrorRecovery { RETRY, SIGN_IN }

/** What Home does after a PSN link or play fails. */
internal enum class PsnFailureStep { SIGN_IN, LINK_WITH_PIN, RETRY }

/**
 * PLE-313: a failed PSN link to a console that is also on this network goes straight to the guided PIN
 * link, which works whatever PSN refused. Only a console out of reach is left with an error and Retry,
 * and an expired sign-in always asks to sign in again.
 */
internal fun psnFailureStep(recovery: PsnErrorRecovery, consoleOnLan: Boolean): PsnFailureStep = when
{
	recovery == PsnErrorRecovery.SIGN_IN -> PsnFailureStep.SIGN_IN
	consoleOnLan -> PsnFailureStep.LINK_WITH_PIN
	else -> PsnFailureStep.RETRY
}

data class PsnActionError(
	val message: String,
	val recovery: PsnErrorRecovery
)

enum class OnboardingHomeState
{
	WELCOME,
	ACCOUNT_CONSOLES,
	HOME
}

/**
 * The welcome screen is only for a first run with nothing to show: a console found on the network
 * is listed straight away, so a new user starts from their console rather than from a choice.
 */
internal fun onboardingHomeState(
	configuredConsoleCount: Int,
	psnSignedIn: Boolean,
	discoveredConsoleCount: Int = 0
): OnboardingHomeState = when
{
	configuredConsoleCount > 0 -> OnboardingHomeState.HOME
	psnSignedIn || discoveredConsoleCount > 0 -> OnboardingHomeState.ACCOUNT_CONSOLES
	else -> OnboardingHomeState.WELCOME
}

/** The unlinked console on the account that a sign-in started for [consoleName] should link. */
internal fun psnConsoleNamed(consoles: List<PsnConsole>, consoleName: String?): PsnConsole?
{
	val name = consoleName?.trim()?.lowercase()?.takeIf(String::isNotEmpty) ?: return null
	return consoles.firstOrNull { it.registeredHost == null && it.device.name.trim().lowercase() == name }
}

internal fun shouldLoadPsnConsoleList(
	psnRemotePlayEnabled: Boolean,
	psnSignInEnabled: Boolean,
	psnAccountId: String?
): Boolean = psnRemotePlayEnabled && psnSignInEnabled && !psnAccountId.isNullOrBlank()

/**
 * Whether enabling the PSN list should (re)load it. A failed list is retried too: after a
 * sign-in that fixed a SIGN_IN error, the list must not keep showing that error until restart.
 */
internal fun shouldReloadPsnConsoleList(state: PsnConsoleListState?): Boolean =
	state == PsnConsoleListState.Hidden || state is PsnConsoleListState.Error

/** PLE-271: a PSN list that loaded but holds no Remote Play console says so instead of staying silent. */
internal fun showPsnNoRemotePlayConsoles(state: PsnConsoleListState?, psnConsoles: List<PsnConsole>): Boolean =
	state == PsnConsoleListState.Ready && psnConsoles.isEmpty()

sealed interface PsnConsoleListState
{
	data object Hidden : PsnConsoleListState
	data object Loading : PsnConsoleListState
	data object Ready : PsnConsoleListState
	data class Error(val message: String, val recovery: PsnErrorRecovery) : PsnConsoleListState
}
