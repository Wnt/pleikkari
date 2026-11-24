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
#include <vector>

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
	bool keep_awdl = false;          // leave AWDL up during a Wi-Fi stream
	bool idr_on_fec_failure = false;
	bool diagnostics = false;        // the lib's per-second stream stats in the log
	bool verbose = false;            // the lib's verbose log level

	// Window and video.
	bool fullscreen = false;
	video::PacingMode pacing = video::PacingMode::Immediate;
	// The four below can also be changed while the stream runs (the Video menu). A choice
	// made there is remembered and used by the next run that does not give the option.
	std::optional<video::Scaler> scaler;        // default bilinear
	std::optional<video::SdrColor> sdr_color;   // default accurate
	std::optional<video::HdrOutput> hdr_output; // default PQ
	std::optional<bool> hdr;         // HDR on or off, whatever --codec, Qt settings or default say
	bool saved_choices = true;       // read and remember the menu's choices
	std::string keys;                // test tool: "seconds:key,..." pressed by the app itself
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

/** One key of --keys: a character ("3", "v") or one of up, down, left, right, enter, esc, space. */
struct KeyPress
{
	double at_s = 0;
	std::string key;
};
/** "2:3,4.5:v,6:down" as presses in the order given; false with *error on a malformed list. */
bool parseKeyScript(const std::string &list, std::vector<KeyPress> &out, std::string *error);

/** What the Video menu changes, as the run starts with it. */
struct ViewChoices
{
	video::Scaler scaler = video::Scaler::Bilinear;
	video::SdrColor sdr_color = video::SdrColor::Accurate;
	video::HdrOutput hdr_output = video::HdrOutput::PQ;
};
ViewChoices resolveViewChoices(const Options &options);

/** The codec with HDR switched on or off: h265-hdr, or the SDR codec the stream had (h265 if it had none). */
ChiakiCodec codecWithHdr(ChiakiCodec codec, bool hdr, ChiakiCodec sdr_codec = CHIAKI_CODEC_H265);

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
