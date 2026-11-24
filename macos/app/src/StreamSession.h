// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// One Remote Play session from start to quit, on its own lifecycle thread:
//
//   start -> SessionHost::start -> wait for CHIAKI_EVENT_QUIT -> SessionHost::shutdown
//
// A session that ends with an error before it ever connected is started again for up to
// 20 s, as the Qt app does (a console still holding the previous session answers
// "Remote Play is already in use" for a while). Whatever ends the session, finished()
// is called once on the lifecycle thread after shutdown, with the quit reason.
//
// The sinks: video to VideoPath (pipeline on the Takion thread), audio to AudioOutput,
// console feedback to ControllerHub, the lib's log lines to the app log.

#ifndef PLK_APP_STREAMSESSION_H
#define PLK_APP_STREAMSESSION_H

#include "SessionHost.h"

#include <atomic>
#include <condition_variable>
#include <dispatch/dispatch.h>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace plk::app {

class ControllerHub;
class VideoPath;

class StreamSession : public SessionDelegate
{
public:
	struct Config
	{
		ConnectTarget target;
		StreamSettings settings;
		double retry_window_s = 20.0;   // keep restarting a session that never connected
		double retry_delay_s = 1.0;
	};
	using FinishedFn = std::function<void(ChiakiQuitReason reason, const std::string &text)>;

	/** video, audio and controller may be null; they must outlive the session. */
	StreamSession(VideoPath *video, PcmSink *audio, ControllerHub *controller);
	~StreamSession() override;
	StreamSession(const StreamSession &) = delete;
	StreamSession &operator=(const StreamSession &) = delete;

	bool start(const Config &config, FinishedFn finished);
	/** Any thread, non-blocking, idempotent: end the session (no retry). */
	void requestStop();
	/**
	 * Any thread, non-blocking: end the running session and start a new one with these
	 * settings, on the same sinks. The console fixes the codec when a session starts, so
	 * switching HDR on or off is a new session. The retry window starts over: right after
	 * the old session ended the console may still answer "in use". finished() is not
	 * called for the session that is replaced.
	 * @return false when the stream has already ended or is being stopped
	 */
	bool restart(const StreamSettings &settings);
	uint64_t restarts() const { return restarts_.load(); }
	/** Wait for the lifecycle thread (the session is shut down after this). */
	void join();
	bool running() const { return running_.load(); }

	/** Any thread (the controller's HID thread). */
	void setControllerState(const ChiakiControllerState &state);
	/** Any thread: schedule an IDR request (coalesced, never blocks the caller). */
	void scheduleKeyframeRequest(const char *why);

	bool connected() const { return connected_.load(); }
	uint64_t keyframeRequests() const { return idr_requests_.load(); }

	// SessionDelegate
	void onConnected(const ChiakiConnectedEvent &info) override;
	void onQuit(ChiakiQuitReason reason, const char *reason_str) override;
	void onLoginPinRequest(bool pin_incorrect) override;
	void onStreamStats(const ChiakiStreamStatsEvent &stats) override;
	void onRumble(const ChiakiRumbleEvent &rumble) override;
	void onTriggerEffects(const ChiakiTriggerEffectsEvent &effects) override;
	void onHapticIntensity(ChiakiDualSenseEffectIntensity intensity) override;
	void onTriggerIntensity(ChiakiDualSenseEffectIntensity intensity) override;
	void onLedColor(const uint8_t rgb[3]) override;
	void onPlayerIndex(uint8_t index) override;
	void onMotionReset() override;
	void onHapticsFrame(const uint8_t *buf, size_t size) override;
	void onVideoFecFailure(const ChiakiVideoFecFailureEvent &failure) override;
	void onCantDisplay(bool cant_display) override;
	void onOtherEvent(const ChiakiEvent &event) override;
	void onLog(ChiakiLogLevel level, const char *message) override;

private:
	void run();

	ControllerHub *const controller_;
	SessionHost host_;
	Config config_;
	FinishedFn finished_;
	std::thread thread_;
	std::atomic<bool> running_{false};
	std::atomic<bool> stop_requested_{false};
	std::atomic<bool> connected_{false};
	std::atomic<bool> ever_connected_{false};
	std::mutex wait_mutex_;
	std::condition_variable wait_cond_;
	std::mutex restart_mutex_;
	std::optional<StreamSettings> restart_settings_; // under restart_mutex_
	std::atomic<bool> restart_requested_{false};
	std::atomic<uint64_t> restarts_{0};

	dispatch_queue_t idr_queue_ = nullptr;
	std::atomic<bool> idr_pending_{false};
	std::atomic<uint64_t> idr_requests_{0};
};

} // namespace plk::app

#endif // PLK_APP_STREAMSESSION_H
