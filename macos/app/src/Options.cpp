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
		"                          it is switched off while the stream runs (asks for the password once)\n"
		"  --idr-on-fec-failure    the lib requests an IDR itself when FEC fails\n"
		"  --diagnostics           the lib's per-second stream statistics in the log\n"
		"  --verbose               the lib's verbose log level\n"
		"  --profile NAME          Qt settings profile to read the registration from\n"
		"window and video\n"
		"  --fullscreen            start in native fullscreen (Cmd-F toggles)\n"
		"  --pacing immediate|immediate-vsync|displaylink   (default immediate)\n"
		"  --scaler bilinear|bicubic|lanczos|metalfx         (default bilinear)\n"
		"  --color accurate|vivid  SDR picture: the console's colours, or stretched to the\n"
		"                          display's full gamut (default accurate)\n"
		"  --hdr on|off            HDR stream (h265-hdr) or not, whatever the codec setting says\n"
		"  --hdr-output pq|linear  (default pq)\n"
		"                          These four are also in the Video menu while the stream runs\n"
		"                          (right click or M; keys 1-4, A, V, H, P, L). A choice made\n"
		"                          there is remembered for the next run that does not give the\n"
		"                          option.\n"
		"  --no-saved-choices      neither use nor remember the menu's choices\n"
		"  --headless              no window: render offscreen, controller read-only\n"
		"  --offscreen WxH         headless render size (default 3456x2234)\n"
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
		"                          key 3 at 2 s, V at 4.5 s, M at 6 s\n";
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
	else
		return fail("unknown command " + cmd);
	if(o.headless)
		o.controller_output = false;
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
		static const char *const names[] = { "up", "down", "left", "right", "enter", "esc", "space" };
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
	return v;
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
