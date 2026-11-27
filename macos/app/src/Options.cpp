// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "Options.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace plk::app {

namespace {

bool parseUnsigned(const std::string &s, unsigned long long max, unsigned long long *out)
{
	if(s.empty() || s[0] == '-')
		return false;
	errno = 0;
	char *end = nullptr;
	unsigned long long v = strtoull(s.c_str(), &end, 10);
	if(errno || !end || *end || v > max)
		return false;
	*out = v;
	return true;
}

bool parseDouble(const std::string &s, double min, double max, double *out)
{
	if(s.empty())
		return false;
	errno = 0;
	char *end = nullptr;
	double v = strtod(s.c_str(), &end);
	if(errno || !end || *end || v < min || v > max)
		return false;
	*out = v;
	return true;
}

bool parseSize(const std::string &s, int *w, int *h)
{
	size_t x = s.find('x');
	if(x == std::string::npos)
		return false;
	unsigned long long a, b;
	if(!parseUnsigned(s.substr(0, x), 16384, &a) || !parseUnsigned(s.substr(x + 1), 16384, &b) || !a || !b)
		return false;
	*w = (int)a;
	*h = (int)b;
	return true;
}

} // namespace

const char *codecName(ChiakiCodec codec)
{
	switch(codec)
	{
		case CHIAKI_CODEC_H264: return "h264";
		case CHIAKI_CODEC_H265: return "h265";
		case CHIAKI_CODEC_H265_HDR: return "h265-hdr";
	}
	return "?";
}

const char *resolutionName(ChiakiVideoResolutionPreset r)
{
	switch(r)
	{
		case CHIAKI_VIDEO_RESOLUTION_PRESET_360p: return "360p";
		case CHIAKI_VIDEO_RESOLUTION_PRESET_540p: return "540p";
		case CHIAKI_VIDEO_RESOLUTION_PRESET_720p: return "720p";
		case CHIAKI_VIDEO_RESOLUTION_PRESET_1080p: return "1080p";
	}
	return "?";
}

bool parseResolution(const std::string &s, ChiakiVideoResolutionPreset *out)
{
	if(s == "360p" || s == "360") { *out = CHIAKI_VIDEO_RESOLUTION_PRESET_360p; return true; }
	if(s == "540p" || s == "540") { *out = CHIAKI_VIDEO_RESOLUTION_PRESET_540p; return true; }
	if(s == "720p" || s == "720") { *out = CHIAKI_VIDEO_RESOLUTION_PRESET_720p; return true; }
	if(s == "1080p" || s == "1080") { *out = CHIAKI_VIDEO_RESOLUTION_PRESET_1080p; return true; }
	return false;
}

bool parseCodec(const std::string &s, ChiakiCodec *out)
{
	if(s == "h264" || s == "avc") { *out = CHIAKI_CODEC_H264; return true; }
	if(s == "h265" || s == "hevc") { *out = CHIAKI_CODEC_H265; return true; }
	if(s == "h265-hdr" || s == "h265_hdr" || s == "hdr" || s == "hevc-hdr") { *out = CHIAKI_CODEC_H265_HDR; return true; }
	return false;
}

