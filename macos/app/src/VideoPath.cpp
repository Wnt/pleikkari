// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "VideoPath.h"

#include "Log.h"

#include <cstring>

namespace plk::app {

std::string dumpSegmentPath(const std::string &first, unsigned index, const std::string &label)
{
	const size_t slash = first.find_last_of('/');
	const size_t name = slash == std::string::npos ? 0 : slash + 1;
	const size_t dot = first.find_last_of('.');
	// A leading dot is a hidden file's name, not an extension.
	const bool extension = dot != std::string::npos && dot > name;
	const std::string stem = extension ? first.substr(0, dot) : first;
	return stem + "-" + std::to_string(index) + (label.empty() ? "" : "-" + label) + (extension ? first.substr(dot) : "");
}

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
	std::lock_guard<std::mutex> lk(dump_mutex_);
	dump_ = std::move(dump);
	dump_first_ = path;
	dump_index_ = 1;
	dump_frames_ = 0;
	dump_config_.clear();
	dumping_.store(true);
	return true;
}

void VideoPath::setDumpLabel(const std::string &label)
{
	std::lock_guard<std::mutex> lk(dump_mutex_);
	dump_label_ = label;
}

void VideoPath::closeDump()
{
	std::lock_guard<std::mutex> lk(dump_mutex_);
	dumping_.store(false);
	if(dump_)
	{
		dump_dropped_ += dump_->dropped();
		dump_->close();
		dump_.reset();
	}
}

uint64_t VideoPath::dumpDropped() const
{
	std::lock_guard<std::mutex> lk(dump_mutex_);
	return dump_dropped_ + (dump_ ? dump_->dropped() : 0);
}

// Another stream starts in a recording that already holds one: the file being written
// is finished and the next one begun. Closing waits for the samples still queued for the
// disk, a few at most, and happens where a stream starts and never inside one.
void VideoPath::nextDumpLocked()
{
	dump_dropped_ += dump_->dropped();
	dump_->close();
	dump_.reset();
	dump_frames_ = 0;
	const std::string path = dumpSegmentPath(dump_first_, ++dump_index_, dump_label_);
	auto next = std::make_unique<AsyncVideoDumpWriter>();
	std::string error;
	if(!next->open(path, &error))
	{
		PLK_LOGW("Video dump: %s; the recording ends here", error.c_str());
		dumping_.store(false);
		return;
	}
	dump_ = std::move(next);
	PLK_LOGI("Recording the stream that starts here to %s", path.c_str());
}

bool VideoPath::onVideoSample(uint8_t *buf, size_t buf_size, uint16_t frame_index, uint64_t frame_ready_time_us,
		int32_t frames_lost, bool recovered)
{
	if(dumping_.load(std::memory_order_relaxed))
	{
		std::lock_guard<std::mutex> lk(dump_mutex_);
		// Another config sample after frames: another stream.
		const bool config = frame_ready_time_us == 0;
		if(dump_ && config && dump_frames_ > 0
			&& (dump_config_.size() != buf_size || memcmp(dump_config_.data(), buf, buf_size) != 0))
			nextDumpLocked();
		if(dump_)
		{
			dump_->write(buf, buf_size, frame_index, frame_ready_time_us, frames_lost, recovered);
			if(config)
				dump_config_.assign(buf, buf + buf_size);
			else
				dump_frames_++;
		}
	}
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
