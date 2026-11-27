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

#include "CameraWarp.h"
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
class FrameGenerator;
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
		ChiakiVideoResolutionPreset resolution = CHIAKI_VIDEO_RESOLUTION_PRESET_1080p; // what the console is asked for
		unsigned bitrate_kbps = 0;   // what the console is asked for
		bool hdr = false;            // the stream is asked for in HDR
		bool hdr_switchable = false; // a PS5 stream: setHdr() can change that
		// Neural rendering (NeuralEnhancer.h). An HDR picture is never changed by it.
		bool nr = false;
		NrNetwork nr_network = NrNetwork::HT;
		int nr_height = kNrHeightDefault;      // 0: the stream's own
		int nr_strength = kNrStrengthDefault;  // percent
		NrLook nr_look = NrLook::Standard;
		bool camera_warp = false;              // the picture moved by what the right stick asked for
		CameraWarp::State camera_warp_state;   // what has been learned of the game's answer to the stick
		int frame_generation_factor = 2;       // 2 or 4 times the game's pictures
		bool frame_generation = false;         // the picture between two of the game's is generated
		bool frame_generation_available = false; // the engine and the weights are there
		std::string frame_generation_status;   // why not, or what is loaded
		NrTiming nr_timing = NrTiming::Wait;  // when a result reaches the screen
		float nr_compare_split = 0.5f; // where the comparison divides the picture, 0...1 of its width
		bool nr_compare = false;     // the left half of the picture stays as decoded
		bool nr_available = false;   // the engine is loaded, or it and the chosen network's model are on disk
		bool nr_loading = false;
		std::string nr_status;       // the loaded model, or why there is none
		bool fps_overlay = false;    // the frame rate over the picture's corner (FrameHud.h)
		bool panel_beside = true;    // with the pipeline panel open, the picture is fitted beside it
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
	/** Where the comparison divides the picture: 0...1 of its width, from the left. */
	void setNrCompareSplit(float share);
	void setNrTiming(NrTiming timing);
	/** Asks the console for another picture size: the session restarts, as with setHdr(). False when it cannot (not a live stream). */
	bool setStreamResolution(ChiakiVideoResolutionPreset resolution);
	/** The same for the bitrate, in kbit/s. */
	bool setStreamBitrate(unsigned kbps);
	void setFrameGeneration(bool on);
	void setCameraWarp(bool on);
	/** 2: one picture between two of the game's (60 a second of 30); 4: three (120). */
	void setFrameGenerationFactor(int factor);
	// The frame rate overlay: drawn about ten times a second on a thread of its own and
	// handed to the pipeline, which puts it over the picture's corner.
	void setFpsOverlay(bool on);
	/** Pixels per point of the display the picture is on (a headless run picks its own). */
	void setOverlayScale(int scale);
	/** Remembered here only: the window does the fitting, through setContentInsets(). */
	void setPanelBeside(bool beside);
	/** Pixels of the drawable, from each edge, that a panel covers: the picture is fitted into the rest. */
	void setContentInsets(int left, int top, int right, int bottom);
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
	void hudLoop();
	void requestNrEngine();
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
	// The union of the controllers as the console gets it, with what the lag probe holds
	// the right stick at; told to the camera warp too.
	void sendControllerState();
	// Camera warp and the lag probe: both live on how far each new picture moved.
	void applyWarp();
	// What the warp learned, kept in a file between sittings (one for each console).
	void loadWarp();
	void saveWarp();
	void lagProbeLoop();
	CameraWarp camera_warp_;
	float warp_shown_x_ = 0, warp_shown_y_ = 0;  // the render thread only: what the last frame was moved by
	double warp_shown_us_ = 0;
	uint64_t warp_logged_ = 0;                    // probe_mutex_
	std::atomic<bool> warp_other_view_{false}, warp_r3_was_{false};  // see sendControllerState()
	std::atomic<float> probe_stick_{0.0f};    // the probe's right stick sideways, -1...1; with probe_holds_
	std::atomic<float> probe_stick_y_{0.0f};  // and up and down
	std::atomic<bool> probe_holds_{false};    // the probe has the right stick
	void stickSweepLoop();
	FILE *sweep_stick_ = nullptr, *sweep_motion_ = nullptr;  // probe_mutex_
	// --record-input and --play-input: a sitting recorded, and played back as the controller.
	void inputPlayLoop();
	// --control: a program on a socket sees the pictures and is the controller.
	void controlLoop();
	std::thread control_thread_;
	std::atomic<int> control_listen_{-1}, control_client_{-1};
	std::mutex control_send_mutex_;
	std::vector<uint8_t> control_out_;        // control_send_mutex_: a picture on its way to the program
	bool control_out_new_ = false;            // control_send_mutex_
	std::condition_variable control_send_cond_;
	FILE *input_log_ = nullptr;               // probe_mutex_
	ChiakiControllerState input_logged_{};    // probe_mutex_: the last state written
	ChiakiControllerState play_state_{};      // pad_state_mutex_; with play_holds_
	std::atomic<bool> play_holds_{false};     // the player is the controller
	std::thread probe_thread_;
	std::mutex probe_mutex_;
	std::condition_variable probe_cond_;
	bool probe_stop_ = false;           // probe_mutex_, as all below
	bool probe_armed_ = false;          // the stick was pushed: the first picture that moves is looked for
	double probe_stick_us_ = 0;
	double probe_arrival_us_ = 0;       // when that picture arrived; 0: not yet
	double probe_glass_us_ = 0;         // when it was on screen; 0: not yet
	double probe_recent_[8] = {};       // how far the last eight pictures moved, as shares of the width
	int probe_recent_at_ = 0;
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
	float nr_compare_split_ = 0.5f;
	std::condition_variable nr_cond_;
	bool nr_stop_ = false;
	bool nr_request_ = false;
	bool nr_loading_ = false;
	// Frame generation: its generator is loaded on a thread of its own the first time it is switched on.
	std::shared_ptr<FrameGenerator> fg_engine_;   // nr_mutex_
	bool fg_loading_ = false;                     // nr_mutex_
	std::string fg_failure_;                      // nr_mutex_
	std::thread fg_thread_;
	NrNetwork nr_request_network_ = NrNetwork::Small;
	NrLook nr_request_look_ = NrLook::Standard;
	std::shared_ptr<NeuralEnhancer> nr_engine_;
	NrNetwork nr_engine_network_ = NrNetwork::Small;
	std::string nr_failure_;
	std::function<void(const std::string &)> notice_;

	// The frame rate overlay's thread; hud_mutex_ guards the three below it.
	std::thread hud_thread_;
	std::mutex hud_mutex_;
	std::condition_variable hud_cond_;
	bool hud_stop_ = false;
	bool hud_on_ = false;
	int hud_scale_ = 1;

	bool workers_down_ = false;
	bool video_down_ = false;
};

} // namespace plk::app

#endif // PLK_APP_RUNNER_H
