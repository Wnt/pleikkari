// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Plays a PLKVDMP1 video dump (recorded by plk-host-probe, the Qt client or this app's
// --dump) through the same VideoPath a live session uses, on a feeder thread at user-
// interactive QoS, with the recorded cadence: each frame is submitted when its recorded
// frame_ready_us offset comes due, and frame_ready_us is re-stamped to "now" on the lib's
// clock (CLOCK_MONOTONIC), as the lib would stamp it. Pauses in the recording are capped
// at 250 ms. No console, network or controller is involved.

#ifndef PLK_APP_REPLAY_H
#define PLK_APP_REPLAY_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace plk::app {

class VideoPath;

class Replay
{
public:
	Replay(VideoPath *video, std::string path, bool loop);
	~Replay();

	/** Reads the dump; false with *error if it cannot be read or has no frames. */
	bool load(std::string *error);
	/** Starts the feeder; finished() runs on it when the dump ends (or stop()). */
	void start(std::function<void()> finished);
	void stop();
	void join();

	size_t records() const { return records_.size(); }
	uint64_t fed() const { return fed_.load(); }

private:
	struct Record
	{
		bool config = false;
		bool recovered = false;
		uint16_t frame_index = 0;
		int32_t frames_lost = 0;
		uint64_t offset_us = 0;  // frame_ready_us minus the first frame's
		std::vector<uint8_t> payload;
	};
	void run();

	VideoPath *const video_;
	const std::string path_;
	const bool loop_;
	std::vector<Record> records_;
	std::function<void()> finished_;
	std::thread thread_;
	std::atomic<bool> stop_{false};
	std::atomic<uint64_t> fed_{0};
};

} // namespace plk::app

#endif // PLK_APP_REPLAY_H
