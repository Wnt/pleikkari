// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// The video sink of a stream: every sample goes to the pipeline on the thread that
// delivers it (the lib's Takion thread, or the replay feeder), with no queue in between.
// The pipeline presents a frame as soon as it is decoded (PacingMode::Immediate).
//
//   config sample (frame_ready_time_us == 0)  -> VideoPipeline::submitConfig
//   access unit                               -> VideoPipeline::submitFrame
//
// A pipeline error that asks for an IDR (decode failure, rebuilt decoder session) is
// handed to the keyframe callback, which must only schedule the request.
// With a dump path every sample is also copied to a PLKVDMP1 file on its own thread. A
// different stream that starts after the first one (the session restarted for HDR on or
// off, the console changed the stream) begins with a config sample of its own and gets a
// file of its own, so that every file holds one kind of stream and replays by itself.

#ifndef PLK_APP_VIDEOPATH_H
#define PLK_APP_VIDEOPATH_H

#include "Sinks.h"
#include "VideoDump.h"

#include <plk/VideoPipeline.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace plk::app {

/**
 * The file of the `index`th stream of a recording whose first stream went to `first`:
 * ("runs/video.plkv", 2, "sdr") is "runs/video-2-sdr.plkv".
 */
std::string dumpSegmentPath(const std::string &first, unsigned index, const std::string &label);

class VideoPath : public VideoSink
{
public:
	explicit VideoPath(video::VideoPipeline *pipeline);
	~VideoPath() override;

	/**
	 * Record every sample from now on; false if the file cannot be created. `path` gets
	 * the first stream, and each stream with another config sample that starts after it
	 * dumpSegmentPath(path, 2, ...), 3 and so on. The same config sample again (a session
	 * that reconnected as it was) continues the file: a replay skips the pause.
	 */
	bool openDump(const std::string &path, std::string *error);
	/** What the files of the streams that start from now on are named after ("hdr", "sdr"). */
	void setDumpLabel(const std::string &label);
	void closeDump();

	bool onVideoSample(uint8_t *buf, size_t buf_size, uint16_t frame_index, uint64_t frame_ready_time_us,
			int32_t frames_lost, bool recovered) override;

	uint64_t configs() const { return configs_.load(); }
	uint64_t frames() const { return frames_.load(); }
	uint64_t rejected() const { return rejected_.load(); }
	uint64_t dumpDropped() const;

private:
	void nextDumpLocked();

	video::VideoPipeline *const pipeline_;
	// The recording: touched by the thread that delivers the samples and by whoever
	// opens, labels, closes or asks about it.
	std::atomic<bool> dumping_{false};
	mutable std::mutex dump_mutex_;
	std::unique_ptr<AsyncVideoDumpWriter> dump_;
	std::string dump_first_, dump_label_;
	std::vector<uint8_t> dump_config_;  // the config sample of the file being written
	unsigned dump_index_ = 0;         // of the stream being written, from 1
	uint64_t dump_frames_ = 0;        // access units in the file being written
	uint64_t dump_dropped_ = 0;       // in the files already closed
	std::atomic<uint64_t> configs_{0}, frames_{0}, rejected_{0};
};

} // namespace plk::app

#endif // PLK_APP_VIDEOPATH_H
