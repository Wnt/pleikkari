// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "Runner.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "AudioOutput.h"
#include "Controller.h"
#include "FrameHud.h"
#include "FrameTelemetry.h"
#include "Gamepads.h"
#include "Log.h"
#include "NeuralEnhancer.h"
#include "Registration.h"
#include "Replay.h"
#include "StreamSession.h"
#include "VideoPath.h"

#include <pthread.h>

#include <chrono>
#include <cstdlib>
#include <cstring>

namespace plk::app {

struct RunnerSecrets
{
	ConnectTarget target; // registration keys; wiped by its destructor
};

namespace {

std::string distLine(const char *name, const video::Distribution &d)
{
	char buf[160];
	if(!d.count)
		return std::string(" ") + name + "=-";
	snprintf(buf, sizeof(buf), " %s=%.2f/%.2f/%.2f/%.2f", name, d.p50, d.p95, d.p99, d.max);
	return buf;
}

bool envSet(const char *name)
{
	const char *v = getenv(name);
	return v && *v && strcmp(v, "0") != 0;
}

} // namespace

Runner::Runner(const Options &options) : options_(options), secrets_(std::make_unique<RunnerSecrets>())
{
	chiaki_controller_state_set_idle(&dualsense_state_);
	chiaki_controller_state_set_idle(&gamepad_state_);
}

void Runner::pushControllerState(bool from_gamepads, const ChiakiControllerState &state)
{
	{
		std::lock_guard<std::mutex> lk(pad_state_mutex_);
		(from_gamepads ? gamepad_state_ : dualsense_state_) = state;
	}
	sendControllerState();
}

void Runner::sendControllerState()
{
	ChiakiControllerState merged;
	{
		std::lock_guard<std::mutex> lk(pad_state_mutex_);
		// The DualSense first: it is the one with motion.
		chiaki_controller_state_or(&merged, &dualsense_state_, &gamepad_state_);
	}
	if(play_holds_.load())
	{
		std::lock_guard<std::mutex> lk(pad_state_mutex_);
		merged = play_state_;
	}
	if(probe_holds_.load())
	{
		merged.right_x = (int16_t)std::lround(std::max(-1.0f, std::min(1.0f, probe_stick_.load())) * 32767.0f);
		merged.right_y = (int16_t)std::lround(std::max(-1.0f, std::min(1.0f, probe_stick_y_.load())) * 32767.0f);
		if(options_.probe_hold_l2)
		{
			merged.l2_state = 0xff;
			merged.buttons |= CHIAKI_CONTROLLER_ANALOG_BUTTON_L2;
		}
	}
	const double now_us = video::hostTimeUs();
	// L2 held far enough to aim: the game answers the stick another way then.
	const bool aiming = merged.l2_state > 100;
	// And L1, which in many games opens a wheel that the stick then picks from.
	// And a view that R3 switches to while L2 is held, and that lasts until L2 is let go (a
	// scope; in one game a closer look at things): the stick is answered differently there.
	const bool r3 = (merged.buttons & CHIAKI_CONTROLLER_BUTTON_R3) != 0;
	if(!aiming)
		warp_other_view_ = false;
	else if(r3 && !warp_r3_was_)
		warp_other_view_ = !warp_other_view_;
	warp_r3_was_ = r3;
	const int warp_mode = (aiming ? 1 : 0) | ((merged.buttons & CHIAKI_CONTROLLER_BUTTON_L1) ? 2 : 0) | (warp_other_view_ ? 4 : 0);
	camera_warp_.onStick(now_us, merged.right_x / 32767.0f, merged.right_y / 32767.0f, warp_mode);
	if(pipeline_)
		pipeline_->setTurning(std::max(std::abs((int)merged.right_x), std::abs((int)merged.right_y)) > 16400);
	{
		std::lock_guard<std::mutex> lk(probe_mutex_);
		if(sweep_stick_)
			fprintf(sweep_stick_, "%.0f,%.4f,%.4f,%d\n", now_us, merged.right_x / 32767.0, merged.right_y / 32767.0, aiming ? 1 : 0);
		// The buttons, the triggers and the sticks, whenever one of them changed. (Not the
		// touchpad nor the motion sensors: a game played with them is not played back by this.)
		if(input_log_ && (merged.buttons != input_logged_.buttons || merged.l2_state != input_logged_.l2_state
				|| merged.r2_state != input_logged_.r2_state || merged.left_x != input_logged_.left_x
				|| merged.left_y != input_logged_.left_y || merged.right_x != input_logged_.right_x
				|| merged.right_y != input_logged_.right_y || ftell(input_log_) < 60))
		{
			fprintf(input_log_, "%.0f,%u,%u,%u,%d,%d,%d,%d\n", now_us, (unsigned)merged.buttons, (unsigned)merged.l2_state,
					(unsigned)merged.r2_state, (int)merged.left_x, (int)merged.left_y, (int)merged.right_x, (int)merged.right_y);
			input_logged_ = merged;
			fflush(input_log_);
		}
	}
	if(session_)
		session_->setControllerState(merged);
}

// The pipeline measures how far each new picture moved only while someone wants to know.
void Runner::applyWarp()
{
	if(!pipeline_)
		return;
	const bool warp = view_.camera_warp, probe = options_.lag_probe > 0 || !options_.stick_sweep.empty()
			|| !options_.record_input.empty() || !options_.control_socket.empty();
	if(!warp && !probe)
	{
		pipeline_->setWarp(nullptr, nullptr);
		return;
	}
	pipeline_->setWarp([this](const video::PictureMotion &m) {
		camera_warp_.onMotion(m.previousUs, m.pictureUs, m.dx, m.dy, m.valid);
		std::lock_guard<std::mutex> lk(probe_mutex_);
		// Every ten seconds or so, what the warp has learned, for the log.
		if(view_.camera_warp && ++warp_logged_ % 400 == 0)
		{
			double cost = 0;
			uint64_t skipped = 0;
			if(pipeline_)
				pipeline_->motionCost(&cost, &skipped);
			PLK_LOGI("camera warp: measuring a picture takes %.1f ms, %llu not measured, told %.0f ms after it arrived; %s", cost,
					(unsigned long long)skipped, (video::hostTimeUs() - m.pictureUs) / 1000.0, camera_warp_.describe().c_str());
		}
		if(sweep_motion_)
		{
			// With what the camera warp would show this picture moved by, a refresh from now.
			float wx = 0, wy = 0;
			camera_warp_.predict(m.pictureUs, m.pictureUs + 8000.0, &wx, &wy);
			const CameraWarp::State w = camera_warp_.state();
			fprintf(sweep_motion_, "%.0f,%.0f,%.6f,%.6f,%d,%.6f,%.6f,%.0f,%.4f,%.4f,%.3f\n", m.previousUs, m.pictureUs, m.dx, m.dy,
					m.valid ? 1 : 0, wx, wy, w.delay_ms, w.gain_x, w.gain_y, w.explained);
		}
		// A pair of pictures with no common shift moved, and far, as far as the probe goes.
		const double moved = m.valid ? std::abs((double)m.dx) : 1.0;
		if(!probe_armed_)
		{
			if(m.valid)
				probe_recent_[probe_recent_at_++ % 8] = moved;
			return;
		}
		// The first picture that moves clearly more than the pictures did while the stick
		// was still (a camera that drifts back by itself is not the stick's doing): three
		// times that, and at least 0.3 % of the width, six pixels of 1920.
		double quiet = 0;
		for(double r : probe_recent_)
			quiet = std::max(quiet, r);
		if(probe_arrival_us_ == 0 && m.pictureUs > probe_stick_us_ && moved > std::max(0.003, quiet * 3.0))
		{
			probe_arrival_us_ = m.pictureUs;
			probe_cond_.notify_all();
		}
	}, warp ? std::function<void(double, double, float *, float *)>([this](double picture_us, double display_us, float *dx, float *dy) {
		camera_warp_.predict(picture_us, display_us, dx, dy);
		// Never a jump: what is shown moves towards what is guessed at no more than two
		// pictures' width a second. A guess that changes its mind between two frames (a new
		// picture came, a step of the table was relearned) is then a glide, not a jerk.
		const double step = std::max(0.0, std::min(0.05, (display_us - warp_shown_us_) / 1e6)) * 2.0;
		warp_shown_x_ += (float)std::max(-step, std::min(step, (double)(*dx - warp_shown_x_)));
		warp_shown_y_ += (float)std::max(-step, std::min(step, (double)(*dy - warp_shown_y_)));
		warp_shown_us_ = display_us;
		*dx = warp_shown_x_;
		*dy = warp_shown_y_;
	}) : nullptr);
}

void Runner::setCameraWarp(bool on)
{
	view_.camera_warp = on;
	applyWarp();
	PLK_LOGI("Video: camera warp %s", on ? "on" : "off");
}

// --lag-probe: the right stick is pushed over, to the right and to the left in turn, and
// the time to the first picture that moves is taken: to when it arrived, and to when it was
// on screen. The player has to stand where the camera can turn and nothing else moves it.
void Runner::lagProbeLoop()
{
	pthread_setname_np("plk.lag-probe");
	auto wait = [this](double seconds) {
		std::unique_lock<std::mutex> lk(probe_mutex_);
		return !probe_cond_.wait_for(lk, std::chrono::duration<double>(seconds), [this] { return probe_stop_; });
	};
	// The stream has to be up and the picture settled.
	for(int i = 0; i < 600; i++)
	{
		if(!wait(0.1))
			return;
		if(pipeline_ && pipeline_->counters().framesRendered > 300)
			break;
	}
	if(!wait(2.0))
		return;
	std::vector<double> arrival, glass;
	int skipped = 0;
	uint32_t random = 12345;
	PLK_LOGI("lag-probe: %d measurements with the right stick", options_.lag_probe);
	for(int i = 0; i < options_.lag_probe; i++)
	{
		// A pause of 1.2 to 1.7 s with the stick still: the camera comes to rest, and no two
		// pushes fall in step with the stream's frames.
		random = random * 1664525u + 1013904223u;
		if(!wait(1.2 + (double)(random >> 16) / 65535.0 * 0.5))
			break;
		const int direction = i % 2 == 0 ? 1 : -1;
		double quiet;
		{
			std::lock_guard<std::mutex> lk(probe_mutex_);
			quiet = 0;
			for(double r : probe_recent_)
				quiet = std::max(quiet, r);
			probe_arrival_us_ = probe_glass_us_ = 0;
			probe_armed_ = true;
			probe_stick_us_ = video::hostTimeUs();
		}
		probe_holds_.store(true);
		probe_stick_.store((float)direction);
		sendControllerState();
		double stick_us, arrival_us, glass_us;
		{
			std::unique_lock<std::mutex> lk(probe_mutex_);
			probe_cond_.wait_for(lk, std::chrono::milliseconds(250), [this] { return probe_stop_; });
			if(probe_stop_)
				break;
			lk.unlock();
			// A quarter of a second over, then back: the next one turns the camera the other way.
			probe_stick_.store(0.0f);
			sendControllerState();
			lk.lock();
			probe_cond_.wait_for(lk, std::chrono::milliseconds(450), [this] { return probe_stop_ || (probe_arrival_us_ > 0 && probe_glass_us_ > 0); });
			if(probe_stop_)
				break;
			probe_armed_ = false;
			stick_us = probe_stick_us_;
			arrival_us = probe_arrival_us_;
			glass_us = probe_glass_us_;
		}
		// Over the last eight pictures before the push: under 0.4 % of the width a picture.
		if(quiet > 0.004 || arrival_us == 0)
		{
			skipped++;
			PLK_LOGI("lag-probe: %d %s skipped (%s)", i + 1, direction > 0 ? "right" : "left",
					arrival_us == 0 ? "the picture did not move" : "the picture was moving before the stick was");
			continue;
		}
		arrival.push_back((arrival_us - stick_us) / 1000.0);
		if(glass_us > 0)
			glass.push_back((glass_us - stick_us) / 1000.0);
		PLK_LOGI("lag-probe: %d %s stick to picture arrived %.1f ms, to on screen %.1f ms", i + 1, direction > 0 ? "right" : "left",
				(arrival_us - stick_us) / 1000.0, glass_us > 0 ? (glass_us - stick_us) / 1000.0 : -1.0);
	}
	probe_stick_.store(0.0f);
	sendControllerState();
	probe_holds_.store(false);
	auto summary = [](std::vector<double> v) {
		char line[200] = "none";
		if(!v.empty())
		{
			std::sort(v.begin(), v.end());
			double sum = 0;
			for(double x : v)
				sum += x;
			snprintf(line, sizeof(line), "n=%zu min=%.1f p10=%.1f median=%.1f mean=%.1f p90=%.1f max=%.1f", v.size(), v.front(),
					v[v.size() / 10], v[v.size() / 2], sum / (double)v.size(), v[std::min(v.size() - 1, v.size() * 9 / 10)], v.back());
		}
		return std::string(line);
	};
	PLK_LOGI("lag-probe result: stream %s %d fps %u kbit/s %s; stick to picture arrived ms: %s", resolutionName(stream_config_.session.resolution),
			stream_config_.fps, stream_config_.effective_bitrate_kbps, codecName(stream_config_.session.codec), summary(arrival).c_str());
	PLK_LOGI("lag-probe result: stick to on screen ms: %s; skipped %d", summary(glass).c_str(), skipped);
	bool stopped;
	{
		std::lock_guard<std::mutex> lk(probe_mutex_);
		stopped = probe_stop_;
	}
	if(!stopped)
		requestStop();
}

Runner::~Runner()
{
	requestStop();
	shutdownWorkers();
	shutdownVideo();
}

std::string Runner::title() const
{
	if(options_.command == Command::Replay)
		return "Pleikkari - replay";
	return "Pleikkari - " + options_.nickname;
}

bool Runner::prepare(int *exit_code)
{
	*exit_code = 0;
	if(!options_.log_path.empty())
	{
		std::string err;
		if(!Log::instance().open(options_.log_path, &err))
		{
			fprintf(stderr, "Pleikkari: %s\n", err.c_str());
			*exit_code = 1;
			return false;
		}
	}
	PLK_LOGI("Pleikkari starting: %s", options_.command == Command::Replay ? "replay" : "stream");
	view_ = resolveViewChoices(options_);

	if(options_.command == Command::Replay)
	{
		stream_config_ = resolveStreamConfig(options_, nullptr);
		return true;
	}

	// Registration and the Qt app's stream settings: read through the loader only. Nothing
	// read from the file is logged but the nickname and the console type.
	const std::string plist = qtSettingsPlistPath(options_.profile);
	std::vector<RegisteredConsole> consoles;
	std::string err;
	if(!loadRegisteredConsoles(plist, consoles, &err))
	{
		PLK_LOGE("Cannot read the console registrations: %s", err.c_str());
		*exit_code = 1;
		return false;
	}
	const RegisteredConsole *console = findConsole(consoles, options_.nickname);
	if(!console)
	{
		std::string names;
		for(const auto &c : consoles)
			names += (names.empty() ? "" : ", ") + c.nickname;
		PLK_LOGE("No registered console named \"%s\" (registered: %s)", options_.nickname.c_str(),
				names.empty() ? "none" : names.c_str());
		*exit_code = 1;
		return false;
	}
	if(!console->usable())
	{
		PLK_LOGE("The registration of \"%s\" is incomplete: register the console again in the Qt app",
				options_.nickname.c_str());
		*exit_code = 1;
		return false;
	}
	secrets_->target = ConnectTarget::fromRegistration(*console, options_.host);

	QtStreamSettings qt;
	const bool have_qt = loadQtStreamSettings(plist, qt, nullptr);
	stream_config_ = resolveStreamConfig(options_, have_qt ? &qt : nullptr);
	const StreamSettings &s = stream_config_.session;
	ps5_ = console->isPS5();
	// HDR off goes back to the codec the settings name, if that is an SDR one.
	{
		Options without_hdr = options_;
		without_hdr.hdr.reset();
		sdr_codec_ = codecWithHdr(resolveStreamConfig(without_hdr, have_qt ? &qt : nullptr).session.codec, false);
	}
	PLK_LOGI("Console \"%s\" (%s) at %s", console->nickname.c_str(), console->consoleTypeName(), options_.host.c_str());
	PLK_LOGI("Stream: %dx%d (%s) at %d fps, %u kbit/s, codec %s, feedback interval %s ms, packet reordering %s, "
			"packet loss max %.2f, IDR on FEC failure %s",
			stream_config_.width, stream_config_.height, resolutionName(s.resolution), stream_config_.fps,
			stream_config_.effective_bitrate_kbps, codecName(s.codec),
			s.feedback_state_min_interval_ms ? std::to_string(s.feedback_state_min_interval_ms).c_str() : "8 (lib default)",
			s.disable_video_packet_reordering ? "off" : "on", s.packet_loss_max, s.idr_on_fec_failure ? "on" : "off");
	PLK_LOGI("Settings from: %s", stream_config_.source.c_str());
	if(!console->isPS5() && s.codec != CHIAKI_CODEC_H264)
		PLK_LOGW("A PS4 streams H.264 only; the codec setting is ignored");
	return true;
}

bool Runner::createPipeline(void *metalLayer, float display_refresh_hz)
{
	frame_logger_ = std::make_unique<FrameLogger>(options_.frame_log);
	video::PipelineConfig pc;
	pc.codec = video::Codec::Auto;
	pacing_ = effectivePacing(options_.pacing, window_fullscreen_, view_.fullscreen_sync);
	pc.pacing = pacing_;
	pc.scaler = view_.scaler;
	pc.hdrOutput = view_.hdr_output;
	pc.hdrBrightness = view_.hdr_brightness / 100.0f;
	pc.hdrToneMap = view_.hdr_tone_map;
	pc.displayHeadroom = display_headroom_.load();
	pc.sdrColor = view_.sdr_color;
	pc.displayRefreshHz = display_refresh_hz > 0 ? display_refresh_hz : 120.0f;
	if(options_.headless)
	{
		pc.offscreenWidth = options_.offscreen_width;
		pc.offscreenHeight = options_.offscreen_height;
		pc.captureDir = options_.capture_dir;
		pc.captureSkip = options_.capture_skip;
		pc.captureCount = options_.capture_frames;
		pc.captureEvery = options_.capture_every;
	}
	analysis_ = std::make_unique<FrameAnalysis>();
	FrameLogger *logger = frame_logger_.get();
	FrameAnalysis *analysis = analysis_.get();
	pc.onFrame = [this, logger, analysis](const video::FrameTiming &t) {
		logger->onFrame(t);
		analysis->onFrame(t);
		// The lag probe: when the picture it waits for was on screen.
		if(options_.lag_probe > 0 && t.presented)
		{
			std::lock_guard<std::mutex> lk(probe_mutex_);
			if(probe_armed_ && probe_arrival_us_ > 0 && probe_glass_us_ == 0 && t.decodedUs >= probe_arrival_us_)
			{
				probe_glass_us_ = t.presentedUs;
				probe_cond_.notify_all();
			}
		}
	};
	pc.onEnhance = [analysis](const video::EnhanceTiming &t) { analysis->onEnhance(t, video::hostTimeUs()); };

	std::string err;
	pipeline_ = video::VideoPipeline::create(metalLayer, pc, [this](const video::Error &e) {
		PLK_LOGW("video: %s (status %d)%s", e.message.c_str(), (int)e.status, e.requestIdr ? ", requesting IDR" : "");
		if(e.requestIdr && session_)
			session_->scheduleKeyframeRequest("decoder");
	}, &err);
	if(!pipeline_)
	{
		PLK_LOGE("Video pipeline: %s", err.c_str());
		exit_code_ = 1;
		return false;
	}
	PLK_LOGI("Video: pacing %s, scaler %s, SDR colour %s, HDR output %s, HDR brightness %d %%, HDR tone mapping %s, display %.0f Hz%s",
			video::toString(pacing_), video::toString(view_.scaler), video::toString(view_.sdr_color),
			video::toString(view_.hdr_output), view_.hdr_brightness, video::toString(view_.hdr_tone_map), pc.displayRefreshHz,
			options_.headless ? ", headless (offscreen)" : "");
	video_path_ = std::make_unique<VideoPath>(pipeline_.get());
	if(!options_.dump_path.empty())
	{
		if(!video_path_->openDump(options_.dump_path, &err))
			PLK_LOGW("Video dump: %s", err.c_str());
		else
		{
			// A stream that starts later (after H, say) gets a file of its own, named after
			// what it is; a replay of a dump is copied as whatever it holds.
			const bool stream = options_.command == Command::Stream;
			if(stream)
				video_path_->setDumpLabel(chiaki_codec_is_hdr(stream_config_.session.codec) ? "hdr" : "sdr");
			PLK_LOGI("Recording the video samples to %s%s%s; a stream that starts later goes to %s and so on",
					options_.dump_path.c_str(), stream ? ", " : "",
					!stream ? "" : chiaki_codec_is_hdr(stream_config_.session.codec) ? "an HDR stream" : "an SDR stream",
					dumpSegmentPath(options_.dump_path, 2, stream ? "hdr|sdr" : "").c_str());
		}
	}
	{
		std::lock_guard<std::mutex> lk(hud_mutex_);
		hud_on_ = view_.fps_overlay;
		// The window says what its display's is; an offscreen target of a Retina panel's
		// size gets the scale such a panel has.
		if(options_.headless)
			hud_scale_ = options_.offscreen_height >= 1600 ? 2 : 1;
	}
	hud_thread_ = std::thread([this] { hudLoop(); });
	// Neural rendering: the engine is loaded beside the stream, which starts without it.
	nr_thread_ = std::thread([this] { nrLoop(); });
	applyEnhanceConfig();
	if(view_.frame_generation)
		setFrameGeneration(true);
	if(!options_.record_input.empty() && options_.command == Command::Stream)
	{
		std::lock_guard<std::mutex> lk(probe_mutex_);
		input_log_ = fopen((options_.record_input + "-input.csv").c_str(), "w");
		sweep_motion_ = fopen((options_.record_input + "-motion.csv").c_str(), "w");
		if(!input_log_ || !sweep_motion_)
			PLK_LOGE("record-input: cannot write %s-*.csv", options_.record_input.c_str());
		if(input_log_)
			fprintf(input_log_, "t_us,buttons,l2,r2,left_x,left_y,right_x,right_y\n");
		if(sweep_motion_)
			fprintf(sweep_motion_, "previous_us,picture_us,dx,dy,valid,warp_x,warp_y,delay_ms,gain_x,gain_y,explained\n");
		PLK_LOGI("record-input: to %s-input.csv, -motion.csv and the video dump", options_.record_input.c_str());
	}
	loadWarp();
	applyWarp();
	if(!options_.control_socket.empty() && options_.command == Command::Stream)
		control_thread_ = std::thread([this] { controlLoop(); });
	if(!options_.play_input.empty() && options_.command == Command::Stream)
		probe_thread_ = std::thread([this] { inputPlayLoop(); });
	else if(options_.lag_probe > 0 && options_.command == Command::Stream)
		probe_thread_ = std::thread([this] { lagProbeLoop(); });
	else if(!options_.stick_sweep.empty() && options_.command == Command::Stream)
		probe_thread_ = std::thread([this] { stickSweepLoop(); });
	if(view_.nr)
	{
		const std::string missing = nrMissing(resolveNrPaths(options_, view_.nr_network));
		if(missing.empty())
			requestNrEngine();
		else
		{
			// Not installed is the ordinary case of a plain build: one line, no warning.
			PLK_LOGI("Neural rendering: %s", missing.c_str());
			std::lock_guard<std::mutex> lk(nr_mutex_);
			nr_failure_ = missing;
		}
	}
	else
		PLK_LOGI("Neural rendering: off");
	return true;
}

void Runner::setDrawableSize(int width, int height)
{
	if(pipeline_ && width > 0 && height > 0)
		pipeline_->setDrawableSize(width, height);
}

Runner::ViewState Runner::viewState() const
{
	ViewState v;
	v.scaler = view_.scaler;
	v.sdr_color = view_.sdr_color;
	v.hdr_output = view_.hdr_output;
	v.hdr_brightness = view_.hdr_brightness;
	v.hdr_tone_map = view_.hdr_tone_map;
	v.fullscreen_sync = view_.fullscreen_sync;
	v.fullscreen_sync_applies = options_.pacing == video::PacingMode::Immediate;
	v.hdr = stream_config_.session.codec == CHIAKI_CODEC_H265_HDR && options_.command == Command::Stream;
	v.hdr_switchable = options_.command == Command::Stream && ps5_ && session_ && !stop_requested_.load();
	v.nr = view_.nr;
	v.nr_network = view_.nr_network;
	v.nr_height = view_.nr_height;
	v.nr_strength = view_.nr_strength;
	v.nr_look = view_.nr_look;
	v.nr_compare = nr_compare_;
	v.nr_compare_split = nr_compare_split_;
	v.resolution = stream_config_.session.resolution;
	v.bitrate_kbps = stream_config_.effective_bitrate_kbps;
	v.nr_timing = view_.nr_timing;
	v.frame_generation = view_.frame_generation;
	v.frame_generation_factor = view_.frame_generation_factor;
	v.camera_warp = view_.camera_warp;
	v.camera_warp_state = camera_warp_.state();
	{
		const std::string missing = frameGenerationMissing(resolveNrPaths(options_, view_.nr_network).library,
				frameGenerationWeightsPath(options_));
		std::lock_guard<std::mutex> lk(nr_mutex_);
		v.frame_generation_available = missing.empty() && fg_failure_.empty();
		v.frame_generation_status = !missing.empty() ? missing : !fg_failure_.empty() ? fg_failure_
				: fg_loading_ ? "loading" : fg_engine_ ? fg_engine_->describe() : "not loaded";
	}
	v.fps_overlay = view_.fps_overlay;
	v.panel_beside = view_.panel_beside;
	std::string failure;
	{
		std::lock_guard<std::mutex> lk(nr_mutex_);
		v.nr_loading = nr_loading_ || nr_request_;
		if(nr_engine_ && nr_engine_network_ == view_.nr_network)
		{
			v.nr_available = true;
			v.nr_status = nr_engine_->describe();
		}
		failure = nr_failure_;
	}
	if(!v.nr_available)
	{
		// Not loaded (yet): what a load would find.
		const std::string missing = nrMissing(resolveNrPaths(options_, view_.nr_network));
		v.nr_available = missing.empty();
		v.nr_status = !missing.empty() ? missing : v.nr_loading ? "loading" : !failure.empty() ? failure : "not loaded";
	}
	return v;
}

void Runner::setScaler(video::Scaler scaler)
{
	view_.scaler = scaler;
	if(pipeline_)
		pipeline_->setScaler(scaler);
	if(analysis_)
		analysis_->clearBaseline();
	PLK_LOGI("Video: scaler %s", video::toString(scaler));
}

void Runner::setSdrColor(video::SdrColor color)
{
	view_.sdr_color = color;
	if(pipeline_)
		pipeline_->setSdrColor(color);
	PLK_LOGI("Video: SDR colour %s", video::toString(color));
}

void Runner::setHdrOutput(video::HdrOutput output)
{
	view_.hdr_output = output;
	if(pipeline_)
		pipeline_->setHdrOutput(output);
	if(analysis_)
		analysis_->clearBaseline();
	PLK_LOGI("Video: HDR output %s", video::toString(output));
}

int Runner::setHdrBrightness(int percent)
{
	percent = percent < kHdrBrightnessMin ? kHdrBrightnessMin : percent > kHdrBrightnessMax ? kHdrBrightnessMax : percent;
	view_.hdr_brightness = percent;
	if(pipeline_)
		pipeline_->setHdrBrightness(percent / 100.0f);
	PLK_LOGI("Video: HDR brightness %d %%", percent);
	return percent;
}

void Runner::setHdrToneMap(video::HdrToneMap tone_map)
{
	view_.hdr_tone_map = tone_map;
	if(pipeline_)
		pipeline_->setHdrToneMap(tone_map);
	if(analysis_)
		analysis_->clearBaseline();
	PLK_LOGI("Video: HDR tone mapping %s", video::toString(tone_map));
}

void Runner::setFullscreenSync(bool on)
{
	view_.fullscreen_sync = on;
	applyPacing();
}

void Runner::setWindowFullscreen(bool fullscreen)
{
	window_fullscreen_ = fullscreen;
	applyPacing();
}

void Runner::applyPacing()
{
	const video::PacingMode mode = effectivePacing(options_.pacing, window_fullscreen_, view_.fullscreen_sync);
	if(mode == pacing_)
		return;
	pacing_ = mode;
	if(pipeline_)
		pipeline_->setPacingMode(mode);
	// In step with the display a frame waits for a refresh: what was measured without says
	// nothing about it.
	if(analysis_)
		analysis_->clearBaseline();
	PLK_LOGI("Video: pacing %s (%s, fullscreen sync %s)", video::toString(mode),
			window_fullscreen_ ? "fullscreen" : "window", view_.fullscreen_sync ? "on" : "off");
}

void Runner::setFpsOverlay(bool on)
{
	view_.fps_overlay = on;
	{
		std::lock_guard<std::mutex> lk(hud_mutex_);
		hud_on_ = on;
	}
	hud_cond_.notify_all();
	PLK_LOGI("Video: frame rate overlay %s", on ? "on" : "off");
}

void Runner::setOverlayScale(int scale)
{
	{
		std::lock_guard<std::mutex> lk(hud_mutex_);
		hud_scale_ = scale < 1 ? 1 : scale > 4 ? 4 : scale;
	}
	hud_cond_.notify_all();
}

void Runner::setPanelBeside(bool beside)
{
	view_.panel_beside = beside;
}

void Runner::setContentInsets(int left, int top, int right, int bottom)
{
	if(pipeline_)
		pipeline_->setContentInsets(left, top, right, bottom);
	// A picture of another size costs the scaler another amount.
	if(analysis_)
		analysis_->clearBaseline();
}

// Draws the overlay ten times a second while it is on; the pipeline shows the newest one
// with every frame.
void Runner::hudLoop()
{
	pthread_setname_np("plk.hud");
	std::unique_lock<std::mutex> lk(hud_mutex_);
	bool shown = false;
	while(!hud_stop_)
	{
		if(!hud_on_)
		{
			if(shown)
			{
				lk.unlock();
				pipeline_->setOverlay(video::OverlayImage());
				lk.lock();
				shown = false;
				continue;
			}
			hud_cond_.wait(lk, [this] { return hud_stop_ || hud_on_; });
			continue;
		}
		const int scale = hud_scale_;
		lk.unlock();
		const HudImage image = renderFrameHud(analysis_->pace(video::hostTimeUs()), scale);
		if(image.width > 0)
		{
			video::OverlayImage overlay;
			overlay.bgra = image.bgra.data();
			overlay.width = image.width;
			overlay.height = image.height;
			overlay.bytesPerRow = image.bytes_per_row;
			overlay.x = overlay.y = kHudMargin * scale;
			pipeline_->setOverlay(overlay);
			shown = true;
		}
		lk.lock();
		hud_cond_.wait_for(lk, std::chrono::milliseconds(100), [this, scale] { return hud_stop_ || !hud_on_ || hud_scale_ != scale; });
	}
}

void Runner::setDisplayHeadroom(float headroom)
{
	display_headroom_ = headroom;
	if(pipeline_)
		pipeline_->setDisplayHeadroom(headroom);
}

bool Runner::setHdr(bool on)
{
	if(!viewState().hdr_switchable)
		return false;
	StreamSettings settings = stream_config_.session;
	settings.codec = codecWithHdr(settings.codec, on, sdr_codec_);
	if(settings.codec == stream_config_.session.codec)
		return true;
	PLK_LOGI("HDR %s: restarting the session with codec %s", on ? "on" : "off", codecName(settings.codec));
	const uint64_t restarts = session_->restarts();
	// A recording names the file of the stream that starts now after what it is.
	if(video_path_)
		video_path_->setDumpLabel(on ? "hdr" : "sdr");
	if(!session_->restart(settings))
	{
		if(video_path_)
			video_path_->setDumpLabel(on ? "sdr" : "hdr");
		return false;
	}
	hdr_switch_restart_ = restarts + 1;
	stream_config_.session = settings;
	if(analysis_)
		analysis_->clearBaseline();
	return true;
}

// The console fixes the picture's size when a session starts, as it does the codec: the
// session is restarted with the other preset, and hdrSwitchSettled() says when it is back.
bool Runner::setStreamResolution(ChiakiVideoResolutionPreset resolution)
{
	if(!viewState().hdr_switchable)
		return false;
	StreamSettings settings = stream_config_.session;
	if(settings.resolution == resolution)
		return true;
	settings.resolution = resolution;
	PLK_LOGI("Stream resolution %s: restarting the session", resolutionName(resolution));
	const uint64_t restarts = session_->restarts();
	if(!session_->restart(settings))
		return false;
	hdr_switch_restart_ = restarts + 1;
	stream_config_.session = settings;
	ChiakiConnectVideoProfile profile;
	memset(&profile, 0, sizeof(profile));
	chiaki_connect_video_profile_preset(&profile, resolution, settings.fps);
	stream_config_.width = (int)profile.width;
	stream_config_.height = (int)profile.height;
	if(analysis_)
		analysis_->clearBaseline();
	return true;
}

bool Runner::setStreamBitrate(unsigned kbps)
{
	if(!viewState().hdr_switchable || kbps < 1000 || kbps > 100000)
		return false;
	if(stream_config_.effective_bitrate_kbps == kbps)
		return true;
	StreamSettings settings = stream_config_.session;
	settings.bitrate_kbps = kbps;
	PLK_LOGI("Stream bitrate %u kbit/s: restarting the session", kbps);
	const uint64_t restarts = session_->restarts();
	if(!session_->restart(settings))
		return false;
	hdr_switch_restart_ = restarts + 1;
	stream_config_.session = settings;
	stream_config_.effective_bitrate_kbps = kbps;
	if(analysis_)
		analysis_->clearBaseline();
	return true;
}

bool Runner::hdrSwitchSettled() const
{
	return session_ && session_->restarts() >= hdr_switch_restart_ && session_->connected();
}

bool Runner::pictureIsHdr() const
{
	return pipeline_ && pipeline_->counters().hdr;
}

// ---- neural rendering -------------------------------------------------------

namespace {

std::string heightName(int height)
{
	return height > 0 ? std::to_string(height) + "p" : std::string("the stream's size");
}

} // namespace

void Runner::setNoticeHandler(std::function<void(const std::string &)> handler)
{
	std::lock_guard<std::mutex> lk(nr_mutex_);
	notice_ = std::move(handler);
}

// What the pipeline does with the engine's results, from the menu's choices; and whether
// the analysis counts the frames from now on as frames without neural rendering.
void Runner::applyEnhanceConfig()
{
	if(!pipeline_)
		return;
	video::EnhanceConfig c;
	c.enabled = view_.nr;
	c.processHeight = view_.nr_height;
	c.strength = (float)view_.nr_strength / 100.0f;
	c.compareSplit = nr_compare_ ? nr_compare_split_ : 0.0f;
	c.wait = view_.nr_timing == NrTiming::Wait;
	c.guided = view_.nr_timing == NrTiming::Guided;
	pipeline_->setEnhanceConfig(c);
	std::lock_guard<std::mutex> lk(nr_mutex_);
	syncAnalysisActiveLocked();
}

// Whether the analysis counts the frames from now on as frames with neural rendering. Told
// from here only, with nr_mutex_ held, so that the menu's thread and the loader cannot
// leave it saying what is no longer true.
void Runner::syncAnalysisActiveLocked()
{
	if(analysis_)
		analysis_->setEnhancerActive(view_.nr && (nr_engine_ || nr_loading_ || nr_request_));
}

// Asks the loader for the engine of the chosen network, unless it is the one in use.
void Runner::requestNrEngine()
{
	if(!pipeline_ || !view_.nr)
		return;
	{
		std::lock_guard<std::mutex> lk(nr_mutex_);
		if(nr_stop_ || (nr_engine_ && nr_engine_network_ == view_.nr_network && !nr_loading_ && !nr_request_))
			return;
		nr_request_ = true;
		nr_request_network_ = view_.nr_network;
		nr_request_look_ = view_.nr_look;
		syncAnalysisActiveLocked();
	}
	nr_cond_.notify_all();
}

void Runner::nrLoop()
{
	pthread_setname_np("plk.nr-load");
	std::unique_lock<std::mutex> lk(nr_mutex_);
	for(;;)
	{
		nr_cond_.wait(lk, [this] { return nr_stop_ || nr_request_; });
		if(nr_stop_)
			return;
		nr_request_ = false;
		const NrNetwork network = nr_request_network_;
		const NrLook look = nr_request_look_;
		if(nr_engine_ && nr_engine_network_ == network)
			continue; // asked for again while it was being loaded
		nr_loading_ = true;
		lk.unlock();

		NeuralEnhancer::Config config;
		config.paths = resolveNrPaths(options_, network);
		config.look = look;
		std::string failure = nrMissing(config.paths);
		std::shared_ptr<NeuralEnhancer> engine;
		const auto began = std::chrono::steady_clock::now();
		if(failure.empty())
			engine = NeuralEnhancer::load(config, &failure);
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
		std::string notice;
		if(engine)
		{
			PLK_LOGI("Neural rendering: %s, loaded in %.0f ms from %s", engine->describe().c_str(), ms,
					config.paths.model.c_str());
			// Builds the pipeline's mixing shaders the first time: here, not on the main thread.
			pipeline_->setEnhancer(engine);
			notice = std::string("Neural rendering ready: ") + (network == NrNetwork::HT ? "original model (HT)" : "performance model");
		}
		else
		{
			PLK_LOGW("Neural rendering is not available: %s", failure.c_str());
			notice = "Neural rendering is not available: " + failure;
		}

		lk.lock();
		nr_loading_ = false;
		std::function<void(const std::string &)> tell = notice_;
		const bool more = nr_request_;
		// The engine this load replaces. Dropping it here is safe on any thread: an engine
		// ends on a queue of its own (NeuralEnhancer's destructor), whoever lets go last.
		std::shared_ptr<NeuralEnhancer> replaced = std::move(nr_engine_);
		nr_engine_ = engine;
		if(engine)
		{
			// A look picked while the model was loading.
			if(nr_request_look_ != look)
				engine->setLook(nr_request_look_);
			nr_engine_network_ = network;
			nr_failure_.clear();
		}
		else
			nr_failure_ = failure;
		syncAnalysisActiveLocked();
		lk.unlock();
		// A model that is there but does not load: the engine of the other network does not
		// go on working under a menu that names this one.
		if(!engine && replaced)
			pipeline_->setEnhancer(nullptr);
		replaced.reset();
		if(tell && !more)
			tell(notice);
		lk.lock();
	}
}

void Runner::setNr(bool on)
{
	{
		std::lock_guard<std::mutex> lk(nr_mutex_);
		view_.nr = on;
	}
	applyEnhanceConfig();
	requestNrEngine();
	PLK_LOGI("Video: neural rendering %s", on ? "on" : "off");
}

bool Runner::setNrNetwork(NrNetwork network, std::string *why)
{
	// A network whose model is not there is not switched to: the one in use keeps working
	// and the menu keeps naming it.
	const std::string missing = nrMissing(resolveNrPaths(options_, network));
	if(!missing.empty())
	{
		PLK_LOGI("Video: neural rendering network %s is not installed: %s", toString(network), missing.c_str());
		if(why)
			*why = missing;
		return false;
	}
	{
		std::lock_guard<std::mutex> lk(nr_mutex_);
		view_.nr_network = network;
	}
	requestNrEngine();
	PLK_LOGI("Video: neural rendering network %s", toString(network));
	return true;
}

void Runner::setNrHeight(int height)
{
	height = height <= 0 ? 0 : height < kNrHeightMin ? kNrHeightMin : height > kNrHeightMax ? kNrHeightMax : height;
	{
		std::lock_guard<std::mutex> lk(nr_mutex_);
		view_.nr_height = height;
	}
	applyEnhanceConfig();
	PLK_LOGI("Video: neural rendering at %s", heightName(view_.nr_height).c_str());
}

int Runner::setNrStrength(int percent)
{
	percent = percent < kNrStrengthMin ? kNrStrengthMin : percent > kNrStrengthMax ? kNrStrengthMax : percent;
	{
		std::lock_guard<std::mutex> lk(nr_mutex_);
		view_.nr_strength = percent;
	}
	applyEnhanceConfig();
	PLK_LOGI("Video: neural rendering strength %d %%", percent);
	return percent;
}

void Runner::setNrLook(NrLook look)
{
	{
		// Under the lock all the way: a load that ends now applies the look it finds here,
		// and the two must not land on the new engine in the wrong order.
		std::lock_guard<std::mutex> lk(nr_mutex_);
		view_.nr_look = look;
		nr_request_look_ = look;
		if(nr_engine_)
			nr_engine_->setLook(look);
	}
	PLK_LOGI("Video: neural rendering look %s", toString(look));
}

void Runner::setNrCompare(bool on)
{
	{
		std::lock_guard<std::mutex> lk(nr_mutex_);
		nr_compare_ = on;
	}
	applyEnhanceConfig();
	PLK_LOGI("Video: neural rendering comparison %s", on ? "on (left half as decoded)" : "off");
}

void Runner::setNrCompareSplit(float share)
{
	{
		std::lock_guard<std::mutex> lk(nr_mutex_);
		// Never at an edge: the line stays where it can be taken hold of again.
		nr_compare_split_ = std::min(0.98f, std::max(0.02f, share));
	}
	applyEnhanceConfig();
}

// Frame generation on or off. The first time on, the generator is loaded beside the stream,
// which goes on as it was until it is there.
void Runner::setFrameGeneration(bool on)
{
	view_.frame_generation = on;
	if(!pipeline_)
		return;
	pipeline_->setFrameGeneration(on ? view_.frame_generation_factor - 1 : 0);
	PLK_LOGI("Video: frame generation %s (%dx)", on ? "on" : "off", view_.frame_generation_factor);
	if(!on)
		return;
	{
		std::lock_guard<std::mutex> lk(nr_mutex_);
		if(fg_engine_ || fg_loading_ || nr_stop_)
			return;
		fg_loading_ = true;
	}
	if(fg_thread_.joinable())
		fg_thread_.join();
	{
		std::lock_guard<std::mutex> lk(probe_mutex_);
		probe_stop_ = true;
	}
	probe_cond_.notify_all();
	if(probe_thread_.joinable())
		probe_thread_.join();
	if(control_thread_.joinable())
	{
		if(pipeline_)
			pipeline_->setFrameTap(nullptr);
		// The thread waits in accept() or read(): both end when their socket is shut.
		const int listening = control_listen_.exchange(-1), client = control_client_.load();
		if(client >= 0)
			shutdown(client, SHUT_RDWR);
		if(listening >= 0)
		{
			shutdown(listening, SHUT_RDWR);
			close(listening);
		}
		control_thread_.join();
	}
	{
		std::lock_guard<std::mutex> lk(probe_mutex_);
		if(input_log_)
		{
			fclose(input_log_);
			input_log_ = nullptr;
			if(sweep_motion_)
				fclose(sweep_motion_);
			sweep_motion_ = nullptr;
		}
	}
	fg_thread_ = std::thread([this] {
		pthread_setname_np("plk.fg-load");
		const std::string library = resolveNrPaths(options_, view_.nr_network).library;
		const std::string weights = frameGenerationWeightsPath(options_);
		std::string failure = frameGenerationMissing(library, weights);
		std::shared_ptr<FrameGenerator> engine;
		if(failure.empty())
			engine = FrameGenerator::load(library, weights, &failure);
		if(engine)
		{
			PLK_LOGI("Frame generation: %s, weights %s", engine->describe().c_str(), weights.c_str());
			pipeline_->setInterpolator(engine);
		}
		else
			PLK_LOGW("Frame generation is not available: %s", failure.c_str());
		std::function<void(const std::string &)> tell;
		{
			std::lock_guard<std::mutex> lk(nr_mutex_);
			fg_engine_ = engine;
			fg_failure_ = engine ? std::string() : failure;
			fg_loading_ = false;
			tell = notice_;
		}
		if(tell)
			tell(engine ? "Frame generation ready" : "Frame generation is not available: " + failure);
	});
}

void Runner::setFrameGenerationFactor(int factor)
{
	view_.frame_generation_factor = factor == 4 ? 4 : 2;
	if(pipeline_ && view_.frame_generation)
		pipeline_->setFrameGeneration(view_.frame_generation_factor - 1);
	PLK_LOGI("Video: frame generation %dx", view_.frame_generation_factor);
}

void Runner::setNrTiming(NrTiming timing)
{
	view_.nr_timing = timing;
	applyEnhanceConfig();
	PLK_LOGI("Video: neural rendering timing %s", toString(timing));
}

// --stick-sweep: the right stick moved through set profiles, every one followed by its
// mirror image so that the camera ends where it began, and everything written down: the
// stick, how far each picture moved, and what the camera warp made of it at that moment.
// A recorded sitting played back: each state the controller sent, at the time it sent it,
// counted from the first. The game is not told that anything is different, and it is not:
// what it does with the same input depends on where it was when the input began.
void Runner::inputPlayLoop()
{
	pthread_setname_np("plk.input-play");
	auto wait = [this](double seconds) {
		std::unique_lock<std::mutex> lk(probe_mutex_);
		return !probe_cond_.wait_for(lk, std::chrono::duration<double>(seconds), [this] { return probe_stop_; });
	};
	struct Row { double t; ChiakiControllerState s; };
	std::vector<Row> rows;
	if(FILE *f = fopen(options_.play_input.c_str(), "r"))
	{
		char line[256];
		while(fgets(line, sizeof(line), f))
		{
			Row r;
			chiaki_controller_state_set_idle(&r.s);
			unsigned buttons, l2, r2;
			int lx, ly, rx, ry;
			if(sscanf(line, "%lf,%u,%u,%u,%d,%d,%d,%d", &r.t, &buttons, &l2, &r2, &lx, &ly, &rx, &ry) != 8)
				continue;
			r.s.buttons = buttons;
			r.s.l2_state = (uint8_t)l2;
			r.s.r2_state = (uint8_t)r2;
			r.s.left_x = (int16_t)lx;
			r.s.left_y = (int16_t)ly;
			r.s.right_x = (int16_t)rx;
			r.s.right_y = (int16_t)ry;
			rows.push_back(r);
		}
		fclose(f);
	}
	if(rows.size() < 2)
	{
		PLK_LOGE("play-input: nothing to play in %s", options_.play_input.c_str());
		requestStop();
		return;
	}
	for(int i = 0; i < 600; i++)
	{
		if(!wait(0.1))
			return;
		if(pipeline_ && pipeline_->counters().framesRendered > 300)
			break;
	}
	if(!wait(2.0))
		return;
	PLK_LOGI("play-input: %zu controller states over %.1f s from %s", rows.size(), (rows.back().t - rows.front().t) / 1e6,
			options_.play_input.c_str());
	const double began = video::hostTimeUs();
	bool stopped = false;
	for(const Row &r : rows)
	{
		const double due = began + (r.t - rows.front().t), now = video::hostTimeUs();
		if(due > now && !wait((due - now) / 1e6))
		{
			stopped = true;
			break;
		}
		{
			std::lock_guard<std::mutex> lk(pad_state_mutex_);
			play_state_ = r.s;
		}
		play_holds_.store(true);
		sendControllerState();
	}
	{
		std::lock_guard<std::mutex> lk(pad_state_mutex_);
		chiaki_controller_state_set_idle(&play_state_);
	}
	sendControllerState();
	if(!stopped)
		stopped = !wait(1.5);
	play_holds_.store(false);
	sendControllerState();
	PLK_LOGI("play-input: done");
	if(!stopped)
		requestStop();
}

// --control: one program at a time. It gets every new picture and what it sends is the
// controller, whole: the buttons, the triggers and both sticks. When it goes, or says
// "release", the controllers in the player's hands are the controller again.
void Runner::controlLoop()
{
	pthread_setname_np("plk.control");
	const int listening = socket(AF_UNIX, SOCK_STREAM, 0);
	sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	if(listening < 0 || options_.control_socket.size() >= sizeof(addr.sun_path))
	{
		PLK_LOGE("control: cannot make a socket at %s", options_.control_socket.c_str());
		return;
	}
	strncpy(addr.sun_path, options_.control_socket.c_str(), sizeof(addr.sun_path) - 1);
	unlink(addr.sun_path);
	if(bind(listening, (const sockaddr *)&addr, sizeof(addr)) != 0 || listen(listening, 1) != 0)
	{
		PLK_LOGE("control: cannot listen at %s: %s", options_.control_socket.c_str(), strerror(errno));
		close(listening);
		return;
	}
	control_listen_.store(listening);
	PLK_LOGI("control: waiting at %s", options_.control_socket.c_str());
	while(control_listen_.load() >= 0)
	{
		const int client = accept(listening, nullptr, nullptr);
		if(client < 0)
			break;
		int on = 1;
		setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
		int room = 1 << 20;
		setsockopt(client, SOL_SOCKET, SO_SNDBUF, &room, sizeof(room));
		control_client_.store(client);
		PLK_LOGI("control: a program is connected");
		if(pipeline_)
			pipeline_->setFrameTap([this](const uint8_t *grey, int w, int h, double picture_us) {
				std::lock_guard<std::mutex> lk(control_send_mutex_);
				const int to = control_client_.load();
				if(to < 0)
					return;
				// Never waiting: this runs where the pictures' motion is measured. The picture
				// is left for a thread of its own to send, the newest in place of one not yet
				// sent. (Sent from here it waited, MSG_DONTWAIT or not, for as long as the
				// program did not read: a program that only sent the stick stopped all
				// measuring while it was connected. 2026-11-27.)
				control_out_.resize(16 + (size_t)w * (size_t)h);
				const uint8_t header[8] = { 'P', 'L', 'K', 'F', (uint8_t)(w & 0xff), (uint8_t)(w >> 8), (uint8_t)(h & 0xff), (uint8_t)(h >> 8) };
				memcpy(control_out_.data(), header, 8);
				memcpy(control_out_.data() + 8, &picture_us, 8);
				memcpy(control_out_.data() + 16, grey, (size_t)w * (size_t)h);
				control_out_new_ = true;
				control_send_cond_.notify_one();
			});
		std::thread sender([this, client] {
			pthread_setname_np("plk.control-send");
			std::vector<uint8_t> out;
			for(;;)
			{
				{
					std::unique_lock<std::mutex> lk(control_send_mutex_);
					control_send_cond_.wait(lk, [this] { return control_out_new_ || control_client_.load() < 0; });
					if(control_client_.load() < 0)
						return;
					out.swap(control_out_);
					control_out_new_ = false;
				}
				size_t sent = 0;
				while(sent < out.size())
				{
					const ssize_t n = send(client, out.data() + sent, out.size() - sent, 0);
					if(n <= 0)
						return;
					sent += (size_t)n;
				}
			}
		});
		std::string pending;
		char buf[512];
		for(;;)
		{
			const ssize_t n = read(client, buf, sizeof(buf));
			if(n <= 0)
				break;
			pending.append(buf, (size_t)n);
			size_t end;
			while((end = pending.find('\n')) != std::string::npos)
			{
				const std::string line = pending.substr(0, end);
				pending.erase(0, end + 1);
				unsigned buttons, l2, r2;
				int lx, ly, rx, ry;
				if(line == "release")
					play_holds_.store(false);
				else if(sscanf(line.c_str(), "%u %u %u %d %d %d %d", &buttons, &l2, &r2, &lx, &ly, &rx, &ry) == 7)
				{
					ChiakiControllerState s;
					chiaki_controller_state_set_idle(&s);
					s.buttons = buttons;
					s.l2_state = (uint8_t)std::min(255u, l2);
					s.r2_state = (uint8_t)std::min(255u, r2);
					auto stick = [](int v) { return (int16_t)std::max(-32767, std::min(32767, v)); };
					s.left_x = stick(lx);
					s.left_y = stick(ly);
					s.right_x = stick(rx);
					s.right_y = stick(ry);
					{
						std::lock_guard<std::mutex> lk(pad_state_mutex_);
						play_state_ = s;
					}
					play_holds_.store(true);
				}
				sendControllerState();
			}
		}
		if(pipeline_)
			pipeline_->setFrameTap(nullptr);
		{
			std::lock_guard<std::mutex> lk(control_send_mutex_);
			control_client_.store(-1);
			control_out_new_ = false;
		}
		// The sender may be waiting in send(): shutting the socket ends that.
		shutdown(client, SHUT_RDWR);
		control_send_cond_.notify_all();
		sender.join();
		close(client);
		play_holds_.store(false);
		sendControllerState();
		PLK_LOGI("control: the program has gone");
	}
}

void Runner::stickSweepLoop()
{
	pthread_setname_np("plk.stick-sweep");
	auto wait = [this](double seconds) {
		std::unique_lock<std::mutex> lk(probe_mutex_);
		return !probe_cond_.wait_for(lk, std::chrono::duration<double>(seconds), [this] { return probe_stop_; });
	};
	for(int i = 0; i < 600; i++)
	{
		if(!wait(0.1))
			return;
		if(pipeline_ && pipeline_->counters().framesRendered > 300)
			break;
	}
	if(!wait(2.0))
		return;
	{
		std::lock_guard<std::mutex> lk(probe_mutex_);
		sweep_stick_ = fopen((options_.stick_sweep + "-stick.csv").c_str(), "w");
		sweep_motion_ = fopen((options_.stick_sweep + "-motion.csv").c_str(), "w");
		if(!sweep_stick_ || !sweep_motion_)
		{
			PLK_LOGE("stick-sweep: cannot write %s-*.csv", options_.stick_sweep.c_str());
			return;
		}
		fprintf(sweep_stick_, "t_us,x,y,aiming\n");
		fprintf(sweep_motion_, "previous_us,picture_us,dx,dy,valid,warp_x,warp_y,delay_ms,gain_x,gain_y,explained\n");
	}
	FILE *marks = fopen((options_.stick_sweep + "-profiles.csv").c_str(), "w");
	if(marks)
		fprintf(marks, "t_us,profile\n");
	// A profile: the stick's place (share of full tilt) at `t` seconds into it, and how long it is.
	struct Profile
	{
		const char *name;
		double seconds;
		bool vertical;
		std::function<double(double)> at;
		std::function<double(double)> also = nullptr;  // the other way at the same time
	};
	std::vector<Profile> profiles;
	for(double level : { 0.25, 0.5, 0.75, 1.0 })
	{
		static char names[4][24];
		const size_t n = profiles.size();
		snprintf(names[n], sizeof(names[n]), "step-%d", (int)(level * 100));
		profiles.push_back({ names[n], 0.45, false, [level](double) { return level; } });
	}
	profiles.push_back({ "ramp-300ms", 0.5, false, [](double t) { return std::min(1.0, t / 0.3); } });
	profiles.push_back({ "ramp-600ms", 0.7, false, [](double t) { return std::min(1.0, t / 0.6); } });
	profiles.push_back({ "tap-60ms", 0.06, false, [](double) { return 1.0; } });
	profiles.push_back({ "tap-120ms", 0.12, false, [](double) { return 1.0; } });
	profiles.push_back({ "reversal", 0.5, false, [](double t) { return t < 0.25 ? 1.0 : -1.0; } });
	profiles.push_back({ "sine-1.5Hz", 1.333, false, [](double t) { return 0.7 * std::sin(2.0 * M_PI * 1.5 * t); } });
	profiles.push_back({ "half-then-full", 0.6, false, [](double t) { return t < 0.3 ? 0.5 : 1.0; } });
	profiles.push_back({ "step-10", 0.6, false, [](double) { return 0.1; } });
	profiles.push_back({ "step-40-long", 1.0, false, [](double) { return 0.4; } });
	profiles.push_back({ "wiggle-3Hz", 1.0, false, [](double t) { return 0.35 * std::sin(2.0 * M_PI * 3.0 * t); } });
	profiles.push_back({ "diagonal-60", 0.4, false, [](double) { return 0.6; }, [](double) { return 0.45; } });
	profiles.push_back({ "figure-eight", 1.0, false, [](double t) { return 0.6 * std::sin(2.0 * M_PI * t); },
			[](double t) { return 0.4 * std::sin(4.0 * M_PI * t); } });
	profiles.push_back({ "vertical-50", 0.3, true, [](double) { return 0.5; } });
	profiles.push_back({ "vertical-100", 0.3, true, [](double) { return 1.0; } });
	PLK_LOGI("stick-sweep: %zu profiles, three rounds, to %s-*.csv", profiles.size(), options_.stick_sweep.c_str());
	probe_holds_.store(true);
	bool stopped = false;
	for(int round = 0; round < 3 && !stopped; round++)
	{
		for(const Profile &p : profiles)
		{
			// Once as it is and once mirrored: the camera comes back.
			for(double sign : { 1.0, -1.0 })
			{
				if(!wait(1.2))
				{
					stopped = true;
					break;
				}
				const double start_us = video::hostTimeUs();
				if(marks)
					fprintf(marks, "%.0f,%s%s\n", start_us, p.name, sign < 0 ? "-mirrored" : "");
				for(;;)
				{
					const double t = (video::hostTimeUs() - start_us) / 1e6;
					if(t >= p.seconds)
						break;
					const float v = (float)(sign * p.at(t));
					probe_stick_.store(p.vertical ? 0.0f : v);
					probe_stick_y_.store(p.vertical ? v : p.also ? (float)(sign * p.also(t)) : 0.0f);
					sendControllerState();
					// As often as a controller reports.
					if(!wait(0.004))
					{
						stopped = true;
						break;
					}
				}
				probe_stick_.store(0.0f);
				probe_stick_y_.store(0.0f);
				sendControllerState();
				if(stopped)
					break;
			}
			if(stopped)
				break;
		}
	}
	probe_holds_.store(false);
	sendControllerState();
	wait(1.0);
	{
		std::lock_guard<std::mutex> lk(probe_mutex_);
		if(sweep_stick_)
			fclose(sweep_stick_);
		if(sweep_motion_)
			fclose(sweep_motion_);
		sweep_stick_ = sweep_motion_ = nullptr;
		stopped = probe_stop_;
	}
	if(marks)
		fclose(marks);
	PLK_LOGI("stick-sweep: done");
	if(!stopped)
		requestStop();
}

AnalysisReport Runner::analysis() const
{
	AnalysisReport r;
	if(!pipeline_ || !analysis_)
		return r;
	r.timing = analysis_->snapshot(video::hostTimeUs());
	const video::Stats s = pipeline_->counters(); // asked four times a second: nothing sorted
	const ViewState v = viewState();
	r.info.stream_width = s.width;
	r.info.stream_height = s.height;
	r.info.nr_width = s.enhanceWidth;
	r.info.nr_height = s.enhanceHeight;
	r.info.picture_width = s.pictureWidth;
	r.info.picture_height = s.pictureHeight;
	r.info.target_width = s.targetWidth;
	r.info.target_height = s.targetHeight;
	r.info.bit_depth = s.bitDepth;
	r.info.hdr = s.hdr;
	r.info.codec = s.codec;
	r.info.scaler = s.scaler;
	r.info.pacing = s.pacing;
	const AnalysisSnapshot &t = r.timing;
	char buf[200];

	if(s.width > 0)
	{
		snprintf(buf, sizeof(buf), "%dx%d %s %d bit %s", s.width, s.height, s.codec.c_str(), s.bitDepth, s.hdr ? "HDR" : "SDR");
		std::string stream = buf;
		if(t.budget_ms > 0)
		{
			snprintf(buf, sizeof(buf), " at %.1f fps", 1000.0 / t.budget_ms);
			stream += buf;
		}
		r.signal.emplace_back("Stream", stream);
		// The picture's own size on the target: less than the target's beside a panel.
		const bool fitted = s.pictureWidth > 0 && s.pictureHeight > 0;
		snprintf(buf, sizeof(buf), "%dx%d, %s%s%s%s", fitted ? s.pictureWidth : s.targetWidth, fitted ? s.pictureHeight : s.targetHeight,
				s.scaler.empty() ? "-" : s.scaler.c_str(), s.pacing.empty() ? "" : ", ", s.pacing.c_str(),
				t.offscreen ? ", offscreen" : "");
		r.signal.emplace_back("Output", buf);
		if(!s.layerColorSpace.empty())
			r.signal.emplace_back("Colour", s.layerColorSpace);
	}
	else
		r.signal.emplace_back("Stream", "waiting for the first frame");

	// The enhancer's own line, in the manner of an effect's row: what, at which size, how strong.
	if(!v.nr)
		r.enhancer = "off";
	else if(v.nr_loading)
		r.enhancer = "loading the model";
	else if(!v.nr_available)
		r.enhancer = "not installed";
	else
	{
		// Of an HDR picture the network is given an SDR rendition.
		snprintf(buf, sizeof(buf), "%s model, %dx%d%s, strength %d %%, %s%s",
				v.nr_network == NrNetwork::HT ? "original (HT)" : "performance", s.enhanceWidth, s.enhanceHeight,
				s.hdr ? " SDR rendition" : "", v.nr_strength, toString(v.nr_look), v.nr_compare ? ", comparison" : "");
		r.enhancer = buf;
	}
	r.signal.emplace_back("Neural rendering", r.enhancer);
	r.signal.emplace_back("Order", v.nr && v.nr_available ? "decode > scale + mix > display; network beside"
			: "decode > scale > display");

	const bool enhancing = v.nr && v.nr_available && !v.nr_loading;
	if(t.not_presented > 0)
	{
		snprintf(buf, sizeof(buf), "%llu of the last second's frames were not shown", (unsigned long long)t.not_presented);
		r.status = buf;
		r.warning = true;
	}
	else if(!t.frames)
		r.status = "No frame in the last second";
	else if(t.budget_ms > 0 && t.processing.samples && t.processing.mean_ms > t.budget_ms)
	{
		snprintf(buf, sizeof(buf), "Over budget: a frame takes %.1f ms to process, and one arrives every %.1f ms",
				t.processing.mean_ms, t.budget_ms);
		r.status = buf;
		r.warning = true;
	}
	else if(enhancing && t.results_per_s <= 0)
	{
		r.status = t.nr_failed ? "Neural rendering fails: see the log" : "Neural rendering: no result in the last second (warming up)";
		r.warning = true;
	}
	else if(enhancing)
	{
		snprintf(buf, sizeof(buf), "Normal. Neural rendering %.0f results/s, on %.0f %% of the frames", t.results_per_s,
				t.enhanced_share * 100.0);
		r.status = buf;
	}
	else
		r.status = std::string("Normal. Neural rendering ") + r.enhancer;
	return r;
}

bool Runner::pressKey(const std::string &key, std::string *what)
{
	if(key.size() != 1)
		return false;
	const ViewState v = viewState();
	std::string done;
	switch(key[0])
	{
		case '1': setScaler(video::Scaler::Bilinear); done = "scaler bilinear"; break;
		case '2': setScaler(video::Scaler::Bicubic); done = "scaler bicubic"; break;
		case '3': setScaler(video::Scaler::Lanczos); done = "scaler lanczos"; break;
		case '4': setScaler(video::Scaler::MetalFX); done = "scaler metalfx"; break;
		case 'a': setSdrColor(video::SdrColor::Accurate); done = "colour accurate"; break;
		case 'v': setSdrColor(video::SdrColor::Vivid); done = "colour vivid"; break;
		case 'p': setHdrOutput(video::HdrOutput::PQ); done = "HDR output pq"; break;
		case 'l': setHdrOutput(video::HdrOutput::ExtendedLinear); done = "HDR output linear"; break;
		case 't':
			setHdrToneMap(v.hdr_tone_map == video::HdrToneMap::System ? video::HdrToneMap::App : video::HdrToneMap::System);
			done = "HDR tone mapping toggled";
			break;
		case '0': setHdrBrightness(kHdrBrightnessDefault); done = "HDR brightness 100 %"; break;
		case '+': setHdrBrightness(stepHdrBrightness(v.hdr_brightness, 1)); done = "HDR brighter"; break;
		case '-': setHdrBrightness(stepHdrBrightness(v.hdr_brightness, -1)); done = "HDR darker"; break;
		case 'n': setNr(!v.nr); done = v.nr ? "neural rendering off" : "neural rendering on"; break;
		case '5':
			done = setNrNetwork(NrNetwork::Small) ? "neural rendering network small" : "neural rendering network small is not installed";
			break;
		case '6':
			done = setNrNetwork(NrNetwork::HT) ? "neural rendering network ht" : "neural rendering network ht is not installed";
			break;
		case '7': setNrHeight(kNrHeights[0]); done = "neural rendering at " + heightName(kNrHeights[0]); break;
		case '8': setNrHeight(kNrHeights[1]); done = "neural rendering at " + heightName(kNrHeights[1]); break;
		case '9': setNrHeight(kNrHeights[2]); done = "neural rendering at " + heightName(kNrHeights[2]); break;
		case 'x': setNrStrength(stepNrStrength(v.nr_strength, 1)); done = "neural rendering stronger"; break;
		case 'z': setNrStrength(stepNrStrength(v.nr_strength, -1)); done = "neural rendering weaker"; break;
		case 'e': setNrLook(nextNrLook(v.nr_look)); done = std::string("neural rendering look ") + toString(nextNrLook(v.nr_look)); break;
		case 'k': setCameraWarp(!v.camera_warp); done = v.camera_warp ? "camera warp off" : "camera warp on"; break;
		case 'j': setFrameGeneration(!v.frame_generation); done = v.frame_generation ? "frame generation off" : "frame generation on"; break;
		case 'w':
		{
			const NrTiming next = v.nr_timing == NrTiming::Wait ? NrTiming::Guided
					: v.nr_timing == NrTiming::Guided ? NrTiming::Beside : NrTiming::Wait;
			setNrTiming(next);
			done = std::string("neural rendering timing ") + toString(next);
			break;
		}
		case 'c': setNrCompare(!v.nr_compare); done = v.nr_compare ? "comparison off" : "comparison on"; break;
		case 'f': setFpsOverlay(!v.fps_overlay); done = v.fps_overlay ? "frame rate overlay off" : "frame rate overlay on"; break;
		default: return false;
	}
	if(what)
		*what = done;
	return true;
}

bool Runner::start(std::function<void()> finished)
{
	finished_ = std::move(finished);
	auto finish = [this] {
		std::call_once(finished_once_, [this] {
			if(finished_)
				finished_();
		});
	};

	if(options_.command == Command::Replay)
	{
		replay_ = std::make_unique<Replay>(video_path_.get(), options_.replay_path, options_.loop);
		std::string err;
		if(!replay_->load(&err))
		{
			PLK_LOGE("Replay: %s", err.c_str());
			exit_code_ = 1;
			return false;
		}
		PLK_LOGI("Replaying %zu records from %s", replay_->records(), options_.replay_path.c_str());
		replay_->start(finish);
	}
	else
	{
		// The first-packet times of the [frame] lines come from the lib's trace hook.
		if(options_.frame_log)
		{
			installPacketTrace(&frame_logger_->spans());
			trace_installed_ = true;
		}
		if(options_.audio)
			audio_ = std::make_unique<AudioOutput>();
		// The hub calls into the session from the HID thread (controller state) and the
		// session into the hub from the Takion thread (console feedback): both exist before
		// either starts, and the session is shut down before the hub stops.
		if(options_.controller)
			controller_ = std::make_unique<ControllerHub>();
		// Every other kind of controller. Not headless: the framework needs the app's main
		// run loop.
		if(options_.controller && !options_.headless)
			gamepads_ = std::make_unique<GamepadHub>();
		session_ = std::make_unique<StreamSession>(video_path_.get(), audio_.get(), controller_.get(), gamepads_.get());
		if(controller_)
		{
			ControllerHub::Config cc;
			cc.output = options_.controller_output && !options_.headless;
			cc.realtime_threads = options_.realtime_controller;
			cc.bt_haptics = stream_config_.bt_haptics;
			cc.usb_haptics = options_.usb_haptics;
			cc.haptic_gain = stream_config_.haptic_gain;
			cc.keep_rumble_with_haptics = envSet("CHIAKI_BT_HAPTICS_KEEP_RUMBLE");
			cc.trigger_log = getenv("CHIAKI_TRIGGER_LOG") != nullptr;
			const bool ok = controller_->start(cc,
					[this](const ChiakiControllerState &state) { pushControllerState(false, state); },
					[](char level, const std::string &message) { Log::instance().write(level, message.c_str()); });
			if(!ok)
				PLK_LOGW("Continuing without the DualSense");
		}
		if(gamepads_)
		{
			GamepadHub::Config gc;
			gc.output = options_.controller_output;
			gamepads_->start(gc, [this](const ChiakiControllerState &state) { pushControllerState(true, state); },
					[](char level, const std::string &message) { Log::instance().write(level, message.c_str()); });
		}
		StreamSession::Config sc;
		sc.target = secrets_->target;
		sc.settings = stream_config_.session;
		sc.retry_window_s = options_.retry_window_s;
		if(!session_->start(sc, [this, finish](ChiakiQuitReason reason, const std::string &) {
			   if(!stop_requested_.load() && chiaki_quit_reason_is_error(reason))
				   exit_code_ = 1;
			   finish();
		   }))
		{
			exit_code_ = 1;
			return false;
		}
	}

	if(options_.stats_interval_s > 0)
		stats_thread_ = std::thread([this] { statsLoop(); });
	if(options_.seconds > 0)
	{
		const double seconds = options_.seconds;
		seconds_thread_ = std::thread([this, seconds] {
			std::unique_lock<std::mutex> lk(stats_mutex_);
			if(!stats_cond_.wait_for(lk, std::chrono::duration<double>(seconds), [this] { return stats_stop_; }))
			{
				lk.unlock();
				PLK_LOGI("--seconds %.1f reached, ending", seconds);
				requestStop();
			}
		});
	}
	return true;
}

void Runner::requestStop()
{
	stop_requested_ = true;
	if(session_)
		session_->requestStop();
	if(replay_)
		replay_->stop();
}

void Runner::shutdownWorkers()
{
	if(workers_down_)
		return;
	workers_down_ = true;
	// 1. The session: stop, join every lib thread. Video and audio stop arriving.
	if(session_)
	{
		session_->requestStop();
		session_->join();
	}
	if(replay_)
	{
		replay_->stop();
		replay_->join();
	}
	// 2. The controllers: haptics stop, effects off, devices closed.
	if(gamepads_)
		gamepads_->stop();
	if(controller_)
		controller_->stop();
	// 3. Stats and the --seconds timer.
	{
		std::lock_guard<std::mutex> lk(stats_mutex_);
		stats_stop_ = true;
	}
	stats_cond_.notify_all();
	if(stats_thread_.joinable())
		stats_thread_.join();
	if(seconds_thread_.joinable())
		seconds_thread_.join();
	if(video_path_)
		video_path_->closeDump();
}

void Runner::shutdownVideo()
{
	if(video_down_)
		return;
	video_down_ = true;
	if(pipeline_)
		logStats(true);
	// The engine loader and the overlay's thread call into the pipeline: they end first.
	{
		std::lock_guard<std::mutex> lk(nr_mutex_);
		nr_stop_ = true;
	}
	nr_cond_.notify_all();
	if(nr_thread_.joinable())
		nr_thread_.join();
	if(fg_thread_.joinable())
		fg_thread_.join();
	{
		std::lock_guard<std::mutex> lk(hud_mutex_);
		hud_stop_ = true;
	}
	hud_cond_.notify_all();
	if(hud_thread_.joinable())
		hud_thread_.join();
	// Nothing submits any more (shutdownWorkers() joined the session or the replay). The
	// pipeline goes first: its error callback may still reach session_ until it is gone.
	saveWarp();
	video_path_.reset();
	pipeline_.reset();
	// The engine after the pipeline. Each engine ends on a queue of its own, where it waits
	// for the frame it may still be working on; the run waits here until they are all gone.
	{
		std::shared_ptr<NeuralEnhancer> engine;
		{
			std::lock_guard<std::mutex> lk(nr_mutex_);
			engine = std::move(nr_engine_);
			nr_engine_ = nullptr;
		}
	}
	NeuralEnhancer::waitForRetired();
	// The gamepad hub and the session call into each other: the hub is silenced, the
	// session goes, then the hub.
	if(gamepads_)
		gamepads_->stop();
	session_.reset();
	gamepads_.reset();
	replay_.reset();
	controller_.reset();
	if(trace_installed_)
	{
		installPacketTrace(nullptr);
		trace_installed_ = false;
	}
	if(audio_)
	{
		audio_->stop();
		audio_->close();
		audio_.reset();
	}
	if(frame_logger_)
		PLK_LOGI("Pleikkari done: %llu frame lines logged", (unsigned long long)frame_logger_->logged());
	Log::instance().flush();
}

// Where what the camera warp learned of this console's game is kept between sittings.
static std::string warpFile(const std::string &nickname)
{
	const char *home = getenv("HOME");
	std::string name;
	for(char c : nickname)
		name += (isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '_';
	return std::string(home ? home : ".") + "/Library/Application Support/Pleikkari/camera-warp-" + (name.empty() ? "console" : name) + ".txt";
}

void Runner::loadWarp()
{
	if(options_.command != Command::Stream)
		return;
	if(FILE *f = fopen(warpFile(options_.nickname).c_str(), "r"))
	{
		std::string text;
		char buf[4096];
		size_t n;
		while((n = fread(buf, 1, sizeof(buf), f)) > 0)
			text.append(buf, n);
		fclose(f);
		if(camera_warp_.load(text))
			PLK_LOGI("camera warp: what was learned before is back (%s)", warpFile(options_.nickname).c_str());
	}
}

void Runner::saveWarp()
{
	if(options_.command != Command::Stream || !camera_warp_.state().learned)
		return;
	const std::string path = warpFile(options_.nickname), text = camera_warp_.save();
	if(FILE *f = fopen((path + ".new").c_str(), "w"))
	{
		const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
		if(fclose(f) == 0 && ok)
			rename((path + ".new").c_str(), path.c_str());
	}
}

void Runner::statsLoop()
{
	pthread_setname_np("plk.stats");
	std::unique_lock<std::mutex> lk(stats_mutex_);
	for(;;)
	{
		if(stats_cond_.wait_for(lk, std::chrono::duration<double>(options_.stats_interval_s), [this] { return stats_stop_; }))
			return;
		lk.unlock();
		logStats(false);
		saveWarp();
		lk.lock();
	}
}

void Runner::logStats(bool final_report)
{
	if(pipeline_)
	{
		const video::Stats s = pipeline_->stats();
		char buf[512];
		snprintf(buf, sizeof(buf),
				"video%s: %dx%d %s %s%s %s -> %dx%d scaler=%s pacing=%s layer=%s hdr_brightness=%d%% tone_map=%s headroom=%.2f submitted=%llu decoded=%llu rendered=%llu "
				"presented=%llu superseded=%llu not_presented=%llu lost=%llu errors=%llu idr=%llu awaiting_idr=%llu",
				final_report ? " (last window)" : "", s.width, s.height, s.codec.c_str(), s.pixelFormat.c_str(),
				s.hdr ? " HDR" : "", s.hardwareDecoder ? "hw" : "sw?", s.targetWidth, s.targetHeight,
				s.scaler.empty() ? "-" : s.scaler.c_str(), s.pacing.empty() ? "-" : s.pacing.c_str(),
				s.layerColorSpace.empty() ? "-" : s.layerColorSpace.c_str(),
				view_.hdr_brightness, video::toString(view_.hdr_tone_map), display_headroom_.load(),
				(unsigned long long)s.framesSubmitted, (unsigned long long)s.framesDecoded,
				(unsigned long long)s.framesRendered, (unsigned long long)s.framesPresented,
				(unsigned long long)s.framesSuperseded, (unsigned long long)s.framesNotPresented,
				(unsigned long long)s.framesLostReported, (unsigned long long)s.decodeErrors,
				(unsigned long long)((session_ ? session_->keyframeRequests() : 0) + s.idrRequests),
				(unsigned long long)s.framesSkippedAwaitingIdr);
		std::string line = buf;
		line += " ms p50/p95/p99/max:";
		line += distLine("submit_call", s.submitCallMs);
		line += distLine("decode", s.decodeMs);
		line += distLine("queue", s.queueMs);
		line += distLine("render_cpu", s.renderCpuMs);
		line += distLine("render_gpu", s.renderGpuMs);
		line += distLine("ready_to_presented", s.readyToPresentedMs);
		line += distLine("present_interval", s.presentIntervalMs);
		Log::instance().write('I', line.c_str());
		// Neural rendering: what the engine was given and what came of it. Only when it is on
		// or has done something, so that a run without it logs what it always did.
		bool nr_on, nr_compare;
		NrNetwork nr_network;
		NrLook nr_look;
		int nr_strength;
		{
			std::lock_guard<std::mutex> lk(nr_mutex_);
			nr_on = view_.nr;
			nr_network = view_.nr_network;
			nr_look = view_.nr_look;
			nr_strength = view_.nr_strength;
			nr_compare = nr_compare_;
		}
		if(nr_on || s.enhanceOffered || s.enhanceFailed || s.framesGenerated || s.generateFailed)
		{
			snprintf(buf, sizeof(buf),
					"nr%s: %s network=%s size=%dx%d strength=%d%% look=%s timing=%s compare=%s offered=%llu completed=%llu failed=%llu "
					"frames_enhanced=%llu/%llu own_result=%llu held_late=%llu skipped=%llu repeats=%llu repeat_check_us=%.1f generated=%llu (failed %llu, %.1f ms a pair)",
					final_report ? " (last window)" : "", nr_on ? "on" : "off", toString(nr_network), s.enhanceWidth,
					s.enhanceHeight, nr_strength, toString(nr_look), toString(view_.nr_timing), nr_compare ? "on" : "off", (unsigned long long)s.enhanceOffered, (unsigned long long)s.enhanceCompleted,
					(unsigned long long)s.enhanceFailed, (unsigned long long)s.framesEnhanced,
					(unsigned long long)s.framesRendered, (unsigned long long)s.framesEnhancedExact,
					(unsigned long long)s.enhanceLate, (unsigned long long)s.enhanceSkipped,
					(unsigned long long)s.framesRepeated, s.repeatChecks ? s.repeatCheckUs / (double)s.repeatChecks : 0.0,
					(unsigned long long)s.framesGenerated, (unsigned long long)s.generateFailed,
					s.generatePairs ? s.generateMsTotal / (double)s.generatePairs : 0.0);
			line = buf;
			line += " ms p50/p95/p99/max:";
			line += distLine("round_trip", s.enhanceMs);
			line += distLine("input_gpu", s.enhanceInputGpuMs);
			line += distLine("age", s.enhanceAgeMs);
			if(!s.enhancer.empty())
				line += " engine=\"" + s.enhancer + "\"";
			Log::instance().write('I', line.c_str());
		}
		if(analysis_ && !final_report)
			Log::instance().write('I', formatAnalysisLine(analysis_->snapshot(video::hostTimeUs())).c_str());
		if(!final_report)
			pipeline_->resetStats();
		if(!s.colorDescription.empty() && s.hdr)
			PLK_LOGI("video colour: %s%s%s", s.colorDescription.c_str(), s.hdrMetadata.empty() ? "" : "; ",
					s.hdrMetadata.c_str());
	}
	if(audio_)
	{
		const AudioOutputStats a = audio_->stats();
		PLK_LOGI("audio: %u ch %u Hz %s in=%llu underruns=%llu silent_frames=%llu hard_drops=%llu overflows=%llu "
				"fill_avg_ms=%.2f margin_ms=%.2f low_water_ms=%.2f drift_ppm=%.1f device_latency_ms=%.2f io_frames=%u "
				"device_changes=%llu",
				a.channels, a.rate, a.running ? "running" : "stopped", (unsigned long long)a.pcm_frames_in,
				(unsigned long long)a.playout.underruns, (unsigned long long)a.playout.silent_frames,
				(unsigned long long)a.playout.hard_drops, (unsigned long long)a.overflow_events, a.playout.avg_fill_ms,
				a.playout.margin_target_ms, a.playout.low_water_ms, a.playout.drift_ppm, a.latency.total_ms,
				a.latency.io_buffer_frames, (unsigned long long)a.device_changes);
	}
	if(controller_)
		Log::instance().write('I', controller_->statsLine().c_str());
	if(gamepads_)
		Log::instance().write('I', gamepads_->statsLine().c_str());
	if(frame_logger_)
		PLK_LOGI("frames: logged=%llu not_presented=%llu", (unsigned long long)frame_logger_->logged(),
				(unsigned long long)frame_logger_->notPresented());
	if(video_path_ && video_path_->dumpDropped())
		PLK_LOGW("video dump: %llu samples dropped", (unsigned long long)video_path_->dumpDropped());
}

} // namespace plk::app
