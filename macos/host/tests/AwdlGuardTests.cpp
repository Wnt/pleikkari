// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "AwdlGuard.h"
#include "TestHarness.h"

// Only the parts that change nothing on the machine: a stream to the loopback address does
// not leave over Wi-Fi, so the guard must leave AWDL alone and ask for no release.
PLK_TEST(awdl_guard_loopback_is_not_wifi)
{
	CHECK(plk::AwdlGuard::outgoingInterface("127.0.0.1") == "lo0");
	CHECK(plk::AwdlGuard::outgoingInterface("::1") == "lo0");
	CHECK(!plk::AwdlGuard::isWifi("lo0"));
	CHECK(!plk::AwdlGuard::isWifi("no-such-interface0"));
	const int before = plk::AwdlGuard::awdlIsUp();
	CHECK(!plk::AwdlGuard::instance().acquire("127.0.0.1", false));
	CHECK(plk::AwdlGuard::awdlIsUp() == before);
}

// What carries a tunnel: the first service in the system's order that is up on one of the
// machine's own interfaces. The tunnel's own service and another tunnel are passed over.
PLK_TEST(awdl_guard_tunnel_is_carried_by_the_first_hardware_service)
{
	const auto hardware = [](const std::string &ifname) { return ifname.compare(0, 2, "en") == 0; };
	typedef std::vector<std::pair<std::string, std::string>> Services;
	const std::vector<std::string> order = {"ethernet", "wifi", "vpn"};

	// The case of 2026-11-25: Wi-Fi and a WireGuard tunnel over it.
	CHECK(plk::AwdlGuard::firstHardwareInterface(order, Services{{"vpn", "utun6"}, {"wifi", "en0"}}, hardware) == "en0");
	// A cable is ahead of Wi-Fi in the order, whichever the store lists first.
	CHECK(plk::AwdlGuard::firstHardwareInterface(order, Services{{"wifi", "en0"}, {"vpn", "utun6"}, {"ethernet", "en7"}}, hardware) == "en7");
	// And behind it when the order says so.
	CHECK(plk::AwdlGuard::firstHardwareInterface({"wifi", "ethernet"}, Services{{"ethernet", "en7"}, {"wifi", "en0"}}, hardware) == "en0");
	// A service the order does not know comes last; with nothing else up it is the one.
	CHECK(plk::AwdlGuard::firstHardwareInterface(order, Services{{"phone", "en9"}, {"wifi", "en0"}}, hardware) == "en0");
	CHECK(plk::AwdlGuard::firstHardwareInterface(order, Services{{"phone", "en9"}, {"vpn", "utun6"}}, hardware) == "en9");
	// Tunnels only, or nothing: no answer.
	CHECK(plk::AwdlGuard::firstHardwareInterface(order, Services{{"vpn", "utun6"}, {"other", "ipsec0"}}, hardware).empty());
	CHECK(plk::AwdlGuard::firstHardwareInterface(order, Services{}, hardware).empty());
	CHECK(plk::AwdlGuard::firstHardwareInterface({}, Services{{"wifi", "en0"}}, hardware) == "en0");
}

// Loopback is neither hardware nor a tunnel, and an interface of the machine's own is itself.
PLK_TEST(awdl_guard_physical_interface_of_what_is_not_a_tunnel)
{
	CHECK(plk::AwdlGuard::physicalInterface("lo0").empty());
	CHECK(plk::AwdlGuard::physicalInterface("").empty());
	CHECK(plk::AwdlGuard::physicalInterface("no-such-interface0").empty());
	// Whatever the default route leaves from on this machine: if it is one of the machine's
	// own interfaces it answers itself, and a tunnel never answers a tunnel.
	const std::string out = plk::AwdlGuard::outgoingInterface("1.1.1.1");
	const std::string physical = plk::AwdlGuard::physicalInterface(out);
	CHECK(physical.empty() || plk::AwdlGuard::physicalInterface(physical) == physical);
}

// A host name is not resolved (that could block the caller): the default route answers.
PLK_TEST(awdl_guard_host_name_does_not_resolve)
{
	const std::string by_name = plk::AwdlGuard::outgoingInterface("console.invalid");
	const std::string by_default = plk::AwdlGuard::outgoingInterface("1.1.1.1");
	CHECK(by_name == by_default);
}
