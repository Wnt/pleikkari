// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Command line of Pleikkari.app and how it turns into session settings.
//
//   Pleikkari stream <nickname> <host> [options]   stream from a registered console
//   Pleikkari replay <dump.plkv> [options]         play a PLKVDMP1 video dump through the
//                                                  same video path (no console)
//   Pleikkari list                                 registered consoles: nickname and type
//
// Stream defaults come from the Qt app's settings (loadQtStreamSettings) where it has
// them, otherwise from the Qt app's own defaults: 1080p, 60 fps, H.265 and the bitrate of
// the resolution preset. A command-line option always wins.

#ifndef PLK_APP_OPTIONS_H
#define PLK_APP_OPTIONS_H

#include "Registration.h"
#include "SessionHost.h"

#include <plk/VideoPipeline.h>

#include <optional>
#include <string>

namespace plk::app {

enum class Command { Help, Stream, Replay, List };

struct Options
{
	Command command = Command::Help;
	std::string nickname;            // stream
	std::string host;                // stream
	std::string replay_path;         // replay
	std::string profile;             // Qt settings profile (com.chiaki.Chiaki-<profile>.plist)

	// Stream settings given on the command line (unset: Qt settings, then defaults).
	std::optional<ChiakiVideoResolutionPreset> resolution;
	std::optional<ChiakiVideoFPSPreset> fps;
	std::optional<unsigned> bitrate_kbps;
	std::optional<ChiakiCodec> codec;
	std::optional<uint32_t> feedback_interval_ms; // lib default 8 ms
	bool disable_reordering = false;
	bool idr_on_fec_failure = false;
	bool diagnostics = false;        // the lib's per-second stream stats in the log
	bool verbose = false;            // the lib's verbose log level

	// Window and video.
	bool fullscreen = false;
	video::PacingMode pacing = video::PacingMode::Immediate;
	video::Scaler scaler = video::Scaler::Bilinear;
	bool hdr_linear = false;         // HDR to an extended-linear drawable instead of PQ
	bool headless = false;           // no window, no NSApplication: video renders offscreen
	int offscreen_width = 3456;      // headless target size (the panel's)
	int offscreen_height = 2234;

	// Controller.
	bool controller = true;
	bool controller_output = true;   // effects, LEDs, haptics (headless: always off)
	bool realtime_controller = true; // DualSense driver threads under the real-time policy
	std::optional<bool> bt_haptics;  // haptics over Bluetooth HID report 0x32
	bool usb_haptics = true;         // haptics over the USB DualSense's audio device
	std::optional<float> haptic_gain;

	// Audio.
	bool audio = true;

	// Logging and telemetry.
	std::string log_path;            // empty: stderr
	bool frame_log = true;           // one [frame] line per presented frame
	double stats_interval_s = 10.0;  // pipeline stats every N s; 0 = off
	std::string dump_path;           // record the session's video samples (PLKVDMP1)

	double retry_window_s = 20;      // restart a session that ends before it connects, for this long
	double seconds = 0;              // stop after N seconds (0 = run until closed/ended)
	bool loop = false;               // replay: start over at the end
};

/** false with *error set on a malformed command line. */
bool parseOptions(int argc, const char *const *argv, Options &out, std::string *error);
std::string usageText();

/** Everything the session needs, after merging command line, Qt settings and defaults. */
struct StreamConfig
{
	StreamSettings session;
	unsigned effective_bitrate_kbps = 0; // what the console is asked for
	int fps = 60;
	int width = 1920, height = 1080;
	bool bt_haptics = true;
	float haptic_gain = 1.0f;
	std::string source;                  // which values came from where, for the log
};

StreamConfig resolveStreamConfig(const Options &options, const QtStreamSettings *qt);

const char *codecName(ChiakiCodec codec);
const char *resolutionName(ChiakiVideoResolutionPreset resolution);
bool parseResolution(const std::string &s, ChiakiVideoResolutionPreset *out);
bool parseCodec(const std::string &s, ChiakiCodec *out);

} // namespace plk::app

#endif // PLK_APP_OPTIONS_H
