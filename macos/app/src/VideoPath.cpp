// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "VideoPath.h"

namespace plk::app {

VideoPath::VideoPath(video::VideoPipeline *pipeline) : pipeline_(pipeline) {}

VideoPath::~VideoPath()
{
	closeDump();
}

bool VideoPath::openDump(const std::string &path, std::string *error)
{
	auto dump = std::make_unique<AsyncVideoDumpWriter>();
	if(!dump->open(path, error))
		return false;
	dump_ = std::move(dump);
	return true;
}

void VideoPath::closeDump()
{
	if(dump_)
	{
		dump_->close();
		dump_.reset();
	}
}

uint64_t VideoPath::dumpDropped() const
{
	return dump_ ? dump_->dropped() : 0;
}

bool VideoPath::onVideoSample(uint8_t *buf, size_t buf_size, uint16_t frame_index, uint64_t frame_ready_time_us,
		int32_t frames_lost, bool recovered)
{
	if(dump_)
		dump_->write(buf, buf_size, frame_index, frame_ready_time_us, frames_lost, recovered);
	if(!pipeline_)
		return true;
	if(frame_ready_time_us == 0)
	{
		configs_.fetch_add(1, std::memory_order_relaxed);
		const bool ok = pipeline_->submitConfig(buf, buf_size);
		if(!ok)
			rejected_.fetch_add(1, std::memory_order_relaxed);
		return ok;
	}
	frames_.fetch_add(1, std::memory_order_relaxed);
	const bool ok = pipeline_->submitFrame(buf, buf_size, frame_index, frame_ready_time_us, recovered, frames_lost);
	if(!ok)
		rejected_.fetch_add(1, std::memory_order_relaxed);
	return ok;
}

} // namespace plk::app
