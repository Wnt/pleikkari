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
// With a dump path every sample is also copied to a PLKVDMP1 file on its own thread.

#ifndef PLK_APP_VIDEOPATH_H
#define PLK_APP_VIDEOPATH_H

#include "Sinks.h"
#include "VideoDump.h"

#include <plk/VideoPipeline.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace plk::app {

class VideoPath : public VideoSink
{
public:
	explicit VideoPath(video::VideoPipeline *pipeline);
	~VideoPath() override;

	/** Record every sample from now on; false if the file cannot be created. */
	bool openDump(const std::string &path, std::string *error);
	void closeDump();

	bool onVideoSample(uint8_t *buf, size_t buf_size, uint16_t frame_index, uint64_t frame_ready_time_us,
			int32_t frames_lost, bool recovered) override;

	uint64_t configs() const { return configs_.load(); }
	uint64_t frames() const { return frames_.load(); }
	uint64_t rejected() const { return rejected_.load(); }
	uint64_t dumpDropped() const;

private:
	video::VideoPipeline *const pipeline_;
	std::unique_ptr<AsyncVideoDumpWriter> dump_;
	std::atomic<uint64_t> configs_{0}, frames_{0}, rejected_{0};
};

} // namespace plk::app

#endif // PLK_APP_VIDEOPATH_H
