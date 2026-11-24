// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "Replay.h"

#include "Log.h"
#include "VideoPath.h"

#include <Dump.h>

#include <chiaki/time.h>

#include <pthread.h>
#include <pthread/qos.h>

#include <algorithm>
#include <chrono>
#include <thread>

namespace plk::app {

namespace {

constexpr uint64_t kMaxPauseUs = 250000;
constexpr uint64_t kPadding = 64; // CHIAKI_VIDEO_BUFFER_PADDING_SIZE: the lib's guarantee to sinks

void sleepUntilMonotonic(uint64_t target_us)
{
	for(;;)
	{
		const uint64_t now = chiaki_time_now_monotonic_us();
		if(now >= target_us)
			return;
		const uint64_t left = target_us - now;
		// Sleep to ~1 ms before, then yield-spin: the feeder stands in for a network thread
		// that wakes on a packet, so a late wake-up would show up as decode latency.
		if(left > 1500)
			std::this_thread::sleep_for(std::chrono::microseconds(left - 1000));
		else
			std::this_thread::yield();
	}
}

} // namespace

Replay::Replay(VideoPath *video, std::string path, bool loop) : video_(video), path_(std::move(path)), loop_(loop) {}

Replay::~Replay()
{
	stop();
	join();
}

bool Replay::load(std::string *error)
{
	std::vector<replay::DumpRecord> raw;
	if(!replay::readDump(path_, raw, error))
		return false;
	uint64_t first = 0;
	bool have_first = false;
	size_t frames = 0;
	records_.clear();
	records_.reserve(raw.size());
	for(auto &r : raw)
	{
		Record rec;
		rec.config = r.isConfig() || r.frameReadyUs == 0;
		rec.recovered = r.recovered();
		rec.frame_index = r.frameIndex;
		rec.frames_lost = r.framesLost;
		if(!rec.config)
		{
			if(!have_first)
			{
				first = r.frameReadyUs;
				have_first = true;
			}
			rec.offset_us = r.frameReadyUs >= first ? r.frameReadyUs - first : 0;
			frames++;
		}
		rec.payload = std::move(r.payload);
		records_.push_back(std::move(rec));
	}
	if(!frames)
	{
		if(error)
			*error = path_ + " holds no video frames";
		return false;
	}
	return true;
}

void Replay::start(std::function<void()> finished)
{
	finished_ = std::move(finished);
	stop_ = false;
	thread_ = std::thread([this] { run(); });
}

void Replay::stop()
{
	stop_ = true;
}

void Replay::join()
{
	if(thread_.joinable() && thread_.get_id() != std::this_thread::get_id())
		thread_.join();
}

void Replay::run()
{
	pthread_setname_np("plk.replay");
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
	uint64_t base = chiaki_time_now_monotonic_us() + 20000; // first frame 20 ms from now
	uint64_t shift = 0;      // pauses cut out of the recording, plus loop lengths
	uint64_t last_offset = 0;
	uint64_t late_sum = 0, late_max = 0, frames = 0, rebases = 0;
	std::vector<uint8_t> buf;
	do
	{
		bool first_of_pass = true;
		for(const Record &r : records_)
		{
			if(stop_.load())
				goto done;
			// A copy with the padding the lib guarantees its sinks after the data.
			buf.assign(r.payload.begin(), r.payload.end());
			const size_t size = buf.size();
			buf.resize(size + kPadding, 0);
			if(r.config)
			{
				video_->onVideoSample(buf.data(), size, r.frame_index, 0, 0, false);
				continue;
			}
			if(first_of_pass)
				first_of_pass = false;
			else if(r.offset_us > last_offset + kMaxPauseUs)
				shift -= (r.offset_us - last_offset) - 16667; // cut the pause to one 60 fps frame
			last_offset = r.offset_us;
			uint64_t due = base + r.offset_us + shift;
			const uint64_t now = chiaki_time_now_monotonic_us();
			if(now > due + 30000)
			{
				// A stall (first decoder session, a slow submit) must not become a burst.
				shift += now - due;
				due = now;
				rebases++;
			}
			sleepUntilMonotonic(due);
			const uint64_t ready = chiaki_time_now_monotonic_us();
			const uint64_t late = ready - due;
			late_sum += late;
			late_max = std::max(late_max, late);
			video_->onVideoSample(buf.data(), size, r.frame_index, ready, r.frames_lost, r.recovered);
			frames++;
			fed_.store(frames);
		}
		// The next pass starts one frame after this one ended.
		shift += last_offset + 16667;
		last_offset = 0;
	} while(loop_ && !stop_.load());
done:
	PLK_LOGI("replay: %llu frames fed, wake-up lateness mean %.2f ms max %.2f ms, %llu schedule re-bases",
			(unsigned long long)frames, frames ? late_sum / 1000.0 / (double)frames : 0.0, late_max / 1000.0,
			(unsigned long long)rebases);
	if(finished_)
		finished_();
}

} // namespace plk::app
