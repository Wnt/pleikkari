// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "SessionHost.h"

#include <chiaki/thread.h>
#include <chiaki/videoreceiver.h>

#include <pthread.h>
#include <pthread/qos.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace plk {

namespace {

// How deep the current thread is inside a library callback, and whether that callback is
// the login PIN request (raised with the session's state mutex held).
thread_local int tls_callback_depth = 0;
thread_local bool tls_in_pin_callback = false;

struct CallbackScope
{
	const bool pin;
	explicit CallbackScope(bool is_pin = false) : pin(is_pin)
	{
		tls_callback_depth++;
		if(pin)
			tls_in_pin_callback = true;
	}
	~CallbackScope()
	{
		tls_callback_depth--;
		if(pin)
			tls_in_pin_callback = false;
	}
};

std::once_flag lib_init_once;
ChiakiErrorCode lib_init_err = CHIAKI_ERR_SUCCESS;

void threadQosCallback(ChiakiThreadName name, void *)
{
	// Called by each library thread on itself as it starts (chiaki_thread_set_affinity()).
	switch(name)
	{
		case CHIAKI_THREAD_NAME_TAKION:   // receives and reassembles video/audio, runs the sinks
		case CHIAKI_THREAD_NAME_FEEDBACK: // sends controller state
			pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
			break;
		default:
			break;
	}
}

void wipe(void *p, size_t n)
{
	if(n)
		memset_s(p, n, 0, n);
}

} // namespace

ConnectTarget::~ConnectTarget()
{
	wipe(regist_key.data(), regist_key.size());
	wipe(morning.data(), morning.size());
}

ConnectTarget ConnectTarget::fromRegistration(const RegisteredConsole &console, const std::string &host)
{
	ConnectTarget t;
	t.host = host;
	t.ps5 = console.isPS5();
	t.regist_key = console.regist_key;
	t.morning = console.morning;
	return t;
}

void SessionDelegate::onLog(ChiakiLogLevel level, const char *message)
{
	char c = 'I';
	switch(level)
	{
		case CHIAKI_LOG_DEBUG: c = 'D'; break;
		case CHIAKI_LOG_VERBOSE: c = 'V'; break;
		case CHIAKI_LOG_INFO: c = 'I'; break;
		case CHIAKI_LOG_WARNING: c = 'W'; break;
		case CHIAKI_LOG_ERROR: c = 'E'; break;
	}
	fprintf(stderr, "[chiaki %c] %s\n", c, message);
}

SessionHost::SessionHost(SessionDelegate *delegate, VideoSink *video, PcmSink *audio)
	: delegate_(delegate), video_(video), audio_(audio)
{
	memset(&session_, 0, sizeof(session_));
	memset(&opus_, 0, sizeof(opus_));
	memset(&opus_sink_, 0, sizeof(opus_sink_));
	chiaki_log_init(&log_, CHIAKI_LOG_INFO | CHIAKI_LOG_WARNING | CHIAKI_LOG_ERROR, logTrampoline, this);
}

SessionHost::~SessionHost()
{
	if(tls_callback_depth)
	{
		fprintf(stderr, "plk::SessionHost destroyed from inside one of its own callbacks; that cannot be done safely\n");
		abort();
	}
	shutdown();
}