std::string usageText()
{
	return
		"usage: Pleikkari stream <nickname> <host> [options]\n"
		"       Pleikkari replay <dump.plkv> [options]\n"
		"       Pleikkari list [--profile NAME]\n"
		"       Pleikkari awdl-helper status|register|unregister\n"
		"                         the helper that keeps AWDL down during a Wi-Fi stream: what macOS\n"
		"                         says about it, hand it to macOS, or take it away again\n"
		"\n"
		"stream: connect to a console registered in the Qt app (chiaki-ng) at <host>, a LAN\n"
		"address or host name. Defaults come from the Qt app's settings for a local PS5,\n"
		"otherwise 1080p, 60 fps, H.265 and the resolution preset's bitrate.\n"
		"\n"
		"session\n"
		"  --resolution 360p|540p|720p|1080p\n"
		"  --fps 30|60\n"
		"  --bitrate KBPS          0 = the resolution preset's (1080p: 15000)\n"
		"  --codec h264|h265|h265-hdr\n"
		"  --feedback-interval MS  minimum spacing of stick/motion packets (lib default 8)\n"
		"  --no-reorder            do not wait for a missing video packet (disable reordering)\n"
		"  --keep-awdl             leave AWDL (AirDrop, AirPlay) up during a Wi-Fi stream; by default\n"
		"                          it is switched off while the stream runs, by the app's helper (allowed once\n"
		"                          in System Settings, Login Items)\n"
		"  --idr-on-fec-failure    the lib requests an IDR itself when FEC fails\n"
		"  --diagnostics           the lib's per-second stream statistics in the log\n"
		"  --verbose               the lib's verbose log level\n"
		"  --profile NAME          Qt settings profile to read the registration from\n"
		"window and video\n"
		"  --fullscreen            start in native fullscreen (Cmd-F toggles)\n"
		"  --pacing immediate|immediate-vsync|displaylink   (default immediate)\n"
		"  --fullscreen-sync on|off   --pacing immediate in a fullscreen window: each frame at a refresh\n"
		"                          of the display, so the picture cannot tear (default on)\n"
		"  --scaler bilinear|bicubic|lanczos|metalfx         (default bilinear)\n"
		"  --color accurate|vivid  SDR picture: the console's colours, or stretched to the\n"
		"                          display's full gamut (default accurate)\n"
		"  --hdr on|off            HDR stream (h265-hdr) or not, whatever the codec setting says\n"
		"  --hdr-output pq|linear  (default pq)\n"
		"  --hdr-brightness PCT    brightness of an HDR picture, 20 to 200 (default 100: its reference white\n"
		"                          as bright as the display's SDR white)\n"
		"  --hdr-tone-map app|system  who fits an HDR picture to the display: the app's shader (default;\n"
		"                          a fullscreen picture goes straight to the display) or macOS\n"
		"                          These four are also in the Video menu while the stream runs\n"
		"                          (right click or M; keys 1-4, A, V, H, P, L). A choice made\n"
		"                          there is remembered for the next run that does not give the\n"
		"                          option.\n"
		"  --nr on|off             neural rendering of the picture (default on in a window, when the\n"
		"                          engine and a model are installed: see macos/nr/README.md)\n"
		"  --nr-network small|ht   which network of the model (default ht; small is about twice as fast and changes less)\n"
		"  --nr-height N           height of the picture the network is given, 360 to 2160, or 0 for\n"
		"                          the stream's own (default 448; the cost grows with the pixels)\n"
		"  --nr-strength PCT       how much of the network's change is shown, 10 to 200 (default 100)\n"
		"  --nr-look standard|natural|cinematic|neutral   the look the network is asked for\n"
		"  --nr-timing wait|guided|beside   when a result reaches the screen. wait: a frame is held for the\n"
		"                          result of itself (no ghost; later by what a result takes; default\n"
		"                          in a window). beside: every frame at once, with the result of an\n"
		"                          earlier one (no delay; what moves ghosts). guided: at once too,\n"
		"                          with the result's tone following what moves and only its fine\n"
		"                          detail left where the picture is still\n"
		"  --camera-warp on|off    the picture is shown moved by what the right stick has asked for\n"
		"                          and the console has not shown yet, so a turn of the camera is on\n"
		"                          screen at once; how the game answers the stick is learned while\n"
		"                          playing and kept in ~/Library/Application Support/Pleikkari/\n"
		"                          camera-warp-<console>.txt (default off; K in the Video menu)\n"
		"  --stick-sweep PREFIX    how the game answers the right stick: the app moves it through\n"
		"                          set profiles (steps of 25 to 100 %, ramps, taps, reversals, a\n"
		"                          sine, up and down), writes the stick to PREFIX-stick.csv and how\n"
		"                          far each picture moved, with what the camera warp would have\n"
		"                          shown, to PREFIX-motion.csv, and ends\n"
		"  --record-input PREFIX   record a sitting: everything the controller sends to the console\n"
		"                          to PREFIX-input.csv, how far each picture moved to\n"
		"                          PREFIX-motion.csv and the video to PREFIX-video.plkv\n"
		"  --play-input FILE       play a recorded PREFIX-input.csv back to the console as the\n"
		"                          controller, with its own timing, then end. With --record-input\n"
		"                          what the game made of it is recorded in turn\n"
		"  --control SOCKET        let a program on this Mac play: it connects to the Unix socket\n"
		"                          SOCKET, is sent every new picture of the game small and grey (a\n"
		"                          header of 'PLKF', width, height as two bytes each and the arrival\n"
		"                          time in microseconds as a double, then the bytes), and sends lines\n"
		"                          of 'buttons l2 r2 left_x left_y right_x right_y' that are the\n"
		"                          controller from then on; 'release' gives it back\n"
		"  --probe-hold-l2         --stick-sweep and --lag-probe hold L2 down meanwhile (aiming)\n"
		"  --lag-probe N           measure the input lag: the app itself pushes the right stick, to\n"
		"                          the right and to the left in turn, N times, and times how long\n"
		"                          the picture takes to start moving; the results go to the log,\n"
		"                          and the app ends. Stand the player where the camera can turn\n"
		"  --frame-generation off|2x|4x   a game that renders 30 pictures a second into a 60 fps stream\n"
		"                          sends each twice. on: in place of the repeat, the picture between\n"
		"                          two of the game's is generated (NVIDIA's network) and shown, so\n"
		"                          motion is 60 pictures a second; every real picture is on screen\n"
		"                          about 25 ms later than without. 4x: three pictures between two,\n"
		"                          120 a second (default off; J in the Video menu)\n"
		"                          These six are in the Video menu too (N, 5-6, 7-9, Z and X, E, W)\n"
		"                          and remembered like the ones above. C splits the picture for\n"
		"                          comparison, I shows what each stage of a frame costs.\n"
		"  --fps-overlay on|off    the frame rate and the last frame times over the picture's corner\n"
		"                          (default on in a window; F)\n"
		"  --panel-picture beside|under   while the pipeline panel is open (G): the picture fitted\n"
		"                          beside it (default), or left at its size with the panel over it\n"
		"  --nr-library PATH       libplknr.dylib (default: the one in the app bundle)\n"
		"  --nr-models DIR         where the .dlssmodel packages are (default:\n"
		"                          ~/Library/Application Support/Pleikkari/models)\n"
		"  --nr-model PATH         one .dlssmodel package, whatever --nr-network says\n"
		"  --no-saved-choices      neither use nor remember the menu's choices\n"
		"  --headless              no window: render offscreen, controller read-only\n"
		"  --offscreen WxH         headless render size (default 3456x2234)\n"
		"  --capture DIR           headless test tool: write rendered frames to DIR at the end, as\n"
		"                          NNNN.raw and frames.csv (macos/app/tools/capture-to-png.py makes\n"
		"                          pictures of them). They are held in memory: 8 MB for 1920x1080\n"
		"  --capture-skip N        rendered frames to let pass first (default 0)\n"
		"  --capture-frames N      how many to keep, 1 to 2000 (default 120)\n"
		"  --capture-every N       keep one frame in N (default 1)\n"
		"controller\n"
		"  --no-controller         do not open the DualSense\n"
		"  --no-controller-output  read the DualSense, never write to it\n"
		"  --no-realtime-controller  driver threads at QoS only, not real-time\n"
		"  --bt-haptics on|off     haptics over Bluetooth (Qt setting, default on)\n"
		"  --usb-haptics on|off    haptics through a USB DualSense's audio device (default on;\n"
		"                          the device must be set to 4 channels in Audio MIDI Setup)\n"
		"  --haptic-gain G         haptics gain (Qt setting haptic_override, default 1.0)\n"
		"audio\n"
		"  --no-audio\n"
		"logging\n"
		"  --log PATH              log file (default: stderr)\n"
		"  --no-frame-log          no per-frame [frame] lines\n"
		"  --stats-interval S      pipeline statistics every S seconds (default 10, 0 = off)\n"
		"  --dump PATH             record the video samples as a PLKVDMP1 dump\n"
		"  --seconds S             end after S seconds\n"
		"  --retry-window S        restart a session that fails before it connects, for S seconds\n"
		"                          (default 20, as the Qt app; 0 = no retry)\n"
		"  --loop                  replay: start over at the end of the dump\n"
		"  --keys LIST             test tool: the app presses keys itself, \"2:3,4.5:v,6:m\" is\n"
		"                          key 3 at 2 s, V at 4.5 s, M at 6 s. With --headless the Video\n"
		"                          menu's keys make their choices directly (not H and M)\n";
}

