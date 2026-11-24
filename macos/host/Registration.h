// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Read-only loader for the console registrations the Qt app (chiaki-ng) stored.
//
// The Qt app keeps them in QSettings (organisation and application "Chiaki"), which on
// macOS is the preferences plist ~/Library/Preferences/com.chiaki.Chiaki.plist
// (com.chiaki.Chiaki-<profile>.plist for a named profile). QSettings flattens it like this:
//
//   - a group separator '/' becomes '.':      "settings/bitrate_local_ps5" -> "settings.bitrate_local_ps5"
//   - an array is 1-based with a size key:    "registered_hosts.size", "registered_hosts.1.server_nickname"
//   - QByteArray is CFData; QString is CFString; int and uint are CFNumber; bool is CFBoolean
//   - a QString that starts with '@' is escaped to "@@..."; other QVariant types are strings
//     "@Variant(...)" / "@ByteArray(...)" (a value imported from an .ini export can arrive
//     that way, and numbers imported from an .ini can arrive as strings)
//
// One registered host (gui/src/host.cpp, RegisteredHost::SaveToSettings) is
//   target (int, ChiakiTarget), server_nickname (string), server_mac (6 bytes),
//   rp_regist_key (16 bytes), rp_key (16 bytes, the "morning"), rp_key_type (uint),
//   ap_ssid/ap_bssid/ap_key/ap_name/console_pin (strings, not used here).
//
// THE FILE HOLDS SECRETS (registration keys, PSN tokens). This loader only reads the
// registered_hosts array, never writes, and never logs a value. Callers must not print
// anything from a RegisteredConsole except nickname and console type.

#ifndef PLK_HOST_REGISTRATION_H
#define PLK_HOST_REGISTRATION_H

#include <chiaki/common.h>
#include <chiaki/session.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace plk {

struct RegisteredConsole
{
	std::string nickname;
	ChiakiTarget target = CHIAKI_TARGET_PS4_UNKNOWN;
	std::array<uint8_t, 6> mac{};
	bool mac_valid = false;
	// rp_regist_key: what ChiakiConnectInfo::regist_key wants (16 bytes, NUL padded).
	std::array<char, CHIAKI_SESSION_AUTH_SIZE> regist_key{};
	bool regist_key_valid = false;
	// rp_key: what ChiakiConnectInfo::morning wants.
	std::array<uint8_t, 0x10> morning{};
	bool morning_valid = false;
	uint32_t rp_key_type = 0;

	RegisteredConsole() = default;
	RegisteredConsole(const RegisteredConsole &) = default;
	RegisteredConsole &operator=(const RegisteredConsole &) = default;
	~RegisteredConsole(); // wipes the keys

	bool isPS5() const { return chiaki_target_is_ps5(target); }
	/** Both keys are present with the right length: enough to start a LAN session. */
	bool usable() const { return regist_key_valid && morning_valid; }
	/** "PS5" or "PS4": the only thing besides the nickname that may be printed. */
	const char *consoleTypeName() const { return isPS5() ? "PS5" : "PS4"; }
};

/** $HOME/Library/Preferences/com.chiaki.Chiaki.plist, or ...Chiaki-<profile>.plist. */
std::string qtSettingsPlistPath(const std::string &profile = std::string());

/**
 * Parse the registered_hosts array out of a QSettings plist (binary or XML).
 * Entries are returned in array order, including ones whose keys are missing or have
 * the wrong length (usable() is false for those). Never modifies the file.
 *
 * @param error set to a description without any value from the file on failure
 * @return false if the file cannot be read or is not a property list dictionary
 */
bool loadRegisteredConsoles(const std::string &plist_path, std::vector<RegisteredConsole> &out, std::string *error = nullptr);

/** The first console with this nickname, or nullptr. */
const RegisteredConsole *findConsole(const std::vector<RegisteredConsole> &consoles, const std::string &nickname);

} // namespace plk

#endif // PLK_HOST_REGISTRATION_H
