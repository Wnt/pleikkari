// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "Options.h"
#include "TestHarness.h"

#include <vector>

using namespace plk;
using namespace plk::app;

namespace {

bool parse(std::vector<const char *> args, Options &o, std::string *err = nullptr)
{
	args.insert(args.begin(), "Pleikkari");
	return parseOptions((int)args.size(), args.data(), o, err);
}

} // namespace

PLK_TEST(options_stream_defaults)
{
	Options o;
	REQUIRE(parse({ "stream", "PS5-466", "192.168.1.164" }, o));
	CHECK(o.command == Command::Stream);
	CHECK(o.nickname == "PS5-466");
	CHECK(o.host == "192.168.1.164");
	CHECK(!o.fullscreen);
	CHECK(!o.headless);
	CHECK(o.controller && o.controller_output && o.realtime_controller);
	CHECK(o.pacing == video::PacingMode::Immediate);
	CHECK(o.frame_log);

	const StreamConfig c = resolveStreamConfig(o, nullptr);
	CHECK(c.session.resolution == CHIAKI_VIDEO_RESOLUTION_PRESET_1080p);
	CHECK(c.session.fps == CHIAKI_VIDEO_FPS_PRESET_60);
	CHECK(c.session.codec == CHIAKI_CODEC_H265);
	CHECK(c.session.bitrate_kbps == 0);
	CHECK(c.effective_bitrate_kbps == 15000); // the 1080p preset, as the Qt app
	CHECK(c.width == 1920 && c.height == 1080 && c.fps == 60);
	CHECK(c.session.enable_dualsense);
	CHECK(c.session.video_profile_auto_downgrade);
	CHECK(!c.session.idr_on_fec_failure);
	CHECK(!c.session.disable_video_packet_reordering);
	CHECK(c.session.suspend_awdl);
	CHECK(c.session.packet_loss_max > 0.0499 && c.session.packet_loss_max < 0.0501);
	CHECK(c.session.feedback_state_min_interval_ms == 0);
	CHECK(c.bt_haptics);
	CHECK(c.haptic_gain == 1.0f);
}

PLK_TEST(options_all_flags)
{
	Options o;
	std::string err;
	REQUIRE(parse({ "stream", "PS5-466", "192.168.1.164", "--fullscreen", "--resolution", "720p", "--fps=30",
						  "--bitrate", "12000", "--codec", "h265-hdr", "--feedback-interval", "4", "--no-reorder", "--keep-awdl",
						  "--log", "/tmp/x.log", "--pacing", "displaylink", "--scaler", "lanczos", "--stats-interval", "5",
						  "--no-controller-output", "--haptic-gain", "0.5", "--bt-haptics", "off", "--seconds", "3" },
			o, &err));
	CHECK(o.fullscreen);
	CHECK(o.log_path == "/tmp/x.log");
	CHECK(o.pacing == video::PacingMode::DisplayLink);
	CHECK(o.scaler == video::Scaler::Lanczos);
	CHECK(o.stats_interval_s == 5.0);
	CHECK(!o.controller_output);
	CHECK(o.seconds == 3.0);
	const StreamConfig c = resolveStreamConfig(o, nullptr);
	CHECK(c.session.resolution == CHIAKI_VIDEO_RESOLUTION_PRESET_720p);
	CHECK(c.session.fps == CHIAKI_VIDEO_FPS_PRESET_30);
	CHECK(c.session.bitrate_kbps == 12000 && c.effective_bitrate_kbps == 12000);
	CHECK(c.session.codec == CHIAKI_CODEC_H265_HDR);
	CHECK(c.session.feedback_state_min_interval_ms == 4);
	CHECK(c.session.disable_video_packet_reordering);
	CHECK(!c.session.suspend_awdl);
	CHECK(c.width == 1280 && c.height == 720);
	CHECK(!c.bt_haptics);
	CHECK(c.haptic_gain == 0.5f);
}

