// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "StreamSession.h"

#include "Controller.h"
#include "Log.h"
#include "VideoPath.h"

#include <pthread.h>

#include <chrono>
#include <cinttypes>

namespace plk::app {

namespace {

double steadySeconds()
{
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

StreamSession::StreamSession(VideoPath *video, PcmSink *audio, ControllerHub *controller)
	: controller_(controller), host_(this, video, audio)
{
	dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
	idr_queue_ = dispatch_queue_create("plk.idr", attr);
}

StreamSession::~StreamSession()
{
	requestStop();
	join();
	host_.shutdown();
	// Let a queued IDR request finish before host_ goes away.
	dispatch_sync(idr_queue_, ^{});
	dispatch_release(idr_queue_);
}

bool StreamSession::start(const Config &config, FinishedFn finished)
{
	if(running_.load())
		return false;
	config_ = config;
	finished_ = std::move(finished);
	stop_requested_ = false;
	running_ = true;
	thread_ = std::thread([this] { run(); });
	return true;
}

void StreamSession::requestStop()
{
	stop_requested_ = true;
	host_.stop();
	wait_cond_.notify_all();
}

bool StreamSession::restart(const StreamSettings &settings)
{
	if(!running_.load() || stop_requested_.load())
		return false;
	{
		std::lock_guard<std::mutex> lk(restart_mutex_);
		restart_settings_ = settings;
	}
	restart_requested_ = true;
	host_.stop();
	wait_cond_.notify_all();
	return true;
}

void StreamSession::join()
{
	if(thread_.joinable() && thread_.get_id() != std::this_thread::get_id())
		thread_.join();
}

void StreamSession::run()
{
	pthread_setname_np("plk.session");
	double first_start = steadySeconds();
	ChiakiQuitReason reason = CHIAKI_QUIT_REASON_STOPPED;
	std::string text;
	int attempt = 0;
	// A restart() that is waiting: take its settings and begin again as a new stream.
	auto take_restart = [&]() {
		if(stop_requested_.load() || !restart_requested_.exchange(false))
			return false;
		{
			std::lock_guard<std::mutex> lk(restart_mutex_);
			if(restart_settings_)
				config_.settings = *restart_settings_;
			restart_settings_.reset();
		}
		first_start = steadySeconds();
		ever_connected_ = false;
		attempt = 0;
		restarts_.fetch_add(1, std::memory_order_relaxed);
		if(controller_)
			controller_->setSessionActive(false);
		PLK_LOGI("Session restarting with codec %s", chiaki_codec_name(config_.settings.codec));
		return true;
	};
	for(;;)
	{
		attempt++;
		connected_ = false;
		const ChiakiErrorCode err = host_.start(config_.target, config_.settings);
		if(err != CHIAKI_ERR_SUCCESS)
		{
			PLK_LOGE("Session could not start: %s", chiaki_error_string(err));
			reason = CHIAKI_QUIT_REASON_SESSION_REQUEST_UNKNOWN;
			text = chiaki_error_string(err);
			break;
		}
		if(attempt > 1)
			PLK_LOGI("Session attempt %d started", attempt);
		// stop() before the session existed did nothing: repeat it now that it does.
		if(stop_requested_.load() || restart_requested_.load())
			host_.stop();
		while(!host_.waitForQuit(std::chrono::milliseconds(200)))
		{
			if(stop_requested_.load() || restart_requested_.load())
				host_.stop();
		}
		reason = host_.quitReason();
		text = host_.quitReasonText();
		host_.shutdown();
		connected_ = false;
		if(take_restart())
			continue;

		const bool retry = !stop_requested_.load() && !ever_connected_.load() && chiaki_quit_reason_is_error(reason)
				&& steadySeconds() - first_start + config_.retry_delay_s < config_.retry_window_s;
		if(!retry)
			break;
		PLK_LOGW("Session ended before it connected (%s), retrying in %.0f s", chiaki_quit_reason_string(reason),
				config_.retry_delay_s);
		std::unique_lock<std::mutex> lk(wait_mutex_);
		wait_cond_.wait_for(lk, std::chrono::duration<double>(config_.retry_delay_s),
				[this] { return stop_requested_.load() || restart_requested_.load(); });
		lk.unlock();
		if(stop_requested_.load())
			break;
		take_restart();
	}
	if(controller_)
		controller_->setSessionActive(false);
	PLK_LOGI("Session finished: %s%s%s", chiaki_quit_reason_string(reason), text.empty() ? "" : ", ", text.c_str());
	running_ = false;
	if(finished_)
		finished_(reason, text);
}

void StreamSession::setControllerState(const ChiakiControllerState &state)
{
	host_.setControllerState(state);
}

void StreamSession::scheduleKeyframeRequest(const char *why)
{
	if(idr_pending_.exchange(true))
		return;
	const char *reason = why ? why : "?";
	dispatch_async(idr_queue_, ^{
		idr_pending_ = false;
		const bool sent = host_.requestKeyframe(true);
		idr_requests_.fetch_add(1, std::memory_order_relaxed);
		PLK_LOGW("IDR requested (%s)%s", reason, sent ? "" : ": not sent, stream not up");
	});
}

void StreamSession::onConnected(const ChiakiConnectedEvent &info)
{
	connected_ = true;
	ever_connected_ = true;
	PLK_LOGI("Session connected%s to %s:%u, mtu %u, rtt %.2f ms%s", info.relay ? " over the PSN relay" : "",
			info.peer_host[0] ? info.peer_host : "?", (unsigned)info.peer_port, (unsigned)info.mtu_in,
			info.rtt_us / 1000.0, info.measured ? "" : " (fallback, not measured)");
	if(controller_)
		controller_->setSessionActive(true);
}

void StreamSession::onQuit(ChiakiQuitReason reason, const char *reason_str)
{
	connected_ = false;
	PLK_LOGI("Session quit: %s%s%s", chiaki_quit_reason_string(reason), reason_str ? ": " : "",
			reason_str ? reason_str : "");
	wait_cond_.notify_all();
}

void StreamSession::onLoginPinRequest(bool pin_incorrect)
{
	PLK_LOGE("The console asks for its login PIN%s. This app has no PIN entry: start the stream once "
			"from the Qt app, or turn the PIN off on the console. Ending the session.",
			pin_incorrect ? " (the previous one was wrong)" : "");
	stop_requested_ = true;
	host_.stop();
}

void StreamSession::onStreamStats(const ChiakiStreamStatsEvent &s)
{
	PLK_LOGI("stream: frames=%" PRIu64 " lost=%" PRIu64 " jitter_ms=%.2f takion_pkts=%" PRIu64 " units_missing=%" PRIu64
			" fec_recovered=%" PRIu64 " unrecoverable=%" PRIu64 " reorder_timeouts=%" PRIu64 " feedback=%" PRIu64
			" feedback_gap_max_ms=%" PRIu64 " target_kbps=%" PRIu64 " measured_kbps=%" PRIu64,
			s.stream_frames, s.video_frames_lost, s.video_packet_jitter_us / 1000.0, s.takion_packets_received,
			s.takion_partial_frame_units_missing, s.fec_recovered_packets, s.unrecoverable_packets,
			s.video_reorder_timeouts, s.feedback_packets, s.feedback_gap_max_ms, s.target_bitrate_bps / 1000,
			s.measured_throughput_bps / 1000);
}

void StreamSession::onRumble(const ChiakiRumbleEvent &rumble)
{
	if(controller_)
		controller_->onRumble(rumble.left, rumble.right);
}

void StreamSession::onTriggerEffects(const ChiakiTriggerEffectsEvent &effects)
{
	if(controller_)
		controller_->onTriggerEffects(effects);
}

void StreamSession::onHapticIntensity(ChiakiDualSenseEffectIntensity intensity)
{
	if(controller_)
		controller_->onHapticIntensity(intensity);
}

void StreamSession::onTriggerIntensity(ChiakiDualSenseEffectIntensity intensity)
{
	if(controller_)
		controller_->onTriggerIntensity(intensity);
}

void StreamSession::onLedColor(const uint8_t rgb[3])
{
	if(controller_)
		controller_->onLedColor(rgb);
}

void StreamSession::onPlayerIndex(uint8_t index)
{
	if(controller_)
		controller_->onPlayerIndex(index);
}

void StreamSession::onMotionReset()
{
	if(controller_)
		controller_->onMotionReset();
}

void StreamSession::onHapticsFrame(const uint8_t *buf, size_t size)
{
	if(controller_)
		controller_->onHapticsFrame(buf, size);
}

void StreamSession::onVideoFecFailure(const ChiakiVideoFecFailureEvent &failure)
{
	if(failure.idr_request_sent)
		PLK_LOGW("Video FEC failure (frame %d), waiting for requested IDR", (int)failure.frame_index);
	else
	{
		PLK_LOGW("Video FEC failure (frame %d), retrying IDR request after send failure", (int)failure.frame_index);
		scheduleKeyframeRequest("FEC failure");
	}
}

void StreamSession::onCantDisplay(bool cant_display)
{
	PLK_LOGW("The console %s", cant_display ? "cannot show this content over Remote Play" : "can show the picture again");
}

void StreamSession::onOtherEvent(const ChiakiEvent &event)
{
	PLK_LOGD("Session event %d", (int)event.type);
}

void StreamSession::onLog(ChiakiLogLevel level, const char *message)
{
	Log::instance().write(chiaki_log_level_char(level), message);
}

} // namespace plk::app