bool parseOptions(int argc, const char *const *argv, Options &o, std::string *error)
{
	o = Options();
	auto fail = [&](const std::string &m) {
		if(error)
			*error = m;
		return false;
	};
	std::vector<std::string> args;
	for(int i = 1; i < argc; i++)
	{
		std::string a = argv[i] ? argv[i] : "";
		// --name=value is the same as --name value
		size_t eq = a.find('=');
		if(a.compare(0, 2, "--") == 0 && eq != std::string::npos)
		{
			args.push_back(a.substr(0, eq));
			args.push_back(a.substr(eq + 1));
		}
		else
			args.push_back(a);
	}
	// macOS passes -psn_0_NNNN to an app started from the Finder.
	std::vector<std::string> positional;
	for(size_t i = 0; i < args.size(); i++)
	{
		const std::string &a = args[i];
		auto value = [&](std::string *v) {
			if(i + 1 >= args.size())
				return false;
			*v = args[++i];
			return true;
		};
		std::string v;
		if(a == "-h" || a == "--help" || a == "help")
		{
			o.command = Command::Help;
			return true;
		}
		else if(a.compare(0, 5, "-psn_") == 0)
			continue;
		else if(a == "--resolution")
		{
			ChiakiVideoResolutionPreset r;
			if(!value(&v) || !parseResolution(v, &r))
				return fail("--resolution wants 360p, 540p, 720p or 1080p");
			o.resolution = r;
		}
		else if(a == "--fps")
		{
			if(!value(&v) || (v != "30" && v != "60"))
				return fail("--fps wants 30 or 60");
			o.fps = v == "30" ? CHIAKI_VIDEO_FPS_PRESET_30 : CHIAKI_VIDEO_FPS_PRESET_60;
		}
		else if(a == "--bitrate")
		{
			unsigned long long n;
			if(!value(&v) || !parseUnsigned(v, 1000000, &n))
				return fail("--bitrate wants kbit/s (0 = preset)");
			o.bitrate_kbps = (unsigned)n;
		}
		else if(a == "--codec")
		{
			ChiakiCodec c;
			if(!value(&v) || !parseCodec(v, &c))
				return fail("--codec wants h264, h265 or h265-hdr");
			o.codec = c;
		}
		else if(a == "--feedback-interval")
		{
			unsigned long long n;
			if(!value(&v) || !parseUnsigned(v, 1000, &n))
				return fail("--feedback-interval wants milliseconds (0..1000)");
			o.feedback_interval_ms = (uint32_t)n;
		}
		else if(a == "--no-reorder")
			o.disable_reordering = true;
		else if(a == "--keep-awdl")
			o.keep_awdl = true;
		else if(a == "--idr-on-fec-failure")
			o.idr_on_fec_failure = true;
		else if(a == "--diagnostics")
			o.diagnostics = true;
		else if(a == "--verbose")
			o.verbose = true;
		else if(a == "--profile")
		{
			if(!value(&o.profile) || o.profile.empty() || o.profile.find('/') != std::string::npos)
				return fail("--profile wants a profile name");
		}
		else if(a == "--fullscreen")
			o.fullscreen = true;
		else if(a == "--pacing")
		{
			if(!value(&v) || !video::parsePacingMode(v, &o.pacing))
				return fail("--pacing wants immediate, immediate-vsync or displaylink");
		}
		else if(a == "--scaler")
		{
			video::Scaler scaler;
			if(!value(&v) || !video::parseScaler(v, &scaler))
				return fail("--scaler wants bilinear, bicubic, lanczos or metalfx");
			o.scaler = scaler;
		}
		else if(a == "--color" || a == "--colour")
		{
			video::SdrColor color;
			if(!value(&v) || !video::parseSdrColor(v, &color))
				return fail("--color wants accurate or vivid");
			o.sdr_color = color;
		}
		else if(a == "--hdr")
		{
			if(!value(&v) || (v != "on" && v != "off"))
				return fail("--hdr wants on or off");
			o.hdr = v == "on";
		}
		else if(a == "--hdr-output")
		{
			video::HdrOutput output;
			if(!value(&v) || !video::parseHdrOutput(v, &output))
				return fail("--hdr-output wants pq or linear");
			o.hdr_output = output;
		}
		else if(a == "--hdr-brightness")
		{
			double pct = 0;
			if(!value(&v) || !parseDouble(v, kHdrBrightnessMin, kHdrBrightnessMax, &pct))
				return fail("--hdr-brightness wants a percentage from 20 to 200");
			o.hdr_brightness = (int)(pct + 0.5);
		}
		else if(a == "--fps-overlay")
		{
			if(!value(&v) || (v != "on" && v != "off"))
				return fail("--fps-overlay wants on or off");
			o.fps_overlay = v == "on";
		}
		else if(a == "--panel-picture")
		{
			if(!value(&v) || (v != "beside" && v != "under"))
				return fail("--panel-picture wants beside or under");
			o.panel_beside = v == "beside";
		}
		else if(a == "--fullscreen-sync")
		{
			if(!value(&v) || (v != "on" && v != "off"))
				return fail("--fullscreen-sync wants on or off");
			o.fullscreen_sync = v == "on";
		}
		else if(a == "--hdr-tone-map")
		{
			video::HdrToneMap tone_map;
			if(!value(&v) || !video::parseHdrToneMap(v, &tone_map))
				return fail("--hdr-tone-map wants app or system");
			o.hdr_tone_map = tone_map;
		}
		else if(a == "--nr")
		{
			if(!value(&v) || (v != "on" && v != "off"))
				return fail("--nr wants on or off");
			o.nr = v == "on";
		}
		else if(a == "--nr-network")
		{
			NrNetwork network;
			if(!value(&v) || !parseNrNetwork(v, &network))
				return fail("--nr-network wants small or ht");
			o.nr_network = network;
		}
		else if(a == "--nr-height")
		{
			unsigned long long n;
			if(!value(&v) || !parseUnsigned(v, kNrHeightMax, &n) || (n != 0 && n < kNrHeightMin))
				return fail("--nr-height wants 0 or a height from 360 to 2160");
			o.nr_height = (int)n;
		}
		else if(a == "--nr-strength")
		{
			double pct = 0;
			if(!value(&v) || !parseDouble(v, kNrStrengthMin, kNrStrengthMax, &pct))
				return fail("--nr-strength wants a percentage from 10 to 200");
			o.nr_strength = (int)(pct + 0.5);
		}
		else if(a == "--nr-look")
		{
			NrLook look;
			if(!value(&v) || !parseNrLook(v, &look))
				return fail("--nr-look wants standard, natural, cinematic or neutral");
			o.nr_look = look;
		}
		else if(a == "--frame-generation")
		{
			if(!value(&v) || (v != "on" && v != "off" && v != "2x" && v != "4x"))
				return fail("--frame-generation wants off, on, 2x or 4x");
			o.frame_generation = v != "off";
			if(v == "2x" || v == "4x")
				o.frame_generation_factor = v == "4x" ? 4 : 2;
		}
		else if(a == "--nr-timing")
		{
			NrTiming timing;
			if(!value(&v) || !parseNrTiming(v, &timing))
				return fail("--nr-timing wants wait, guided or beside");
			o.nr_timing = timing;
		}
		else if(a == "--nr-library")
		{
			if(!value(&o.nr_library) || o.nr_library.empty())
				return fail("--nr-library wants the path of libplknr.dylib");
		}
		else if(a == "--nr-models")
		{
			if(!value(&o.nr_models) || o.nr_models.empty())
				return fail("--nr-models wants a directory");
		}
		else if(a == "--nr-model")
		{
			if(!value(&o.nr_model) || o.nr_model.empty())
				return fail("--nr-model wants the path of a .dlssmodel package");
		}
		else if(a == "--no-saved-choices")
			o.saved_choices = false;
		else if(a == "--keys")
		{
			std::vector<KeyPress> presses;
			std::string why;
			if(!value(&v) || !parseKeyScript(v, presses, &why))
				return fail("--keys wants a list like 2:3,4.5:v" + (why.empty() ? "" : " (" + why + ")"));
			o.keys = v;
		}
		else if(a == "--headless")
			o.headless = true;
		else if(a == "--offscreen")
		{
			if(!value(&v) || !parseSize(v, &o.offscreen_width, &o.offscreen_height))
				return fail("--offscreen wants WxH");
		}
		else if(a == "--capture")
		{
			if(!value(&v) || v.empty())
				return fail("--capture wants a directory");
			o.capture_dir = v;
		}
		else if(a == "--capture-skip" || a == "--capture-frames" || a == "--capture-every")
		{
			unsigned long long n;
			const bool count = a != "--capture-skip";
			if(!value(&v) || !parseUnsigned(v, a == "--capture-frames" ? 2000 : 1000000, &n) || (count && n == 0))
				return fail(a + " wants a number" + (a == "--capture-frames" ? " from 1 to 2000" : count ? " from 1" : ""));
			if(a == "--capture-skip")
				o.capture_skip = n;
			else if(a == "--capture-frames")
				o.capture_frames = (unsigned)n;
			else
				o.capture_every = (unsigned)n;
		}
		else if(a == "--no-controller")
			o.controller = false;
		else if(a == "--no-controller-output")
			o.controller_output = false;
		else if(a == "--no-realtime-controller")
			o.realtime_controller = false;
		else if(a == "--bt-haptics")
		{
			if(!value(&v) || (v != "on" && v != "off"))
				return fail("--bt-haptics wants on or off");
			o.bt_haptics = v == "on";
		}
		else if(a == "--usb-haptics")
		{
			if(!value(&v) || (v != "on" && v != "off"))
				return fail("--usb-haptics wants on or off");
			o.usb_haptics = v == "on";
		}
		else if(a == "--haptic-gain")
		{
			double g;
			if(!value(&v) || !parseDouble(v, 0.0, 10.0, &g))
				return fail("--haptic-gain wants a number 0..10");
			o.haptic_gain = (float)g;
		}
		else if(a == "--no-audio")
			o.audio = false;
		else if(a == "--log")
		{
			if(!value(&o.log_path) || o.log_path.empty())
				return fail("--log wants a path");
		}
		else if(a == "--no-frame-log")
			o.frame_log = false;
		else if(a == "--stats-interval")
		{
			if(!value(&v) || !parseDouble(v, 0.0, 3600.0, &o.stats_interval_s))
				return fail("--stats-interval wants seconds (0 = off)");
		}
		else if(a == "--dump")
		{
			if(!value(&o.dump_path) || o.dump_path.empty())
				return fail("--dump wants a path");
		}
		else if(a == "--stick-sweep")
		{
			if(!value(&v) || v.empty())
				return fail("--stick-sweep wants a file name prefix");
			o.stick_sweep = v;
		}
		else if(a == "--record-input")
		{
			if(!value(&o.record_input) || o.record_input.empty())
				return fail("--record-input wants a file name prefix");
			if(o.dump_path.empty())
				o.dump_path = o.record_input + "-video.plkv";
		}
		else if(a == "--play-input")
		{
			if(!value(&o.play_input) || o.play_input.empty())
				return fail("--play-input wants a recorded input file");
		}
		else if(a == "--control")
		{
			if(!value(&o.control_socket) || o.control_socket.empty())
				return fail("--control wants a socket path");
		}
		else if(a == "--probe-hold-l2")
			o.probe_hold_l2 = true;
		else if(a == "--lag-probe")
		{
			double n = 0;
			if(!value(&v) || !parseDouble(v, 1.0, 1000.0, &n))
				return fail("--lag-probe wants a number of measurements from 1 to 1000");
			o.lag_probe = (int)n;
		}
		else if(a == "--camera-warp")
		{
			if(!value(&v) || (v != "on" && v != "off"))
				return fail("--camera-warp wants on or off");
			o.camera_warp = v == "on";
		}
		else if(a == "--seconds")
		{
			if(!value(&v) || !parseDouble(v, 0.0, 1e7, &o.seconds))
				return fail("--seconds wants a number");
		}
		else if(a == "--retry-window")
		{
			if(!value(&v) || !parseDouble(v, 0.0, 600.0, &o.retry_window_s))
				return fail("--retry-window wants seconds (0 = no retry)");
		}
		else if(a == "--loop")
			o.loop = true;
		else if(a.size() > 1 && a[0] == '-')
			return fail("unknown option " + a);
		else
			positional.push_back(a);
	}

	if(positional.empty())
	{
		// No command: the app was opened, not run. main() streams from the registered console.
		o.no_command = true;
		o.command = Command::Help;
		return true;
	}
	const std::string &cmd = positional[0];
	if(cmd == "stream")
	{
		if(positional.size() != 3)
			return fail("stream wants <nickname> <host>");
		o.command = Command::Stream;
		o.nickname = positional[1];
		o.host = positional[2];
		if(o.nickname.empty() || o.host.empty())
			return fail("stream wants <nickname> <host>");
	}
	else if(cmd == "replay")
	{
		if(positional.size() != 2)
			return fail("replay wants <dump.plkv>");
		o.command = Command::Replay;
		o.replay_path = positional[1];
	}
	else if(cmd == "list")
	{
		if(positional.size() != 1)
			return fail("list takes no arguments");
		o.command = Command::List;
	}
	else if(cmd == "awdl-helper")
	{
		if(positional.size() != 2 || (positional[1] != "status" && positional[1] != "register" && positional[1] != "unregister"))
			return fail("awdl-helper wants status, register or unregister");
		o.command = Command::AwdlHelper;
		o.awdl_helper_action = positional[1];
	}
	else
		return fail("unknown command " + cmd);
	if(o.headless)
		o.controller_output = false;
	else if(!o.capture_dir.empty())
		return fail("--capture needs --headless");
	return true;
}

