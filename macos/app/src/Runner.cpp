// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "Runner.h"

#include "AudioOutput.h"
#include "Controller.h"
#include "FrameTelemetry.h"
#include "Log.h"
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

Runner::Runner(const Options &options) : options_(options), secrets_(std::make_unique<RunnerSecrets>()) {}

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
	pc.pacing = options_.pacing;
	pc.scaler = options_.scaler;
	pc.hdrOutput = options_.hdr_linear ? video::HdrOutput::ExtendedLinear : video::HdrOutput::PQ;
	pc.displayRefreshHz = display_refresh_hz > 0 ? display_refresh_hz : 120.0f;
	if(options_.headless)
	{
		pc.offscreenWidth = options_.offscreen_width;
		pc.offscreenHeight = options_.offscreen_height;
	}
	FrameLogger *logger = frame_logger_.get();
	pc.onFrame = [logger](const video::FrameTiming &t) { logger->onFrame(t); };

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
	PLK_LOGI("Video: pacing %s, scaler %s, HDR output %s, display %.0f Hz%s", video::toString(options_.pacing),
			video::toString(options_.scaler), options_.hdr_linear ? "extended linear" : "PQ", pc.displayRefreshHz,
			options_.headless ? ", headless (offscreen)" : "");
	video_path_ = std::make_unique<VideoPath>(pipeline_.get());
	if(!options_.dump_path.empty())
	{
		if(!video_path_->openDump(options_.dump_path, &err))
			PLK_LOGW("Video dump: %s", err.c_str());
		else
			PLK_LOGI("Recording the video samples to %s", options_.dump_path.c_str());
	}
	return true;
}

void Runner::setDrawableSize(int width, int height)
{
	if(pipeline_ && width > 0 && height > 0)
		pipeline_->setDrawableSize(width, height);
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
		session_ = std::make_unique<StreamSession>(video_path_.get(), audio_.get(), controller_.get());
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
			StreamSession *session = session_.get();
			const bool ok = controller_->start(cc,
					[session](const ChiakiControllerState &state) { session->setControllerState(state); },
					[](char level, const std::string &message) { Log::instance().write(level, message.c_str()); });
			if(!ok)
				PLK_LOGW("Continuing without the DualSense");
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
	// Nothing submits any more (shutdownWorkers() joined the session or the replay). The
	// pipeline goes first: its error callback may still reach session_ until it is gone.
	video_path_.reset();
	pipeline_.reset();
	session_.reset();
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
				"video%s: %dx%d %s %s%s %s -> %dx%d submitted=%llu decoded=%llu rendered=%llu presented=%llu "
				"superseded=%llu not_presented=%llu lost=%llu errors=%llu idr=%llu awaiting_idr=%llu",
				final_report ? " (last window)" : "", s.width, s.height, s.codec.c_str(), s.pixelFormat.c_str(),
				s.hdr ? " HDR" : "", s.hardwareDecoder ? "hw" : "sw?", s.targetWidth, s.targetHeight,
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
	if(frame_logger_)
		PLK_LOGI("frames: logged=%llu not_presented=%llu", (unsigned long long)frame_logger_->logged(),
				(unsigned long long)frame_logger_->notPresented());
	if(video_path_ && video_path_->dumpDropped())
		PLK_LOGW("video dump: %llu samples dropped", (unsigned long long)video_path_->dumpDropped());
}

} // namespace plk::app
