// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// SessionHost: one chiaki session to a registered console on the LAN (or any directly
// reachable address), without Qt or SDL in the way.
//
//   video    library -> VideoSink::onVideoSample()           as delivered, no queue, no copy
//   audio    library -> Opus decoder -> PcmSink::onPcm()     on the library's thread
//   input    setControllerState() -> chiaki_session_set_controller_state(), no timer
//   events   library -> SessionDelegate::on...()             on the thread that raised them
//
// Threads the callbacks arrive on (named by the library):
//   "Chiaki Session": onConnected, onQuit, onLoginPinRequest, onStreamStats
//   "Chiaki Takion":  VideoSink, PcmSink, onHapticsFrame, onRumble, onTriggerEffects,
//                     onHapticIntensity, onTriggerIntensity, onLedColor, onPlayerIndex,
//                     onMotionReset, onVideoFecFailure
//   "Chiaki Ctrl":    onCantDisplay, keyboard events (onOtherEvent)
//   any thread:       onLog
//
// Rules for callbacks: they may call stop(), setControllerState(), requestKeyframe() and
// setLoginPin() (from onLoginPinRequest these are handed to a helper thread, because the
// library holds its session lock while it raises that event). They must not call
// shutdown() or destroy the SessionHost: that joins the very threads they run on.
//
// Not here on purpose: PSN/holepunch connections, registration, wake-up, discovery.

#ifndef PLK_HOST_SESSIONHOST_H
#define PLK_HOST_SESSIONHOST_H

#include "Registration.h"
#include "Sinks.h"

#include <chiaki/log.h>
#include <chiaki/opusdecoder.h>
#include <chiaki/session.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

namespace plk {

struct StreamSettings
{
	ChiakiVideoResolutionPreset resolution = CHIAKI_VIDEO_RESOLUTION_PRESET_1080p;
	ChiakiVideoFPSPreset fps = CHIAKI_VIDEO_FPS_PRESET_60;
	unsigned bitrate_kbps = 0;                     // 0 = the preset's (1080p: 15000)
	ChiakiCodec codec = CHIAKI_CODEC_H265;         // PS5: H264, H265 or H265_HDR; a PS4 always gets H264
	bool enable_dualsense = true;                  // DualSense features: haptics stream, trigger effects
	bool video_profile_auto_downgrade = true;      // as the Qt app
	bool idr_on_fec_failure = false;               // library requests an IDR itself on FEC failure (Qt default: off)
	bool disable_video_packet_reordering = false;  // false: a missing head packet is waited for up to 16 ms
	double packet_loss_max = 0.05;                 // as the Qt app
	uint32_t feedback_state_min_interval_ms = 0;   // stick/motion packet spacing; 0 = library default (8 ms)
	bool stream_diagnostics = false;               // onStreamStats() once per second (adds per-packet bookkeeping)
	ChiakiDisableAudioVideo audio_video_disabled = CHIAKI_NONE_DISABLED;
	// Process-wide: the library's receive ("Chiaki Takion") and input ("Chiaki Feedback
	// Sender") threads run at QOS_CLASS_USER_INTERACTIVE, so they stay on performance
	// cores and wake promptly. Installed by the first start() that asks for it.
	bool elevate_thread_qos = true;
	uint32_t log_mask = CHIAKI_LOG_INFO | CHIAKI_LOG_WARNING | CHIAKI_LOG_ERROR; // VERBOSE dumps request headers
};

/** Where to connect and with which registration. Holds secrets; wiped on destruction. */
struct ConnectTarget
{
	std::string host;                                    // IPv4/IPv6 literal or host name; ports 9295-9297
	bool ps5 = true;
	std::array<char, CHIAKI_SESSION_AUTH_SIZE> regist_key{};
	std::array<uint8_t, 0x10> morning{};

	ConnectTarget() = default;
	ConnectTarget(const ConnectTarget &) = default;
	ConnectTarget &operator=(const ConnectTarget &) = default;
	~ConnectTarget();

	static ConnectTarget fromRegistration(const RegisteredConsole &console, const std::string &host);
};

/**
 * Everything the session reports besides video and audio, forwarded as the library
 * produced it. Every method has an empty default; onLog() prints to stderr.
 */
class SessionDelegate
{
public:
	virtual ~SessionDelegate() = default;

	// "Chiaki Session" thread
	virtual void onConnected(const ChiakiConnectedEvent & /*info*/) {}
	/** reason_str: the console's text, or nullptr; valid during the call only. */
	virtual void onQuit(ChiakiQuitReason /*reason*/, const char * /*reason_str*/) {}
	/** Answer with SessionHost::setLoginPin() (from here or any thread) or stop(). */
	virtual void onLoginPinRequest(bool /*pin_incorrect*/) {}
	/** Only with StreamSettings::stream_diagnostics. */
	virtual void onStreamStats(const ChiakiStreamStatsEvent & /*stats*/) {}

