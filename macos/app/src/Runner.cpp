// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "Runner.h"

#include "AudioOutput.h"
#include "Controller.h"
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
	ChiakiControllerState merged;
	{
		std::lock_guard<std::mutex> lk(pad_state_mutex_);
		(from_gamepads ? gamepad_state_ : dualsense_state_) = state;
		// The DualSense first: it is the one with motion.
		chiaki_controller_state_or(&merged, &dualsense_state_, &gamepad_state_);
	}
	if(session_)
		session_->setControllerState(merged);
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
	}
	analysis_ = std::make_unique<FrameAnalysis>();
	FrameLogger *logger = frame_logger_.get();
	FrameAnalysis *analysis = analysis_.get();
	pc.onFrame = [logger, analysis](const video::FrameTiming &t) {
		logger->onFrame(t);
		analysis->onFrame(t);
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
	// Neural rendering: the engine is loaded beside the stream, which starts without it.
	nr_thread_ = std::thread([this] { nrLoop(); });
	applyEnhanceConfig();
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
	c.compareSplit = nr_compare_ ? 0.5f : 0.0f;
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

AnalysisReport Runner::analysis() const
{
	AnalysisReport r;
	if(!pipeline_ || !analysis_)
		return r;
	r.timing = analysis_->snapshot(video::hostTimeUs());
	const video::Stats s = pipeline_->counters(); // asked four times a second: nothing sorted
	const ViewState v = viewState();
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
		snprintf(buf, sizeof(buf), "%dx%d, %s%s%s%s", s.targetWidth, s.targetHeight, s.scaler.empty() ? "-" : s.scaler.c_str(),
				s.pacing.empty() ? "" : ", ", s.pacing.c_str(), t.offscreen ? ", offscreen" : "");
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
		case 'c': setNrCompare(!v.nr_compare); done = v.nr_compare ? "comparison off" : "comparison on"; break;
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
	// The engine loader calls into the pipeline: it ends first.
	{
		std::lock_guard<std::mutex> lk(nr_mutex_);
		nr_stop_ = true;
	}
	nr_cond_.notify_all();
	if(nr_thread_.joinable())
		nr_thread_.join();
	// Nothing submits any more (shutdownWorkers() joined the session or the replay). The
	// pipeline goes first: its error callback may still reach session_ until it is gone.
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
		if(nr_on || s.enhanceOffered || s.enhanceFailed)
		{
			snprintf(buf, sizeof(buf),
					"nr%s: %s network=%s size=%dx%d strength=%d%% look=%s compare=%s offered=%llu completed=%llu failed=%llu "
					"frames_enhanced=%llu/%llu",
					final_report ? " (last window)" : "", nr_on ? "on" : "off", toString(nr_network), s.enhanceWidth,
					s.enhanceHeight, nr_strength, toString(nr_look), nr_compare ? "on" : "off",
					(unsigned long long)s.enhanceOffered, (unsigned long long)s.enhanceCompleted,
					(unsigned long long)s.enhanceFailed, (unsigned long long)s.framesEnhanced,
					(unsigned long long)s.framesRendered);
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