bool parseKeyScript(const std::string &list, std::vector<KeyPress> &out, std::string *error)
{
	out.clear();
	size_t pos = 0;
	while(pos <= list.size())
	{
		size_t end = list.find(',', pos);
		if(end == std::string::npos)
			end = list.size();
		const std::string entry = list.substr(pos, end - pos);
		pos = end + 1;
		if(entry.empty())
			continue;
		const size_t colon = entry.find(':');
		KeyPress press;
		char *rest = nullptr;
		if(colon == std::string::npos || colon == 0 || colon + 1 >= entry.size())
		{
			if(error)
				*error = "\"" + entry + "\" is not seconds:key";
			return false;
		}
		const std::string seconds = entry.substr(0, colon);
		press.at_s = strtod(seconds.c_str(), &rest);
		if(!rest || *rest || press.at_s < 0)
		{
			if(error)
				*error = "\"" + seconds + "\" is not a time in seconds";
			return false;
		}
		press.key = entry.substr(colon + 1);
		static const char *const names[] = { "up", "down", "left", "right", "enter", "esc", "space", "pointer" };
		bool known = press.key.size() == 1;
		for(const char *name : names)
			known = known || press.key == name;
		if(!known)
		{
			if(error)
				*error = "\"" + press.key + "\" is not a key";
			return false;
		}
		out.push_back(press);
	}
	return true;
}