PLK_TEST(options_qt_settings_are_defaults_command_line_wins)
{
	QtStreamSettings qt;
	qt.has_resolution = true;
	qt.resolution = "720p";
	qt.has_fps = true;
	qt.fps = 30;
	qt.has_bitrate = true;
	qt.bitrate_kbps = 20000;
	qt.has_codec = true;
	qt.codec = "h264";
	qt.has_haptic_gain = true;
	qt.haptic_gain = 0.25f;
	qt.has_bt_haptics = true;
	qt.bt_haptics = false;
	qt.has_packet_loss_max = true;
	qt.packet_loss_max = 0.1;

	Options o;
	REQUIRE(parse({ "stream", "a", "b" }, o));
	StreamConfig c = resolveStreamConfig(o, &qt);
	CHECK(c.session.resolution == CHIAKI_VIDEO_RESOLUTION_PRESET_720p);
	CHECK(c.session.fps == CHIAKI_VIDEO_FPS_PRESET_30);
	CHECK(c.effective_bitrate_kbps == 20000);
	CHECK(c.session.codec == CHIAKI_CODEC_H264);
	CHECK(c.haptic_gain == 0.25f);
	CHECK(!c.bt_haptics);
	CHECK(c.session.packet_loss_max > 0.0999 && c.session.packet_loss_max < 0.1001);
	CHECK(c.source.find("resolution Qt settings") != std::string::npos);

	REQUIRE(parse({ "stream", "a", "b", "--resolution", "1080p", "--codec", "h265", "--bitrate", "0", "--fps", "60" }, o));
	c = resolveStreamConfig(o, &qt);
	CHECK(c.session.resolution == CHIAKI_VIDEO_RESOLUTION_PRESET_1080p);
	CHECK(c.session.codec == CHIAKI_CODEC_H265);
	CHECK(c.session.fps == CHIAKI_VIDEO_FPS_PRESET_60);
	CHECK(c.session.bitrate_kbps == 0 && c.effective_bitrate_kbps == 15000); // explicit 0 = preset
	CHECK(c.source.find("resolution command line") != std::string::npos);
}

PLK_TEST(options_errors_and_other_commands)
{
	Options o;
	std::string err;
	CHECK(!parse({ "stream", "only-nick" }, o, &err));
	CHECK(!err.empty());
	CHECK(!parse({ "stream", "a", "b", "--codec", "av1" }, o, &err));
	CHECK(!parse({ "stream", "a", "b", "--fps", "120" }, o, &err));
	CHECK(!parse({ "stream", "a", "b", "--resolution", "4k" }, o, &err));
	CHECK(!parse({ "stream", "a", "b", "--bogus" }, o, &err));
	CHECK(!parse({ "stream", "a", "b", "--bitrate" }, o, &err));
	CHECK(!parse({ "frobnicate" }, o, &err));
	CHECK(!parse({ "list", "extra" }, o, &err));

	REQUIRE(parse({}, o));
	CHECK(o.command == Command::Help);
	REQUIRE(parse({ "--help" }, o));
	CHECK(o.command == Command::Help);
	REQUIRE(parse({ "list" }, o));
	CHECK(o.command == Command::List);
	REQUIRE(parse({ "replay", "x.plkv", "--headless", "--loop" }, o));
	CHECK(o.command == Command::Replay && o.replay_path == "x.plkv" && o.loop);
	// Headless never writes to the controller.
	CHECK(o.headless && !o.controller_output);
	// Finder's process serial number argument is ignored.
	REQUIRE(parse({ "-psn_0_12345", "list" }, o));
	CHECK(o.command == Command::List);
}

PLK_TEST(options_codec_names)
{
	ChiakiCodec c;
	CHECK(parseCodec("hevc", &c) && c == CHIAKI_CODEC_H265);
	CHECK(parseCodec("h265_hdr", &c) && c == CHIAKI_CODEC_H265_HDR);
	CHECK(parseCodec("h264", &c) && c == CHIAKI_CODEC_H264);
	CHECK(std::string(codecName(CHIAKI_CODEC_H265_HDR)) == "h265-hdr");
	ChiakiVideoResolutionPreset r;
	CHECK(parseResolution("540p", &r) && r == CHIAKI_VIDEO_RESOLUTION_PRESET_540p);
	CHECK(std::string(resolutionName(CHIAKI_VIDEO_RESOLUTION_PRESET_360p)) == "360p");
	CHECK(!usageText().empty());
}

PLK_TEST(options_view_choices_and_hdr_switch)
{
	Options o;
	REQUIRE(parse({ "stream", "PS5-466", "192.168.1.164" }, o));
	CHECK(!o.scaler && !o.sdr_color && !o.hdr_output && !o.hdr);
	CHECK(o.saved_choices);
	ViewChoices v = resolveViewChoices(o);
	CHECK(v.scaler == video::Scaler::Bilinear);
	CHECK(v.sdr_color == video::SdrColor::Accurate);
	CHECK(v.hdr_output == video::HdrOutput::PQ);

	REQUIRE(parse({ "stream", "PS5-466", "192.168.1.164", "--scaler", "metalfx", "--color", "vivid", "--hdr-output", "linear",
						  "--hdr", "on", "--no-saved-choices" },
			o));
	CHECK(!o.saved_choices);
	v = resolveViewChoices(o);
	CHECK(v.scaler == video::Scaler::MetalFX);
	CHECK(v.sdr_color == video::SdrColor::Vivid);
	CHECK(v.hdr_output == video::HdrOutput::ExtendedLinear);
	// --hdr on makes the default H.265 an HDR stream.
	CHECK(resolveStreamConfig(o, nullptr).session.codec == CHIAKI_CODEC_H265_HDR);

	// --hdr off takes HDR out of whatever names it, and leaves an SDR codec alone.
	REQUIRE(parse({ "stream", "PS5-466", "192.168.1.164", "--hdr", "off", "--codec", "h265-hdr" }, o));
	CHECK(resolveStreamConfig(o, nullptr).session.codec == CHIAKI_CODEC_H265);
	REQUIRE(parse({ "stream", "PS5-466", "192.168.1.164", "--hdr", "off" }, o));
	QtStreamSettings qt;
	qt.has_codec = true;
	qt.codec = "h265_hdr";
	CHECK(resolveStreamConfig(o, &qt).session.codec == CHIAKI_CODEC_H265);
	qt.codec = "h264";
	CHECK(resolveStreamConfig(o, &qt).session.codec == CHIAKI_CODEC_H264);

	std::string err;
	CHECK(!parse({ "stream", "a", "b", "--color", "loud" }, o, &err));
	CHECK(!parse({ "stream", "a", "b", "--hdr", "maybe" }, o, &err));
	CHECK(!parse({ "stream", "a", "b", "--hdr-output", "hlg" }, o, &err));
}