ChiakiErrorCode SessionHost::start(const ConnectTarget &target, const StreamSettings &settings)
{
	if(tls_callback_depth)
		return CHIAKI_ERR_INVALID_DATA;
	std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
	if(session_inited_)
		return CHIAKI_ERR_INVALID_DATA; // shutdown() the previous session first
	if(target.host.empty())
		return CHIAKI_ERR_PARSE_ADDR;

	std::call_once(lib_init_once, [] { lib_init_err = chiaki_lib_init(); });
	if(lib_init_err != CHIAKI_ERR_SUCCESS)
		return lib_init_err;
	if(settings.elevate_thread_qos)
		chiaki_thread_set_affinity_cb(threadQosCallback, nullptr);

	chiaki_log_set_level(&log_, settings.log_mask);
	host_ = target.host;

	ChiakiConnectInfo info;
	memset(&info, 0, sizeof(info));
	info.ps5 = target.ps5;
	info.host = host_.c_str();
	memcpy(info.regist_key, target.regist_key.data(), sizeof(info.regist_key));
	memcpy(info.morning, target.morning.data(), sizeof(info.morning));
	chiaki_connect_video_profile_preset(&info.video_profile, settings.resolution, settings.fps);
	if(settings.bitrate_kbps)
		info.video_profile.bitrate = settings.bitrate_kbps;
	info.video_profile.codec = target.ps5 ? settings.codec : CHIAKI_CODEC_H264;
	info.video_profile_auto_downgrade = settings.video_profile_auto_downgrade;
	info.enable_keyboard = false;
	info.enable_dualsense = settings.enable_dualsense;
	info.audio_video_disabled = settings.audio_video_disabled;
	info.auto_regist = false;
	info.holepunch_session = nullptr;
	info.remote_connection = nullptr;
	info.rudp_sock = nullptr;
	info.packet_loss_max = settings.packet_loss_max;
	info.adaptive_loss_report = false;
	info.enable_idr_on_fec_failure = settings.idr_on_fec_failure;
	info.disable_video_packet_reordering = settings.disable_video_packet_reordering;
	info.feedback_state_min_interval_ms = settings.feedback_state_min_interval_ms;
	info.feedback_stats_log_interval_ms = 0;
	info.stream_diagnostics_enabled = settings.stream_diagnostics;

	// chiaki_session_init() resolves the host (getaddrinfo) and cleans up after itself on failure.
	ChiakiErrorCode err = chiaki_session_init(&session_, &info, &log_);
	wipe(info.regist_key, sizeof(info.regist_key));
	wipe(info.morning, sizeof(info.morning));
	if(err != CHIAKI_ERR_SUCCESS)
	{
		memset(&session_, 0, sizeof(session_));
		return err;
	}

	chiaki_opus_decoder_init(&opus_, &log_);
	if(audio_)
	{
		chiaki_opus_decoder_set_cb(&opus_, opusSettingsCallback, opusFrameCallback, this);
		chiaki_opus_decoder_get_sink(&opus_, &opus_sink_);
		ChiakiAudioSink sink;
		sink.user = this;
		sink.header_cb = audioHeaderTrampoline;
		sink.frame_cb = audioFrameTrampoline;
		chiaki_session_set_audio_sink(&session_, &sink);
	}
	if(settings.enable_dualsense && delegate_)
	{
		ChiakiAudioSink haptics;
		haptics.user = this;
		haptics.header_cb = nullptr;
		haptics.frame_cb = hapticsTrampoline;
		chiaki_session_set_haptics_sink(&session_, &haptics);
	}
	if(video_)
		chiaki_session_set_video_sample_cb(&session_, videoTrampoline, this);
	chiaki_session_set_event_cb(&session_, eventTrampoline, this);
	ChiakiCtrlDisplaySink display;
	display.user = this;
	display.cantdisplay_cb = cantDisplayTrampoline;
	chiaki_session_ctrl_set_display_sink(&session_, &display);

	session_inited_ = true;
	{
		std::lock_guard<std::mutex> lock(state_mutex_);
		state_ = State::Connecting;
		quit_seen_ = false;
		quit_reason_ = CHIAKI_QUIT_REASON_NONE;
		quit_reason_text_.clear();
	}
	{
		std::unique_lock<std::shared_mutex> lock(api_mutex_);
		session_live_ = true;
	}

	err = chiaki_session_start(&session_);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		{
			std::unique_lock<std::shared_mutex> lock(api_mutex_);
			session_live_ = false;
		}
		chiaki_session_fini(&session_);
		chiaki_opus_decoder_fini(&opus_);
		memset(&session_, 0, sizeof(session_));
		session_inited_ = false;
		std::lock_guard<std::mutex> lock(state_mutex_);
		state_ = State::Idle;
		return err;
	}
	session_thread_started_ = true;
	return CHIAKI_ERR_SUCCESS;
}

void SessionHost::stop()
{
	std::shared_lock<std::shared_mutex> lock(api_mutex_);
	if(!session_live_)
		return;
	if(tls_in_pin_callback)
	{
		// The session thread holds its state mutex while it raises the PIN request, and
		// chiaki_session_stop() takes it: hand it to a thread that can wait for it.
		runOnHelperThread([this] { chiaki_session_stop(&session_); });
		return;
	}
	chiaki_session_stop(&session_);
}

