// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// The registration loader against a synthetic QSettings plist (tests/fixtures), in the
// XML form it is committed in and the binary form QSettings writes.

#include "Registration.h"
#include "TestHarness.h"

#include <CoreFoundation/CoreFoundation.h>

#include <cstring>
#include <fstream>
#include <iterator>

using plk::RegisteredConsole;

namespace {

std::string fixturePath()
{
	return std::string(PLK_TEST_FIXTURE_DIR) + "/qsettings-registered-hosts.xml";
}

std::vector<uint8_t> readAll(const std::string &path)
{
	std::ifstream f(path, std::ios::binary);
	return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

bool writeAll(const std::string &path, const void *data, size_t size)
{
	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	f.write(static_cast<const char *>(data), (std::streamsize)size);
	return (bool)f;
}

bool writeText(const std::string &path, const std::string &text)
{
	return writeAll(path, text.data(), text.size());
}

/** Re-encode a property list file as a binary plist, as QSettings stores it. */
bool convertToBinary(const std::string &in, const std::string &out)
{
	std::vector<uint8_t> raw = readAll(in);
	if(raw.empty())
		return false;
	CFDataRef data = CFDataCreate(kCFAllocatorDefault, raw.data(), (CFIndex)raw.size());
	CFPropertyListRef plist = CFPropertyListCreateWithData(kCFAllocatorDefault, data, kCFPropertyListImmutable, nullptr, nullptr);
	CFRelease(data);
	if(!plist)
		return false;
	CFDataRef bin = CFPropertyListCreateData(kCFAllocatorDefault, plist, kCFPropertyListBinaryFormat_v1_0, 0, nullptr);
	CFRelease(plist);
	if(!bin)
		return false;
	bool ok = writeAll(out, CFDataGetBytePtr(bin), (size_t)CFDataGetLength(bin));
	CFRelease(bin);
	return ok;
}

const std::string kPlistHead =
	"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	"<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
	"<plist version=\"1.0\">\n";
const std::string kPlistTail = "</plist>\n";

void checkFixture(const std::vector<RegisteredConsole> &c)
{
	CHECK(c.size() == 3);
	if(c.size() != 3)
		return;

	// 1: native types
	CHECK(c[0].nickname == "Fake PS5");
	CHECK(c[0].target == CHIAKI_TARGET_PS5_1);
	CHECK(c[0].isPS5());
	CHECK(strcmp(c[0].consoleTypeName(), "PS5") == 0);
	CHECK(c[0].mac_valid);
	const uint8_t mac[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
	CHECK(memcmp(c[0].mac.data(), mac, 6) == 0);
	CHECK(c[0].regist_key_valid);
	bool regist_ok = true, morning_ok = true;
	for(size_t i = 0; i < 16; i++)
	{
		regist_ok = regist_ok && (uint8_t)c[0].regist_key[i] == 0x10 + i;
		morning_ok = morning_ok && c[0].morning[i] == 0xa0 + i;
	}
	CHECK(regist_ok);
	CHECK(c[0].morning_valid);
	CHECK(morning_ok);
	CHECK(c[0].rp_key_type == 2);
	CHECK(c[0].usable());

	// 2: string forms, escaped nickname
	CHECK(c[1].nickname == "@Den");
	CHECK(c[1].target == CHIAKI_TARGET_PS4_10);
	CHECK(!c[1].isPS5());
	CHECK(strcmp(c[1].consoleTypeName(), "PS4") == 0);
	CHECK(c[1].mac_valid && memcmp(c[1].mac.data(), "ABCDEF", 6) == 0);
	CHECK(c[1].regist_key_valid && memcmp(c[1].regist_key.data(), "FAKEREGISTKEY016", 16) == 0);
	CHECK(c[1].morning_valid && memcmp(c[1].morning.data(), "FAKEMORNINGKEY16", 16) == 0);
	CHECK(c[1].rp_key_type == 1);
	CHECK(c[1].usable());

	// 3: wrong key length, missing key, nickname as data
	CHECK(c[2].nickname == "Fake");
	CHECK(c[2].target == CHIAKI_TARGET_PS5_UNKNOWN);
	CHECK(c[2].isPS5());
	CHECK(!c[2].mac_valid);
	CHECK(!c[2].regist_key_valid);
	CHECK(!c[2].morning_valid);
	CHECK(!c[2].usable());
}

} // namespace

PLK_TEST(registration_xml_fixture)
{
	std::vector<RegisteredConsole> consoles;
	std::string error;
	REQUIRE(plk::loadRegisteredConsoles(fixturePath(), consoles, &error));
	checkFixture(consoles);
}

PLK_TEST(registration_binary_fixture)
{
	const std::string bin = plktest::tempDir() + "/qsettings-binary.plist";
	REQUIRE(convertToBinary(fixturePath(), bin));
	std::vector<uint8_t> head = readAll(bin);
	REQUIRE(head.size() > 8);
	CHECK(memcmp(head.data(), "bplist00", 8) == 0);
	std::vector<RegisteredConsole> consoles;
	std::string error;
	REQUIRE(plk::loadRegisteredConsoles(bin, consoles, &error));
	checkFixture(consoles);
}

PLK_TEST(registration_find)
{
	std::vector<RegisteredConsole> consoles;
	REQUIRE(plk::loadRegisteredConsoles(fixturePath(), consoles));
	const RegisteredConsole *den = plk::findConsole(consoles, "@Den");
	REQUIRE(den);
	CHECK(den == &consoles[1]);
	CHECK(plk::findConsole(consoles, "Beyond size") == nullptr);
	CHECK(plk::findConsole(consoles, "fake ps5") == nullptr); // exact match only
}

PLK_TEST(registration_missing_file)
{
	std::vector<RegisteredConsole> consoles;
	std::string error;
	CHECK(!plk::loadRegisteredConsoles(plktest::tempDir() + "/does-not-exist.plist", consoles, &error));
	CHECK(!error.empty());
	CHECK(consoles.empty());
}

PLK_TEST(registration_not_a_plist)
{
	const std::string path = plktest::tempDir() + "/garbage.plist";
	REQUIRE(writeText(path, "this is not a property list"));
	std::vector<RegisteredConsole> consoles;
	std::string error;
	CHECK(!plk::loadRegisteredConsoles(path, consoles, &error));
	CHECK(!error.empty());
	// A property list whose root is not a dictionary is not QSettings' either.
	REQUIRE(writeText(path, kPlistHead + "<array><string>x</string></array>\n" + kPlistTail));
	error.clear();
	CHECK(!plk::loadRegisteredConsoles(path, consoles, &error));
	CHECK(!error.empty());
}

PLK_TEST(registration_no_hosts)
{
	const std::string path = plktest::tempDir() + "/no-hosts.plist";
	REQUIRE(writeText(path, kPlistHead + "<dict><key>settings.audio_volume</key><integer>128</integer></dict>\n" + kPlistTail));
	std::vector<RegisteredConsole> consoles(1);
	CHECK(plk::loadRegisteredConsoles(path, consoles));
	CHECK(consoles.empty());
}

PLK_TEST(registration_size_beyond_entries)
{
	// QSettings reads missing array entries as empty values; so does the loader.
	const std::string path = plktest::tempDir() + "/short-array.plist";
	REQUIRE(writeText(path, kPlistHead +
		"<dict>"
		"<key>registered_hosts.size</key><integer>2</integer>"
		"<key>registered_hosts.1.server_nickname</key><string>Only</string>"
		"<key>registered_hosts.1.target</key><integer>1000100</integer>"
		"</dict>\n" + kPlistTail));
	std::vector<RegisteredConsole> consoles;
	REQUIRE(plk::loadRegisteredConsoles(path, consoles));
	REQUIRE(consoles.size() == 2);
	CHECK(consoles[0].nickname == "Only");
	CHECK(!consoles[0].usable());
	CHECK(consoles[1].nickname.empty());
	CHECK(!consoles[1].usable());
}

PLK_TEST(registration_plist_path)
{
	const std::string suffix = "/Library/Preferences/com.chiaki.Chiaki.plist";
	const std::string p = plk::qtSettingsPlistPath();
	CHECK(p.size() > suffix.size() && p.compare(p.size() - suffix.size(), suffix.size(), suffix) == 0);
	const std::string q = plk::qtSettingsPlistPath("work");
	const std::string qsuffix = "/Library/Preferences/com.chiaki.Chiaki-work.plist";
	CHECK(q.size() > qsuffix.size() && q.compare(q.size() - qsuffix.size(), qsuffix.size(), qsuffix) == 0);
}
