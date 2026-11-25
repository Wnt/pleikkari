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

// Neural rendering (macos/nr): the two networks of an NVIDIA DLSS neural rendering model.
// The small one is NVIDIA's own default and about twice as fast; HT is the original one.
enum class NrNetwork { Small, HT };
const char *toString(NrNetwork network);
bool parseNrNetwork(const std::string &s, NrNetwork *out);
// The look the network is asked for (its rendering profiles). Neutral switches its tone
// and structure changes off: a diagnostic.
enum class NrLook { Standard, Natural, Cinematic, Neutral };
const char *toString(NrLook look);
bool parseNrLook(const std::string &s, NrLook *out);
NrLook nextNrLook(NrLook look);

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
	video::PacingMode pacing = video::PacingMode::Immediate; // see effectivePacing()
	// The four below can also be changed while the stream runs (the Video menu). A choice
	// made there is remembered and used by the next run that does not give the option.
	std::optional<video::Scaler> scaler;        // default bilinear
	std::optional<video::SdrColor> sdr_color;   // default accurate
	std::optional<video::HdrOutput> hdr_output; // default PQ
	std::optional<int> hdr_brightness;          // percent, default 100
	std::optional<video::HdrToneMap> hdr_tone_map; // default app
	std::optional<bool> fullscreen_sync;        // default on, see effectivePacing()
	std::optional<bool> hdr;         // HDR on or off, whatever --codec, Qt settings or default say
	// Neural rendering of the picture; these five are in the Video menu and remembered too.
	std::optional<bool> nr;                 // default on in a window when engine and model are there
	std::optional<NrNetwork> nr_network;    // default small
	std::optional<int> nr_height;           // height of the picture the network gets; 0 = the stream's
	std::optional<int> nr_strength;         // percent of the network's change that is shown
	std::optional<NrLook> nr_look;          // default standard
	std::string nr_library;          // libplknr.dylib (default: the one in the app bundle)
	std::string nr_models;           // directory of the .dlssmodel packages (default: defaultNrModelsDirectory())
	std::string nr_model;            // one package, whatever the network choice says
	bool saved_choices = true;       // read and remember the menu's choices
	std::string keys;                // test tool: "seconds:key,..." pressed by the app itself
	bool headless = false;           // no window, no NSApplication: video renders offscreen
	int offscreen_width = 3456;      // headless target size (the panel's)
	int offscreen_height = 2234;
	std::string capture_dir;         // headless: rendered frames are written here at the end
	uint64_t capture_skip = 0;       // rendered frames to let pass first
	unsigned capture_frames = 120;
	unsigned capture_every = 1;

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

// Brightness of an HDR picture in percent (a factor on its linear light), and the step of the
// menu's brighter and darker. 100 %: HDR reference white (203 nits) as bright as the
// display's SDR white when the app does the tone mapping; with macOS's tone mapping, what
// macOS makes of the picture as the console sends it.
constexpr int kHdrBrightnessMin = 20;
constexpr int kHdrBrightnessMax = 200;
constexpr int kHdrBrightnessDefault = 100;
constexpr int kHdrBrightnessStep = 5;
/** The next value a step up (+1) or down (-1) from percent, within the range. */
int stepHdrBrightness(int percent, int direction);

// How much of the network's change is shown, in percent, and the step of the menu's stronger
// and weaker. Above 100 the change is exaggerated.
constexpr int kNrStrengthMin = 10;
constexpr int kNrStrengthMax = 200;
constexpr int kNrStrengthDefault = 100;
constexpr int kNrStrengthStep = 10;
int stepNrStrength(int percent, int direction);
// The heights the menu offers for the picture the network is given (0: the stream's own).
// The cost of a frame grows with its pixels: 720 is half of 1080's.
constexpr int kNrHeights[] = { 540, 720, 0 };
constexpr int kNrHeightDefault = 720;
constexpr int kNrHeightMin = 360;
constexpr int kNrHeightMax = 2160;

/** What the Video menu changes, as the run starts with it. */
struct ViewChoices
{
	video::Scaler scaler = video::Scaler::Bilinear;
	video::SdrColor sdr_color = video::SdrColor::Accurate;
	video::HdrOutput hdr_output = video::HdrOutput::PQ;
	int hdr_brightness = kHdrBrightnessDefault;
	video::HdrToneMap hdr_tone_map = video::HdrToneMap::App;
	bool fullscreen_sync = true;
	bool nr = true;
	NrNetwork nr_network = NrNetwork::Small;
	int nr_height = kNrHeightDefault;
	int nr_strength = kNrStrengthDefault;
	NrLook nr_look = NrLook::Standard;
};
ViewChoices resolveViewChoices(const Options &options);

/**
 * The pipeline's pacing for --pacing as given and the window as it is. A fullscreen window's
 * layer can go straight to the display, past the compositor, and a frame presented without
 * display sync is then switched in wherever the refresh happens to be: the picture tears.
 * So immediate becomes immediate-vsync in fullscreen, unless fullscreen sync is switched
 * off. The other modes are left as given.
 */
video::PacingMode effectivePacing(video::PacingMode given, bool window_fullscreen, bool fullscreen_sync);

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
