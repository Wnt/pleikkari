// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef PLK_HOST_AWDLHELPERCLIENT_H
#define PLK_HOST_AWDLHELPERCLIENT_H

#include <string>

namespace plk {

/**
 * The app's side of the AWDL helper (macos/awdl-helper): one connection to the launch daemon
 * in the app bundle, and its registration with macOS when it is not registered yet.
 *
 * All calls block for as long as the helper takes to answer (it answers at once) and belong
 * on one queue: AwdlGuard's.
 */
class AwdlHelperClient
{
public:
	enum class Result
	{
		Held,          // awdl0 is down and the helper keeps it down
		NeedsApproval, // registered, and waiting for the user to allow it in System Settings
		Unavailable,   // no helper: not an app bundle, an ad hoc build, or it failed
	};

	/**
	 * Asks the helper to keep awdl0 down for this process.
	 * @param may_register register the daemon when it is not registered, and show the user
	 *                     where to allow it (System Settings, Login Items) when that is needed
	 * @param detail       one line for the log about what happened, when there is one to say
	 */
	static Result hold(bool may_register, std::string &detail);
	/** Gives the hold back and closes the connection. The helper brings awdl0 back up. */
	static void release();
	/** false once the helper's connection is gone (it was switched off, it crashed). */
	static bool holding();
	/**
	 * The helper's registration with macOS, by hand: "status" changes nothing, "register"
	 * hands the daemon to macOS (and opens System Settings when it waits to be allowed),
	 * "unregister" takes it away again. Returns what to tell the user.
	 */
	static std::string manage(const std::string &action);
};

} // namespace plk

#endif // PLK_HOST_AWDLHELPERCLIENT_H