ViewChoices resolveViewChoices(const Options &o)
{
	ViewChoices v;
	if(o.scaler)
		v.scaler = *o.scaler;
	if(o.sdr_color)
		v.sdr_color = *o.sdr_color;
	if(o.hdr_output)
		v.hdr_output = *o.hdr_output;
	if(o.hdr_brightness)
		v.hdr_brightness = *o.hdr_brightness;
	if(o.hdr_tone_map)
		v.hdr_tone_map = *o.hdr_tone_map;
	if(o.fullscreen_sync)
		v.fullscreen_sync = *o.fullscreen_sync;
	// A headless run is a measurement: it gets neural rendering only when it asks for it.
	v.nr = o.nr ? *o.nr : !o.headless;
	if(o.nr_network)
		v.nr_network = *o.nr_network;
	if(o.nr_height)
		v.nr_height = *o.nr_height;
	if(o.nr_strength)
		v.nr_strength = *o.nr_strength;
	if(o.nr_look)
		v.nr_look = *o.nr_look;
	// A headless run is a measurement of the stream's own path unless it asks for this.
	if(o.camera_warp)
		v.camera_warp = *o.camera_warp;
	if(o.frame_generation)
		v.frame_generation = *o.frame_generation;
	if(o.frame_generation_factor)
		v.frame_generation_factor = *o.frame_generation_factor;
	v.nr_timing = o.nr_timing ? *o.nr_timing : o.headless ? NrTiming::Beside : NrTiming::Wait;
	// And nothing over the picture it renders.
	v.fps_overlay = o.fps_overlay ? *o.fps_overlay : !o.headless;
	if(o.panel_beside)
		v.panel_beside = *o.panel_beside;
	return v;
}

