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

#include "Options.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace plk {
class AudioOutput;
}

namespace plk::app {

class ControllerHub;
class FrameLogger;
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

	int exitCode() const { return exit_code_.load(); }
	const Options &options() const { return options_; }
	const StreamConfig &streamConfig() const { return stream_config_; }
	std::string title() const;

private:
	void statsLoop();
	void logStats(bool final_report);

	const Options options_;
	StreamConfig stream_config_;
	std::unique_ptr<struct RunnerSecrets> secrets_;

	std::unique_ptr<FrameLogger> frame_logger_;
	std::unique_ptr<video::VideoPipeline> pipeline_;
	std::unique_ptr<VideoPath> video_path_;
	std::unique_ptr<AudioOutput> audio_;
	std::unique_ptr<ControllerHub> controller_;
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
	bool workers_down_ = false;
};

} // namespace plk::app

#endif // PLK_APP_RUNNER_H