	// "Chiaki Takion" thread
	virtual void onRumble(const ChiakiRumbleEvent & /*rumble*/) {}
	virtual void onTriggerEffects(const ChiakiTriggerEffectsEvent & /*effects*/) {}
	virtual void onHapticIntensity(ChiakiDualSenseEffectIntensity /*intensity*/) {}
	virtual void onTriggerIntensity(ChiakiDualSenseEffectIntensity /*intensity*/) {}
	virtual void onLedColor(const uint8_t /*rgb*/[3]) {}
	virtual void onPlayerIndex(uint8_t /*index*/) {}
	virtual void onMotionReset() {}
	/**
	 * The console's DualSense haptics stream (only with enable_dualsense): raw PCM,
	 * signed 16-bit little-endian, 2 channels at 3000 Hz, normally 120 bytes (10 ms) per call.
	 */
	virtual void onHapticsFrame(const uint8_t * /*buf*/, size_t /*size*/) {}
	virtual void onVideoFecFailure(const ChiakiVideoFecFailureEvent & /*failure*/) {}

	// "Chiaki Ctrl" thread
	/** The console says the picture cannot be streamed now (protected content), or can again. */
	virtual void onCantDisplay(bool /*cant_display*/) {}

	/** Event types not listed above (keyboard; PSN-only types a LAN session does not raise). */
	virtual void onOtherEvent(const ChiakiEvent & /*event*/) {}

	// any thread
	virtual void onLog(ChiakiLogLevel level, const char *message);
};

class SessionHost
{
public:
	enum class State
	{
		Idle,       // not started, or shut down
		Connecting, // started, CHIAKI_EVENT_CONNECTED not seen yet
		Streaming,  // connected
		Quit        // the session thread reported CHIAKI_EVENT_QUIT; call shutdown()
	};

	/**
	 * The sinks and the delegate are not owned and must outlive the session; any of them
	 * may be null. A null VideoSink or PcmSink means that data is dropped by the library.
	 */
	SessionHost(SessionDelegate *delegate, VideoSink *video, PcmSink *audio);
	~SessionHost();

	SessionHost(const SessionHost &) = delete;
	SessionHost &operator=(const SessionHost &) = delete;

	/** Initialise and start the session thread. Not from a callback. */
	ChiakiErrorCode start(const ConnectTarget &target, const StreamSettings &settings);

	/** Ask the session to end. Non-blocking, idempotent, any thread (callbacks included). */
	void stop();

	/**
	 * stop(), join every library thread, free the session. Safe in every state, including
	 * a session that never connected or never started. Blocking; not from a callback
	 * (there it only calls stop() and returns false).
	 */
	bool shutdown();

	/** Wait until CHIAKI_EVENT_QUIT has been delivered (true) or the timeout passed. */
	bool waitForQuit(std::chrono::milliseconds timeout);

	/**
	 * Any thread, any rate. Straight into chiaki_session_set_controller_state(): buttons,
	 * triggers and touches are queued for the feedback sender at once; sticks and motion are
	 * sent at most every feedback_state_min_interval_ms (8 ms). Before the stream is up the
	 * state is kept and sent when it comes up. false if no session exists.
	 */
	bool setControllerState(const ChiakiControllerState &state);

	/**
	 * Any thread (also from VideoSink::onVideoSample): send the console an IDR request.
	 * With discard_until_keyframe the library also drops P-frames until the IDR arrives,
	 * as the Qt app does. false if the stream is not up (its first frame is an IDR anyway)
	 * or the send failed.
	 */
	bool requestKeyframe(bool discard_until_keyframe = true);

	/** Answer CHIAKI_EVENT_LOGIN_PIN_REQUEST. */
	bool setLoginPin(const std::string &pin);

	State state() const;
	ChiakiQuitReason quitReason() const;
	std::string quitReasonText() const;

private:
	static void eventTrampoline(ChiakiEvent *event, void *user);
	static bool videoTrampoline(uint8_t *buf, size_t buf_size, ChiakiSeqNum16 frame_index, uint64_t frame_ready_time_us,
			int32_t frames_lost, bool frame_recovered, void *user);
	static void audioHeaderTrampoline(ChiakiAudioHeader *header, void *user);
	static void audioFrameTrampoline(uint8_t *buf, size_t buf_size, void *user);
	static void opusSettingsCallback(uint32_t channels, uint32_t rate, void *user);
	static void opusFrameCallback(int16_t *buf, size_t frames, void *user);
	static void hapticsTrampoline(uint8_t *buf, size_t buf_size, void *user);
	static void cantDisplayTrampoline(void *user, bool cant_display);
	static void logTrampoline(ChiakiLogLevel level, const char *msg, void *user);

	void handleEvent(const ChiakiEvent &event);
	void runOnHelperThread(std::function<void()> fn);

	SessionDelegate *const delegate_;
	VideoSink *const video_;
	PcmSink *const audio_;

	ChiakiSession session_;
	ChiakiLog log_;
	ChiakiOpusDecoder opus_;
	ChiakiAudioSink opus_sink_; // the decoder's own sink, called from audio*Trampoline
	std::string host_;

	std::mutex lifecycle_mutex_;        // start() / shutdown()
	bool session_inited_ = false;
	bool session_thread_started_ = false;

	// Shared by the per-call API (controller, keyframe, PIN, stop); exclusive only around
	// chiaki_session_fini(), so a call never touches a freed session.
	mutable std::shared_mutex api_mutex_;
	bool session_live_ = false;

	mutable std::mutex state_mutex_;
	std::condition_variable state_cond_;
	State state_ = State::Idle;
	bool quit_seen_ = false;
	ChiakiQuitReason quit_reason_ = CHIAKI_QUIT_REASON_NONE;
	std::string quit_reason_text_;

	std::mutex helpers_mutex_;
	std::vector<std::thread> helpers_;
};

} // namespace plk

#endif // PLK_HOST_SESSIONHOST_H