video::PacingMode effectivePacing(video::PacingMode given, bool window_fullscreen, bool fullscreen_sync)
{
	if(given == video::PacingMode::Immediate && window_fullscreen && fullscreen_sync)
		return video::PacingMode::ImmediateVsync;
	return given;
}

const char *toString(NrTiming timing)
{
	switch(timing)
	{
		case NrTiming::Wait: return "wait";
		case NrTiming::Guided: return "guided";
		case NrTiming::Beside: break;
	}
	return "beside";
}

bool parseNrTiming(const std::string &s, NrTiming *out)
{
	for(NrTiming timing : { NrTiming::Beside, NrTiming::Wait, NrTiming::Guided })
		if(s == toString(timing))
		{
			*out = timing;
			return true;
		}
	return false;
}

const char *toString(NrLook look)
{
	switch(look)
	{
		case NrLook::Natural: return "natural";
		case NrLook::Cinematic: return "cinematic";
		case NrLook::Neutral: return "neutral";
		case NrLook::Standard: break;
	}
	return "standard";
}

bool parseNrLook(const std::string &s, NrLook *out)
{
	for(NrLook look : { NrLook::Standard, NrLook::Natural, NrLook::Cinematic, NrLook::Neutral })
		if(s == toString(look))
		{
			*out = look;
			return true;
		}
	return false;
}

