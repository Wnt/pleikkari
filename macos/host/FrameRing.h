// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Wait-free single-producer single-consumer ring of interleaved float audio frames:
// the hand-off between the library's receive thread (producer, Opus decoder output)
// and the CoreAudio IO thread (consumer). No locks, no allocation after construction.

#ifndef PLK_HOST_FRAMERING_H
#define PLK_HOST_FRAMERING_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace plk {

class FrameRing
{
public:
	/** @param capacity_frames rounded up to a power of two */
	FrameRing(size_t capacity_frames, unsigned max_channels)
		: max_channels_(max_channels ? max_channels : 1)
	{
		size_t cap = 1;
		while(cap < capacity_frames)
			cap <<= 1;
		capacity_ = cap;
		buf_.assign(capacity_ * max_channels_, 0.0f);
	}

	FrameRing(const FrameRing &) = delete;
	FrameRing &operator=(const FrameRing &) = delete;

	size_t capacity() const { return capacity_; }
	unsigned maxChannels() const { return max_channels_; }

	/**
	 * Channel count of the frames in the ring. Change it only while neither side runs
	 * (both callbacks stopped), together with reset().
	 */
	void setChannels(unsigned channels) { channels_ = channels == 0 ? 1 : (channels > max_channels_ ? max_channels_ : channels); }
	unsigned channels() const { return channels_; }

	/** Empty the ring. Only while neither side runs. */
	void reset()
	{
		write_.store(0, std::memory_order_relaxed);
		read_.store(0, std::memory_order_relaxed);
		last_write_frames_.store(0, std::memory_order_relaxed);
	}

	// ---- producer side ----

	size_t writableFrames() const
	{
		uint64_t w = write_.load(std::memory_order_relaxed);
		uint64_t r = read_.load(std::memory_order_acquire);
		return capacity_ - (size_t)(w - r);
	}

	/**
	 * Append signed 16-bit interleaved frames, scaled by gain. All or nothing: if they do
	 * not fit, nothing is written and false is returned (the caller counts an overflow;
	 * the consumer's catch-up trims a backlog on its side).
	 */
	bool writeS16(const int16_t *interleaved, size_t frames, float gain = 1.0f)
	{
		if(frames > writableFrames())
			return false;
		const unsigned ch = channels_;
		const float scale = gain / 32768.0f;
		uint64_t w = write_.load(std::memory_order_relaxed);
		for(size_t i = 0; i < frames; i++)
		{
			float *dst = slot(w + i);
			for(unsigned c = 0; c < ch; c++)
				dst[c] = (float)interleaved[i * ch + c] * scale;
		}
		last_write_frames_.store((uint32_t)frames, std::memory_order_relaxed);
		write_.store(w + frames, std::memory_order_release);
		return true;
	}

	/** Like writeS16 for float input (tests). */
	bool writeF32(const float *interleaved, size_t frames)
	{
		if(frames > writableFrames())
			return false;
		const unsigned ch = channels_;
		uint64_t w = write_.load(std::memory_order_relaxed);
		for(size_t i = 0; i < frames; i++)
			memcpy(slot(w + i), interleaved + i * ch, ch * sizeof(float));
		last_write_frames_.store((uint32_t)frames, std::memory_order_relaxed);
		write_.store(w + frames, std::memory_order_release);
		return true;
	}

	/** Size of the producer's last write: how coarsely input arrives (480 frames from the PS5). */
	size_t lastWriteFrames() const { return last_write_frames_.load(std::memory_order_relaxed); }

	// ---- consumer side ----

	size_t readableFrames() const
	{
		uint64_t r = read_.load(std::memory_order_relaxed);
		uint64_t w = write_.load(std::memory_order_acquire);
		return (size_t)(w - r);
	}

	/** Frame `offset` after the read position (0 = oldest); offset < readableFrames(). */
	const float *frame(size_t offset) const
	{
		return slot(read_.load(std::memory_order_relaxed) + offset);
	}

	void consume(size_t frames)
	{
		read_.store(read_.load(std::memory_order_relaxed) + frames, std::memory_order_release);
	}

private:
	float *slot(uint64_t index) { return &buf_[(size_t)(index & (capacity_ - 1)) * max_channels_]; }
	const float *slot(uint64_t index) const { return &buf_[(size_t)(index & (capacity_ - 1)) * max_channels_]; }

	std::vector<float> buf_;
	size_t capacity_ = 0;
	unsigned max_channels_ = 2;
	unsigned channels_ = 2;
	alignas(64) std::atomic<uint64_t> write_{0};
	std::atomic<uint32_t> last_write_frames_{0}; // producer-written, beside write_
	alignas(64) std::atomic<uint64_t> read_{0};
};

} // namespace plk

#endif // PLK_HOST_FRAMERING_H