bool SessionHost::shutdown()
{
	if(tls_callback_depth)
	{
		stop();
		return false;
	}
	std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
	if(!session_inited_)
		return true;

	chiaki_session_stop(&session_);
	if(session_thread_started_)
		chiaki_session_join(&session_); // the session thread joins ctrl, takion and the feedback sender

	std::vector<std::thread> helpers;
	{
		std::lock_guard<std::mutex> lock(helpers_mutex_);
		helpers.swap(helpers_);
	}
	for(auto &t : helpers)
		t.join();

	{
		std::unique_lock<std::shared_mutex> lock(api_mutex_);
		session_live_ = false;
	}
	chiaki_session_fini(&session_);
	chiaki_opus_decoder_fini(&opus_);
	memset(&session_, 0, sizeof(session_));
	memset(&opus_, 0, sizeof(opus_));
	memset(&opus_sink_, 0, sizeof(opus_sink_));
	session_inited_ = false;
	session_thread_started_ = false;

	std::lock_guard<std::mutex> lock(state_mutex_);
	state_ = State::Idle;
	return true;
}

bool SessionHost::waitForQuit(std::chrono::milliseconds timeout)
{
	std::unique_lock<std::mutex> lock(state_mutex_);
	return state_cond_.wait_for(lock, timeout, [this] { return quit_seen_; });
}

bool SessionHost::setControllerState(const ChiakiControllerState &state)
{
	std::shared_lock<std::shared_mutex> lock(api_mutex_);
	if(!session_live_)
		return false;
	// The library copies *state under its feedback sender mutex and never writes to it.
	return chiaki_session_set_controller_state(&session_, const_cast<ChiakiControllerState *>(&state)) == CHIAKI_ERR_SUCCESS;
}

bool SessionHost::requestKeyframe(bool discard_until_keyframe)
{
	std::shared_lock<std::shared_mutex> lock(api_mutex_);
	if(!session_live_)
		return false;
	// chiaki_session_request_idr() sends on the takion socket, whose mutexes exist only
	// between chiaki_takion_connect() and chiaki_takion_close(). feedback_sender_active is
	// true, under feedback_sender_mutex, strictly inside that window (and video_receiver
	// is alive in it): streamconnection.c sets it after streaminfo and clears it before
	// closing takion. chiaki_session_set_controller_state() relies on the same pair.
	ChiakiStreamConnection *sc = &session_.stream_connection;
	if(chiaki_mutex_lock(&sc->feedback_sender_mutex) != CHIAKI_ERR_SUCCESS)
		return false;
	bool sent = false;
	if(sc->feedback_sender_active)
	{
		sent = chiaki_session_request_idr(&session_) == CHIAKI_ERR_SUCCESS;
		if(sent && discard_until_keyframe && sc->video_receiver)
			chiaki_video_receiver_set_waiting_for_idr(sc->video_receiver, true);
	}
	chiaki_mutex_unlock(&sc->feedback_sender_mutex);
	return sent;
}

bool SessionHost::setLoginPin(const std::string &pin)
{
	std::shared_lock<std::shared_mutex> lock(api_mutex_);
	if(!session_live_)
		return false;
	if(tls_in_pin_callback)
	{
		runOnHelperThread([this, pin] {
			chiaki_session_set_login_pin(&session_, reinterpret_cast<const uint8_t *>(pin.data()), pin.size());
		});
		return true;
	}
	return chiaki_session_set_login_pin(&session_, reinterpret_cast<const uint8_t *>(pin.data()), pin.size()) == CHIAKI_ERR_SUCCESS;
}

void SessionHost::runOnHelperThread(std::function<void()> fn)
{
	std::lock_guard<std::mutex> lock(helpers_mutex_);
	helpers_.emplace_back(std::move(fn));
}

SessionHost::State SessionHost::state() const
{
	std::lock_guard<std::mutex> lock(state_mutex_);
	return state_;
}

ChiakiQuitReason SessionHost::quitReason() const
{
	std::lock_guard<std::mutex> lock(state_mutex_);
	return quit_reason_;
}

std::string SessionHost::quitReasonText() const
{
	std::lock_guard<std::mutex> lock(state_mutex_);
	return quit_reason_text_;
}

// ---- library callbacks ----

void SessionHost::eventTrampoline(ChiakiEvent *event, void *user)
{
	CallbackScope scope(event->type == CHIAKI_EVENT_LOGIN_PIN_REQUEST);
	static_cast<SessionHost *>(user)->handleEvent(*event);
}

