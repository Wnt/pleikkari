// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "VideoDump.h"

#include <cerrno>
#include <cstring>

namespace plk {

namespace {

void putLe16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

void putLe32(uint8_t *p, uint32_t v)
{
	for(int i = 0; i < 4; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}

void putLe64(uint8_t *p, uint64_t v)
{
	for(int i = 0; i < 8; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}

} // namespace

void videoDumpEncodeHeader(uint8_t *out, uint32_t payload_size, uint16_t frame_index, uint64_t frame_ready_us,
		int32_t frames_lost, bool recovered)
{
	uint16_t flags = 0;
	if(frame_ready_us == 0)
		flags |= kVideoDumpFlagConfig;
	if(recovered)
		flags |= kVideoDumpFlagRecovered;
	putLe32(out + 0, payload_size);
	putLe16(out + 4, frame_index);
	putLe16(out + 6, flags);
	putLe64(out + 8, frame_ready_us);
	putLe32(out + 16, (uint32_t)frames_lost);
}

bool VideoDumpWriter::open(const std::string &path, std::string *error)
{
	close();
	file_ = fopen(path.c_str(), "wb");
	if(!file_)
	{
		if(error)
			*error = "cannot create " + path + ": " + strerror(errno);
		return false;
	}
	failed_ = fwrite(kVideoDumpMagic, 1, sizeof(kVideoDumpMagic), file_) != sizeof(kVideoDumpMagic);
	records_ = 0;
	return !failed_;
}

bool VideoDumpWriter::write(const uint8_t *payload, size_t size, uint16_t frame_index, uint64_t frame_ready_us,
		int32_t frames_lost, bool recovered)
{
	if(!file_ || size > UINT32_MAX)
		return false;
	uint8_t header[kVideoDumpRecordHeaderSize];
	videoDumpEncodeHeader(header, (uint32_t)size, frame_index, frame_ready_us, frames_lost, recovered);
	bool ok = fwrite(header, 1, sizeof(header), file_) == sizeof(header)
		&& (size == 0 || fwrite(payload, 1, size, file_) == size);
	failed_ = failed_ || !ok;
	if(ok)
		records_++;
	return ok;
}

bool VideoDumpWriter::writeEncoded(const uint8_t *record, size_t size)
{
	if(!file_)
		return false;
	bool ok = fwrite(record, 1, size, file_) == size;
	failed_ = failed_ || !ok;
	if(ok)
		records_++;
	return ok;
}

bool VideoDumpWriter::close()
{
	if(!file_)
		return !failed_;
	bool ok = fflush(file_) == 0;
	ok = (fclose(file_) == 0) && ok;
	file_ = nullptr;
	ok = ok && !failed_;
	failed_ = false;
	return ok;
}

bool AsyncVideoDumpWriter::open(const std::string &path, std::string *error)
{
	close();
	if(!writer_.open(path, error))
		return false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stop_ = false;
		write_failed_ = false;
		dropped_ = 0;
	}
	thread_ = std::thread(&AsyncVideoDumpWriter::run, this);
	return true;
}

bool AsyncVideoDumpWriter::write(const uint8_t *payload, size_t size, uint16_t frame_index, uint64_t frame_ready_us,
		int32_t frames_lost, bool recovered)
{
	if(size > UINT32_MAX)
		return false;
	Item item;
	item.bytes.resize(kVideoDumpRecordHeaderSize + size);
	videoDumpEncodeHeader(item.bytes.data(), (uint32_t)size, frame_index, frame_ready_us, frames_lost, recovered);
	if(size)
		memcpy(item.bytes.data() + kVideoDumpRecordHeaderSize, payload, size);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if(!thread_.joinable() || stop_ || queued_bytes_ + item.bytes.size() > max_queued_bytes_)
		{
			dropped_++;
			return false;
		}
		queued_bytes_ += item.bytes.size();
		queue_.push_back(std::move(item));
	}
	cond_.notify_one();
	return true;
}

void AsyncVideoDumpWriter::run()
{
	std::unique_lock<std::mutex> lock(mutex_);
	while(true)
	{
		cond_.wait(lock, [this] { return stop_ || !queue_.empty(); });
		if(queue_.empty() && stop_)
			break;
		Item item = std::move(queue_.front());
		queue_.pop_front();
		queued_bytes_ -= item.bytes.size();
		lock.unlock();
		bool ok = writer_.writeEncoded(item.bytes.data(), item.bytes.size());
		lock.lock();
		write_failed_ = write_failed_ || !ok;
	}
}

bool AsyncVideoDumpWriter::close()
{
	if(!thread_.joinable())
		return writer_.close();
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stop_ = true;
	}
	cond_.notify_one();
	thread_.join();
	bool ok = writer_.close();
	std::lock_guard<std::mutex> lock(mutex_);
	return ok && !write_failed_;
}

uint64_t AsyncVideoDumpWriter::dropped() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return dropped_;
}

} // namespace plk
