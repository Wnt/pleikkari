// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Writer for the PLKVDMP1 video sample dump (read by the video module's replay tool).
//
//   file:   8 bytes magic "PLKVDMP1"
//   record: uint32 payload_size
//           uint16 frame_index
//           uint16 flags            bit0 = config sample (frame_ready_us == 0), bit1 = recovered
//           uint64 frame_ready_us
//           int32  frames_lost
//           uint8  payload[payload_size]
//
// All integers little-endian. A record header is 20 bytes.

#ifndef PLK_HOST_VIDEODUMP_H
#define PLK_HOST_VIDEODUMP_H

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace plk {

constexpr char kVideoDumpMagic[8] = { 'P', 'L', 'K', 'V', 'D', 'M', 'P', '1' };
constexpr size_t kVideoDumpRecordHeaderSize = 20;
constexpr uint16_t kVideoDumpFlagConfig = 1u << 0;
constexpr uint16_t kVideoDumpFlagRecovered = 1u << 1;

/** Serialise one record header into out[kVideoDumpRecordHeaderSize]. */
void videoDumpEncodeHeader(uint8_t *out, uint32_t payload_size, uint16_t frame_index, uint64_t frame_ready_us,
		int32_t frames_lost, bool recovered);

/** Synchronous writer: each write() goes to stdio. */
class VideoDumpWriter
{
public:
	VideoDumpWriter() = default;
	~VideoDumpWriter() { close(); }
	VideoDumpWriter(const VideoDumpWriter &) = delete;
	VideoDumpWriter &operator=(const VideoDumpWriter &) = delete;

	bool open(const std::string &path, std::string *error = nullptr);
	bool write(const uint8_t *payload, size_t size, uint16_t frame_index, uint64_t frame_ready_us,
			int32_t frames_lost, bool recovered);
	/** A record already encoded as header + payload. */
	bool writeEncoded(const uint8_t *record, size_t size);
	/** Flushes and closes; false if anything failed since open(). */
	bool close();
	bool isOpen() const { return file_ != nullptr; }
	uint64_t records() const { return records_; }

private:
	FILE *file_ = nullptr;
	bool failed_ = false;
	uint64_t records_ = 0;
};

/**
 * Copies each sample and writes it on its own thread, so disk I/O never stalls the
 * library's receive thread. For the probe; the copy is the price of that.
 */
class AsyncVideoDumpWriter
{
public:
	AsyncVideoDumpWriter() = default;
	~AsyncVideoDumpWriter() { close(); }
	AsyncVideoDumpWriter(const AsyncVideoDumpWriter &) = delete;
	AsyncVideoDumpWriter &operator=(const AsyncVideoDumpWriter &) = delete;

	bool open(const std::string &path, std::string *error = nullptr);
	/** Any thread. Returns false (and counts a drop) if more than max_queued_bytes are waiting. */
	bool write(const uint8_t *payload, size_t size, uint16_t frame_index, uint64_t frame_ready_us,
			int32_t frames_lost, bool recovered);
	bool close();
	uint64_t dropped() const;

private:
	void run();

	struct Item
	{
		std::vector<uint8_t> bytes; // header + payload
	};

	VideoDumpWriter writer_;
	std::thread thread_;
	mutable std::mutex mutex_;
	std::condition_variable cond_;
	std::deque<Item> queue_;
	size_t queued_bytes_ = 0;
	size_t max_queued_bytes_ = 256u << 20;
	uint64_t dropped_ = 0;
	bool stop_ = false;
	bool write_failed_ = false;
};

} // namespace plk

#endif // PLK_HOST_VIDEODUMP_H
