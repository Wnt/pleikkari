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

enum class Command { Help, Stream, Replay, List, AwdlHelper };

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

// When the network's result reaches the screen (video::EnhanceConfig::wait).
//   Wait    a frame is held for the result of itself: nothing ghosts, and every picture is
//           later by what a result takes
//   Beside  every frame at once, with the result of an earlier one: no delay, and what
//           moves ghosts
// A third way, a new picture at once and its repeat with the picture's own result, was
// tried on 2026-11-27 and taken out the same day: the picture alternated between the plain
// and the enhanced one thirty times a second, which the user found terrible to look at.
//   Guided  every frame at once, as Beside, but the result's tone is looked up by each
//           frame's own pixels (video::EnhanceConfig::guided), so it follows what moves; only
//           its fine detail is tied to where things were, and shows where the picture is still
enum class NrTiming { Beside, Wait, Guided };
const char *toString(NrTiming timing);
bool parseNrTiming(const std::string &s, NrTiming *out);

struct Options
{
	Command command = Command::Help;
	bool no_command = false;         // started with no command at all, as from the Finder
	std::string nickname;            // stream
	std::string host;                // stream
	std::string awdl_helper_action;  // awdl-helper: status, register, unregister
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
	std::optional<bool> camera_warp;        // the picture moved by what the right stick asked for; default off
	std::optional<bool> frame_generation;   // the picture between two of the game's; default off
	std::optional<int> frame_generation_factor; // 2 or 4 times the game's pictures; default 2
	std::optional<NrTiming> nr_timing;      // default wait in a window
	// The frame rate over the picture's corner, and where the picture is while the pipeline
	// panel is open (PipelinePanel.h): both in the Video menu and remembered too.
	std::optional<bool> fps_overlay;        // default on in a window
	std::optional<bool> panel_beside;       // default true: the picture is fitted beside the panel
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
	std::string stick_sweep;         // move the right stick through set profiles and record what the picture did, to PREFIX-*.csv
	std::string record_input;        // record what the controller sends, how the picture moved and the video, to PREFIX-*
	std::string play_input;          // play a recorded PREFIX-input.csv to the console as the controller, then end
	std::string control_socket;      // serve pictures to, and take the controller from, a program on this socket
	bool probe_hold_l2 = false;      // --lag-probe and --stick-sweep hold L2 down meanwhile (aiming)
	int lag_probe = 0;               // measure the input lag N times with the right stick, then end
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
// The cost of a frame grows with its pixels. Multiples of 64, the network's own grid: 576
// (1024x576, which needs no padding at all) is faster than 540, and 704 is the largest that
// does every frame of a 60 fps stream on an M5 Max (13.4 ms a result, measured 2026-11-27).
// Not 720: at exactly 1280x720 the network changes the picture about twice as much, and
// darker, as at 704 or 768 or any other size tried (mean change of 5.0 eight-bit levels
// against 2.0 to 2.7), for a reason not found.
// The HT network, the one whose change is worth having (the user's judgement, 2026-11-27),
// does every frame of a 60 fps stream at 448 on an M5 Max (12.5 ms a result; 15.5 at 512,
// 17.5 at 576, 25 at 704), so that is the default.
constexpr int kNrHeights[] = { 448, 576, 704, 0 };
constexpr int kNrHeightDefault = 448;
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
	NrNetwork nr_network = NrNetwork::HT;
	int nr_height = kNrHeightDefault;
	int nr_strength = kNrStrengthDefault;
	NrLook nr_look = NrLook::Standard;
	NrTiming nr_timing = NrTiming::Wait;
	bool frame_generation = false;
	bool camera_warp = false;
	int frame_generation_factor = 2;  // 2: one picture between two of the game's; 4: three
	bool fps_overlay = true;
	bool panel_beside = true;
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
