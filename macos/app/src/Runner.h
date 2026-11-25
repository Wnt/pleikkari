// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Everything a run of Pleikkari owns, in the order it is built and torn down. Shared by
// the windowed app (main.mm's AppDelegate) and the headless mode.
//
//   prepare()         log file, registration (stream) or dump (replay), stream settings
//   createPipeline()  VideoToolbox -> Metal pipeline on the window's layer (or offscreen)
//   start()           audio output, DualSense manager, session or replay, stats thread
//   requestStop()     any thread, non-blocking
//   shutdownWorkers() blocking, not on the main thread of the app: session stop + shutdown,
//                     DualSense manager stop (effects off), stats thread, dump file
//   shutdownVideo()   pipeline teardown (main thread in the app), trace callback removed,
//                     final stats, log flushed

#ifndef PLK_APP_RUNNER_H
#define PLK_APP_RUNNER_H

#include "FrameAnalysis.h"
#include "Options.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace plk {
class AudioOutput;
}

namespace plk::app {

class ControllerHub;
class FrameLogger;
class GamepadHub;
class NeuralEnhancer;
class Replay;
class StreamSession;
class VideoPath;

class Runner
{
public:
	explicit Runner(const Options &options);
	~Runner();
	Runner(const Runner &) = delete;
	Runner &operator=(const Runner &) = delete;

	/** false: do not continue; *exit_code says how to exit. */
	bool prepare(int *exit_code);
	/** metalLayer: a CAMetalLayer* (bridged). Headless runs render offscreen. */
	bool createPipeline(void *metalLayer, float display_refresh_hz);
	void setDrawableSize(int width, int height);
	/** finished() runs once, on a worker thread, when the session or replay has ended. */
	bool start(std::function<void()> finished);
	void requestStop();
	void shutdownWorkers();
	void shutdownVideo();

	// What the Video menu changes while the run goes on. All of it from one thread (the
	// app's main thread). The first three take effect with the next frame.
	struct ViewState
	{
		video::Scaler scaler = video::Scaler::Bilinear;
		video::SdrColor sdr_color = video::SdrColor::Accurate;
		video::HdrOutput hdr_output = video::HdrOutput::PQ;
		int hdr_brightness = kHdrBrightnessDefault; // percent
		video::HdrToneMap hdr_tone_map = video::HdrToneMap::App;
		bool fullscreen_sync = true;
		bool fullscreen_sync_applies = true; // false: --pacing fixed something else
		bool hdr = false;            // the stream is asked for in HDR
		bool hdr_switchable = false; // a PS5 stream: setHdr() can change that
		// Neural rendering (NeuralEnhancer.h). An HDR picture is never changed by it.
		bool nr = false;
		NrNetwork nr_network = NrNetwork::Small;
		int nr_height = kNrHeightDefault;      // 0: the stream's own
		int nr_strength = kNrStrengthDefault;  // percent
		NrLook nr_look = NrLook::Standard;
		bool nr_compare = false;     // the left half of the picture stays as decoded
		bool nr_available = false;   // the engine is loaded, or it and the chosen network's model are on disk
		bool nr_loading = false;
		std::string nr_status;       // the loaded model, or why there is none
	};
	ViewState viewState() const;
	void setScaler(video::Scaler scaler);
	void setSdrColor(video::SdrColor color);
	void setHdrOutput(video::HdrOutput output);
	/** Brightness of an HDR picture in percent, kept within the range. @return what was set */
	int setHdrBrightness(int percent);
	void setHdrToneMap(video::HdrToneMap tone_map);
	/** What the display shows now, SDR white = 1 (video::usableDisplayHeadroom()). */
	void setDisplayHeadroom(float headroom);
	/** See effectivePacing(). The window tells when it is fullscreen. */
	void setFullscreenSync(bool on);
	void setWindowFullscreen(bool fullscreen);
	/**
	 * HDR on or off. The console fixes the codec when a session starts, so this ends the
	 * session and starts a new one (a few seconds without picture); window, video pipeline,
	 * audio output and controller stay.
	 * @return false when this is not a PS5 stream or the stream has ended
	 */
	bool setHdr(bool on);
	/** The session the last setHdr() asked for is connected: the picture is about to return. */
	bool hdrSwitchSettled() const;
	/** The picture being shown is HDR (false until the first frame). */
	bool pictureIsHdr() const;