NrLook nextNrLook(NrLook look)
{
	return look == NrLook::Neutral ? NrLook::Standard : (NrLook)((int)look + 1);
}

const char *toString(NrNetwork network)
{
	return network == NrNetwork::HT ? "ht" : "small";
}

bool parseNrNetwork(const std::string &s, NrNetwork *out)
{
	if(s == "small") { *out = NrNetwork::Small; return true; }
	if(s == "ht" || s == "HT") { *out = NrNetwork::HT; return true; }
	return false;
}

int stepNrStrength(int percent, int direction)
{
	int next;
	if(direction > 0)
		next = (percent / kNrStrengthStep + 1) * kNrStrengthStep;
	else if(direction < 0)
		next = ((percent + kNrStrengthStep - 1) / kNrStrengthStep - 1) * kNrStrengthStep;
	else
		next = percent;
	return next < kNrStrengthMin ? kNrStrengthMin : next > kNrStrengthMax ? kNrStrengthMax : next;
}

int stepHdrBrightness(int percent, int direction)
{
	// Onto the step grid first, so a value given by hand lands on a round number.
	int next;
	if(direction > 0)
		next = (percent / kHdrBrightnessStep + 1) * kHdrBrightnessStep;
	else if(direction < 0)
		next = ((percent + kHdrBrightnessStep - 1) / kHdrBrightnessStep - 1) * kHdrBrightnessStep;
	else
		next = percent;
	return next < kHdrBrightnessMin ? kHdrBrightnessMin : next > kHdrBrightnessMax ? kHdrBrightnessMax : next;
}