void SessionHost::handleEvent(const ChiakiEvent &e)
{
	SessionDelegate *d = delegate_;
	switch(e.type)
	{
		case CHIAKI_EVENT_CONNECTED:
		{
			std::lock_guard<std::mutex> lock(state_mutex_);
			state_ = State::Streaming;
		}
			if(d)
				d->onConnected(e.connected);
			break;
		case CHIAKI_EVENT_QUIT:
			if(d)
				d->onQuit(e.quit.reason, e.quit.reason_str);
			{
				std::lock_guard<std::mutex> lock(state_mutex_);
				state_ = State::Quit;
				quit_seen_ = true;
				quit_reason_ = e.quit.reason;
				quit_reason_text_ = e.quit.reason_str ? e.quit.reason_str : "";
			}
			state_cond_.notify_all();
			break;
		case CHIAKI_EVENT_LOGIN_PIN_REQUEST:
			if(d)
				d->onLoginPinRequest(e.login_pin_request.pin_incorrect);
			break;
		case CHIAKI_EVENT_STREAM_STATS:
			if(d)
				d->onStreamStats(e.stream_stats);
			break;
		case CHIAKI_EVENT_RUMBLE:
			if(d)
				d->onRumble(e.rumble);
			break;
		case CHIAKI_EVENT_TRIGGER_EFFECTS:
			if(d)
				d->onTriggerEffects(e.trigger_effects);
			break;
		case CHIAKI_EVENT_HAPTIC_INTENSITY:
			if(d)
				d->onHapticIntensity(e.intensity);
			break;
		case CHIAKI_EVENT_TRIGGER_INTENSITY:
			if(d)
				d->onTriggerIntensity(e.intensity);
			break;
		case CHIAKI_EVENT_LED_COLOR:
			if(d)
				d->onLedColor(e.led_state);
			break;
		case CHIAKI_EVENT_PLAYER_INDEX:
			if(d)
				d->onPlayerIndex(e.player_index);
			break;
		case CHIAKI_EVENT_MOTION_RESET:
			if(d)
				d->onMotionReset();
			break;
		case CHIAKI_EVENT_VIDEO_FEC_FAILURE:
			if(d)
				d->onVideoFecFailure(e.video_fec_failure);
			break;
		default:
			if(d)
				d->onOtherEvent(e);
			break;
	}
}

bool SessionHost::videoTrampoline(uint8_t *buf, size_t buf_size, ChiakiSeqNum16 frame_index, uint64_t frame_ready_time_us,
		int32_t frames_lost, bool frame_recovered, void *user)
{
	CallbackScope scope;
	return static_cast<SessionHost *>(user)->video_->onVideoSample(buf, buf_size, frame_index, frame_ready_time_us,
			frames_lost, frame_recovered);
}

void SessionHost::audioHeaderTrampoline(ChiakiAudioHeader *header, void *user)
{
	CallbackScope scope;
	SessionHost *self = static_cast<SessionHost *>(user);
	self->opus_sink_.header_cb(header, self->opus_sink_.user); // -> opusSettingsCallback
}

void SessionHost::audioFrameTrampoline(uint8_t *buf, size_t buf_size, void *user)
{
	CallbackScope scope;
	SessionHost *self = static_cast<SessionHost *>(user);
	self->opus_sink_.frame_cb(buf, buf_size, self->opus_sink_.user); // -> opusFrameCallback
}

void SessionHost::opusSettingsCallback(uint32_t channels, uint32_t rate, void *user)
{
	static_cast<SessionHost *>(user)->audio_->onAudioFormat(channels, rate);
}

void SessionHost::opusFrameCallback(int16_t *buf, size_t frames, void *user)
{
	// opus_decode() returns samples per channel, i.e. frames.
	static_cast<SessionHost *>(user)->audio_->onPcm(buf, frames);
}

void SessionHost::hapticsTrampoline(uint8_t *buf, size_t buf_size, void *user)
{
	CallbackScope scope;
	static_cast<SessionHost *>(user)->delegate_->onHapticsFrame(buf, buf_size);
}

void SessionHost::cantDisplayTrampoline(void *user, bool cant_display)
{
	CallbackScope scope;
	SessionHost *self = static_cast<SessionHost *>(user);
	if(self->delegate_)
		self->delegate_->onCantDisplay(cant_display);
}

void SessionHost::logTrampoline(ChiakiLogLevel level, const char *msg, void *user)
{
	CallbackScope scope;
	SessionHost *self = static_cast<SessionHost *>(user);
	if(self->delegate_)
		self->delegate_->onLog(level, msg);
	else
		SessionDelegate().onLog(level, msg);
}

} // namespace plk