	// Neural rendering. The engine and the model are loaded on a thread of their own the
	// first time they are wanted (and again for the other network); the picture goes on
	// unchanged until they are there. What becomes of a load is told through the notice
	// handler, from that thread.
	void setNr(bool on);
	/**
	 * @return false when that network's model (or the engine) is not installed: nothing
	 * changes then, the network in use keeps working, and *why says what is missing
	 */
	bool setNrNetwork(NrNetwork network, std::string *why = nullptr);
	void setNrHeight(int height);
	/** @return what was set, within the range */
	int setNrStrength(int percent);
	void setNrLook(NrLook look);
	void setNrCompare(bool on);
	void setNoticeHandler(std::function<void(const std::string &)> handler);
	/** What each stage of a frame costs right now (FrameAnalysis.h), with the words around it. */
	AnalysisReport analysis() const;
	/**
	 * A key of the Video menu, for a run without a window (--headless --keys): the choice is
	 * made as the menu would make it, nothing is remembered. HDR on/off (H) is not one of them.
	 * @return false when the key is none of the menu's; *what says what was done
	 */
	bool pressKey(const std::string &key, std::string *what);

	int exitCode() const { return exit_code_.load(); }
	const Options &options() const { return options_; }
	const StreamConfig &streamConfig() const { return stream_config_; }
	std::string title() const;

private:
	void statsLoop();
	void logStats(bool final_report);
	void nrLoop();
	void requestNrEngine();
	bool streamAskedHdr() const;
	void syncAnalysisActiveLocked();
	void applyEnhanceConfig();

	const Options options_;
	StreamConfig stream_config_;
	ViewChoices view_;
	bool ps5_ = false;
	ChiakiCodec sdr_codec_ = CHIAKI_CODEC_H265; // what HDR off goes back to
	uint64_t hdr_switch_restart_ = 0;           // the session restart count the last setHdr() waits for
	std::atomic<float> display_headroom_{0.0f};
	void applyPacing();
	bool window_fullscreen_ = false;
	video::PacingMode pacing_ = video::PacingMode::Immediate; // what the pipeline has
	std::unique_ptr<struct RunnerSecrets> secrets_;

	std::unique_ptr<FrameLogger> frame_logger_;
	std::unique_ptr<FrameAnalysis> analysis_;
	std::unique_ptr<video::VideoPipeline> pipeline_;
	std::unique_ptr<VideoPath> video_path_;
	std::unique_ptr<AudioOutput> audio_;
	std::unique_ptr<ControllerHub> controller_;
	std::unique_ptr<GamepadHub> gamepads_;
	// The DualSense hub and the gamepad hub each report the union of their own
	// controllers; the console gets the union of the two.
	void pushControllerState(bool from_gamepads, const ChiakiControllerState &state);
	std::mutex pad_state_mutex_;
	ChiakiControllerState dualsense_state_;
	ChiakiControllerState gamepad_state_;
	std::unique_ptr<StreamSession> session_;
	std::unique_ptr<Replay> replay_;
	bool trace_installed_ = false;

	std::atomic<int> exit_code_{0};
	std::atomic<bool> stop_requested_{false};
	std::function<void()> finished_;
	std::once_flag finished_once_;

	std::thread stats_thread_;
	std::mutex stats_mutex_;
	std::condition_variable stats_cond_;
	bool stats_stop_ = false;
	std::thread seconds_thread_;

	// Neural rendering. nr_mutex_ guards everything below it, and the writes of view_'s nr
	// fields: the menu's thread writes them, the loader and the statistics thread read them.
	std::thread nr_thread_;
	mutable std::mutex nr_mutex_;
	bool nr_compare_ = false;
	std::condition_variable nr_cond_;
	bool nr_stop_ = false;
	bool nr_request_ = false;
	bool nr_loading_ = false;
	NrNetwork nr_request_network_ = NrNetwork::Small;
	NrLook nr_request_look_ = NrLook::Standard;
	std::shared_ptr<NeuralEnhancer> nr_engine_;
	NrNetwork nr_engine_network_ = NrNetwork::Small;
	std::string nr_failure_;
	std::function<void(const std::string &)> notice_;

	bool workers_down_ = false;
	bool video_down_ = false;
};

} // namespace plk::app

#endif // PLK_APP_RUNNER_H
