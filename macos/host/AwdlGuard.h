// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef PLK_HOST_AWDLGUARD_H
#define PLK_HOST_AWDLGUARD_H

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace plk {

/**
 * Keeps Apple Wireless Direct Link down while a stream runs over Wi-Fi: straight over it, or
 * inside a VPN tunnel that does.
 *
 * AWDL (AirDrop, AirPlay, Handoff) shares the Wi-Fi radio: while awdl0 is up the radio leaves
 * the access point's channel for up to 100 ms every 0.5 to 2 s. Measured on a 1080p60 stream
 * with the Qt client: ping to the console 4 ms with every sixth one between 30 and 95 ms, six
 * 95 ms stops in the picture every 10 s and 65 of 600 frames never shown; with awdl0 down no
 * stop and every frame shown. A stream through a WireGuard tunnel over Wi-Fi, before the guard
 * looked under tunnels: a 100 ms stop every 0.52 s (207 in 121 s) and about 40 of 60 frames a
 * second shown.
 *
 * Taking awdl0 down needs root. The first time, the system's password dialog is shown once and
 * a sudoers rule is installed, checked with visudo first, that allows exactly
 * "ifconfig awdl0 down" and "ifconfig awdl0 up" without a password
 * (/private/etc/sudoers.d/chiaki-awdl, the rule the Qt client installs too; delete the file to
 * undo). After that the interface is taken down when a Wi-Fi stream starts, taken down again
 * within a second whenever macOS raises it, and brought back up three seconds after the last
 * stream ended or when the process exits.
 *
 * One guard serves the process and counts streams. All of it runs on a queue of its own:
 * acquire() and release() return at once and may be called from any thread.
 */
class AwdlGuard
{
public:
	static AwdlGuard &instance();

	/**
	 * A stream to host starts.
	 * @param host         IPv4/IPv6 literal or host name, as the session connects to it
	 * @param allow_prompt show the password dialog when the rule is not installed yet
	 * @return true when the stream leaves over Wi-Fi, as it is or inside a tunnel: release()
	 *         has to follow
	 */
	bool acquire(const std::string &host, bool allow_prompt = true);
	/** A stream that acquire() answered true for has ended. */
	void release();
	/** Brings the interface back up now if this process took it down. Runs at exit by itself. */
	void restoreNow();

	// The pieces, for tests and tools.
	/** Name of the interface a packet to host would leave from; "" if unknown. Sends nothing. */
	static std::string outgoingInterface(const std::string &host);
	/**
	 * The hardware interface the packets of ifname are sent from: ifname itself when it is
	 * one, and for a tunnel (a VPN's utun, ipsec or ppp interface) the interface of the first
	 * network service, in the system's order, that is connected through a router. That is the
	 * interface macOS sends a tunnel's own packets from. "" for anything else (loopback, an
	 * interface that does not exist) and when no such service is up.
	 */
	static std::string physicalInterface(const std::string &ifname);
	/**
	 * The choice physicalInterface() makes for a tunnel.
	 * @param order    service identifiers in the system's order of preference
	 * @param active   (service identifier, interface) of the services connected through a router
	 * @param hardware is this interface one of the machine's own (and not a tunnel)?
	 */
	static std::string firstHardwareInterface(const std::vector<std::string> &order,
			const std::vector<std::pair<std::string, std::string>> &active,
			const std::function<bool(const std::string &)> &hardware);
	static bool isWifi(const std::string &ifname);
	/** 1 up, 0 down, -1 no such interface */
	static int awdlIsUp();

private:
	AwdlGuard();
	struct Impl;
	Impl *const impl_;
};

} // namespace plk

#endif // PLK_HOST_AWDLGUARD_H