ChiakiCodec codecWithHdr(ChiakiCodec codec, bool hdr, ChiakiCodec sdr_codec)
{
	if(hdr)
		return CHIAKI_CODEC_H265_HDR;
	if(codec != CHIAKI_CODEC_H265_HDR)
		return codec;
	return sdr_codec == CHIAKI_CODEC_H265_HDR ? CHIAKI_CODEC_H265 : sdr_codec;
}

StreamConfig resolveStreamConfig(const Options &o, const QtStreamSettings *qt)
{
	StreamConfig c;
	StreamSettings &s = c.session;
	std::string src;
	auto note = [&](const char *what, const char *from) {
		if(!src.empty())
			src += ", ";
		src += what;
		src += " ";
		src += from;
	};

	ChiakiVideoResolutionPreset res = CHIAKI_VIDEO_RESOLUTION_PRESET_1080p;
	const char *res_from = "default";
	if(o.resolution)
		res = *o.resolution, res_from = "command line";
	else if(qt && qt->has_resolution && parseResolution(qt->resolution, &res))
		res_from = "Qt settings";
	note("resolution", res_from);

	ChiakiVideoFPSPreset fps = CHIAKI_VIDEO_FPS_PRESET_60;
	const char *fps_from = "default";
	if(o.fps)
		fps = *o.fps, fps_from = "command line";
	else if(qt && qt->has_fps)
		fps = qt->fps == 30 ? CHIAKI_VIDEO_FPS_PRESET_30 : CHIAKI_VIDEO_FPS_PRESET_60, fps_from = "Qt settings";
	note("fps", fps_from);

	unsigned bitrate = 0;
	const char *bitrate_from = "preset";
	if(o.bitrate_kbps)
		bitrate = *o.bitrate_kbps, bitrate_from = *o.bitrate_kbps ? "command line" : "preset";
	else if(qt && qt->has_bitrate && qt->bitrate_kbps)
		bitrate = qt->bitrate_kbps, bitrate_from = "Qt settings";
	note("bitrate", bitrate_from);

	ChiakiCodec codec = CHIAKI_CODEC_H265;
	const char *codec_from = "default";
	if(o.codec)
		codec = *o.codec, codec_from = "command line";
	else if(qt && qt->has_codec && parseCodec(qt->codec, &codec))
		codec_from = "Qt settings";
	// HDR on or off on top of that: the menu's choice or --hdr.
	if(o.hdr && *o.hdr != (codec == CHIAKI_CODEC_H265_HDR))
		codec = codecWithHdr(codec, *o.hdr), codec_from = "HDR choice";
	note("codec", codec_from);

	s.resolution = res;
	s.fps = fps;
	s.bitrate_kbps = bitrate;
	s.codec = codec;
	s.enable_dualsense = true;
	s.video_profile_auto_downgrade = true;
	s.idr_on_fec_failure = o.idr_on_fec_failure;
	s.disable_video_packet_reordering = o.disable_reordering;
	s.packet_loss_max = (qt && qt->has_packet_loss_max) ? qt->packet_loss_max : 0.05;
	s.feedback_state_min_interval_ms = o.feedback_interval_ms ? *o.feedback_interval_ms : 0;
	s.stream_diagnostics = o.diagnostics;
	s.audio_video_disabled = CHIAKI_NONE_DISABLED; // --no-audio only leaves the PCM sink out
	s.elevate_thread_qos = true;
	s.suspend_awdl = !o.keep_awdl;
	s.log_mask = CHIAKI_LOG_INFO | CHIAKI_LOG_WARNING | CHIAKI_LOG_ERROR;
	if(o.verbose)
		s.log_mask |= CHIAKI_LOG_VERBOSE;

	ChiakiConnectVideoProfile profile;
	memset(&profile, 0, sizeof(profile));
	chiaki_connect_video_profile_preset(&profile, res, fps);
	c.effective_bitrate_kbps = bitrate ? bitrate : profile.bitrate;
	c.width = (int)profile.width;
	c.height = (int)profile.height;
	c.fps = (int)profile.max_fps;

	c.bt_haptics = o.bt_haptics ? *o.bt_haptics : (qt && qt->has_bt_haptics ? qt->bt_haptics : true);
	c.haptic_gain = o.haptic_gain ? *o.haptic_gain : (qt && qt->has_haptic_gain ? qt->haptic_gain : 1.0f);
	c.source = src;
	return c;
}

} // namespace plk::app
