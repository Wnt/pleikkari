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
// registered_hosts array and the named stream settings of QtStreamSettings, never writes,
// and never logs a value. Callers must not print
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

/**
 * The Qt app's non-secret stream settings for a PS5 on the LAN (Settings::GetVideoProfileLocalPS5()
 * and the DualSense settings), so a native client can start with what the user chose there.
 * Only these keys are read; a key that is missing or unreadable leaves its has_ flag false and
 * the field at the Qt app's default.
 */
struct QtStreamSettings
{
	bool has_resolution = false;
	std::string resolution = "1080p";      // settings/resolution_local_ps5: "360p", "540p", "720p", "1080p"
	bool has_fps = false;
	int fps = 60;                          // settings/fps_local_ps5: 30 or 60
	bool has_bitrate = false;
	unsigned bitrate_kbps = 0;             // settings/bitrate_local_ps5: 0 = the resolution preset's
	bool has_codec = false;
	std::string codec = "h265";            // settings/codec_local_ps5: "h264", "h265", "h265_hdr"
	bool has_haptic_gain = false;
	float haptic_gain = 1.0f;              // settings/haptic_override
	bool has_bt_haptics = false;
	bool bt_haptics = true;                // settings/dualsense_bt_haptics
	bool has_packet_loss_max = false;
	double packet_loss_max = 0.05;         // settings/packet_loss_reported_max (or the older packet_loss_max)
};

/** Read QtStreamSettings from a QSettings plist; false only if the file is not a readable dictionary. */
bool loadQtStreamSettings(const std::string &plist_path, QtStreamSettings &out, std::string *error = nullptr);

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

/**
 * The addresses of the Qt app's manually added consoles (manual_hosts), in array order.
 * This is where a console's address is remembered: a registration itself has none.
 */
bool loadManualHosts(const std::string &plist_path, std::vector<std::string> &out, std::string *error = nullptr);

/** The first console with this nickname, or nullptr. */
const RegisteredConsole *findConsole(const std::vector<RegisteredConsole> &consoles, const std::string &nickname);

} // namespace plk

#endif // PLK_HOST_REGISTRATION_H