PLK_TEST(options_codec_with_hdr)
{
	CHECK(codecWithHdr(CHIAKI_CODEC_H265, true) == CHIAKI_CODEC_H265_HDR);
	CHECK(codecWithHdr(CHIAKI_CODEC_H264, true) == CHIAKI_CODEC_H265_HDR);
	CHECK(codecWithHdr(CHIAKI_CODEC_H265_HDR, true) == CHIAKI_CODEC_H265_HDR);
	// Off: back to the SDR codec the stream had, H.265 when it never had one.
	CHECK(codecWithHdr(CHIAKI_CODEC_H265_HDR, false) == CHIAKI_CODEC_H265);
	CHECK(codecWithHdr(CHIAKI_CODEC_H265_HDR, false, CHIAKI_CODEC_H264) == CHIAKI_CODEC_H264);
	CHECK(codecWithHdr(CHIAKI_CODEC_H265_HDR, false, CHIAKI_CODEC_H265_HDR) == CHIAKI_CODEC_H265);
	CHECK(codecWithHdr(CHIAKI_CODEC_H264, false) == CHIAKI_CODEC_H264);
}

PLK_TEST(options_key_script)
{
	std::vector<KeyPress> keys;
	std::string err;
	REQUIRE(parseKeyScript("2:3,4.5:v,6:down,7:enter", keys, &err));
	REQUIRE(keys.size() == 4);
	CHECK(keys[0].at_s == 2.0 && keys[0].key == "3");
	CHECK(keys[1].at_s == 4.5 && keys[1].key == "v");
	CHECK(keys[2].key == "down");
	CHECK(keys[3].at_s == 7.0 && keys[3].key == "enter");
	CHECK(parseKeyScript("", keys, &err) && keys.empty());
	CHECK(!parseKeyScript("3", keys, &err));
	CHECK(!parseKeyScript("x:3", keys, &err));
	CHECK(!parseKeyScript("2:", keys, &err));
	CHECK(!parseKeyScript("2:sideways", keys, &err));
	CHECK(!parseKeyScript("-1:a", keys, &err));

	Options o;
	REQUIRE(parse({ "replay", "/tmp/x.plkv", "--keys", "1:2,2:m" }, o));
	CHECK(o.keys == "1:2,2:m");
	CHECK(!parse({ "replay", "/tmp/x.plkv", "--keys", "soon:2" }, o, &err));
}

PLK_TEST(options_hdr_brightness)
{
	Options o;
	REQUIRE(parse({ "stream", "PS5-466", "192.168.1.164" }, o));
	CHECK(!o.hdr_brightness);
	CHECK(resolveViewChoices(o).hdr_brightness == 100);
	REQUIRE(parse({ "stream", "PS5-466", "192.168.1.164", "--hdr-brightness", "55" }, o));
	REQUIRE(o.hdr_brightness);
	CHECK(resolveViewChoices(o).hdr_brightness == 55);
	CHECK(!parse({ "stream", "PS5-466", "192.168.1.164", "--hdr-brightness", "10" }, o));
	CHECK(!parse({ "stream", "PS5-466", "192.168.1.164", "--hdr-brightness", "250" }, o));
	CHECK(!parse({ "stream", "PS5-466", "192.168.1.164", "--hdr-brightness", "bright" }, o));

	// Steps of 5 on a grid of 5, within 20 to 200.
	CHECK(stepHdrBrightness(100, 1) == 105);
	CHECK(stepHdrBrightness(100, -1) == 95);
	CHECK(stepHdrBrightness(100, 0) == 100);
	CHECK(stepHdrBrightness(57, 1) == 60);
	CHECK(stepHdrBrightness(57, -1) == 55);
	CHECK(stepHdrBrightness(200, 1) == 200);
	CHECK(stepHdrBrightness(20, -1) == 20);
	CHECK(stepHdrBrightness(22, -1) == 20);
	CHECK(stepHdrBrightness(198, 1) == 200);
}
