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

// A host name is not resolved (that could block the caller): the default route answers.
PLK_TEST(awdl_guard_host_name_does_not_resolve)
{
	const std::string by_name = plk::AwdlGuard::outgoingInterface("console.invalid");
	const std::string by_default = plk::AwdlGuard::outgoingInterface("1.1.1.1");
	CHECK(by_name == by_default);
}
