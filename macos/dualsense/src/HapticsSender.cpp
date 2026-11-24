// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "HapticsSender.h"
#include "Protocol.h"
#include "ThreadUtil.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

namespace plk::dualsense {

using namespace protocol;

namespace {

// qEnvironmentVariableIntValue() semantics: base auto-detected, whole string must parse.
bool EnvInt(const char *name, int *value)
{
	const char *s = getenv(name);
	if(!s || !*s)
		return false;
	char *end = nullptr;
	long v = strtol(s, &end, 0);
	if(*end)
		return false;
	*value = (int)v;
	return true;
}

int EnvIntClamped(const char *name, int def, int min, int max)
{
	int v;
	if(!EnvInt(name, &v))
		return def;
	return std::clamp(v, min, max);
}

constexpr int64_t kNoNonSilent = std::numeric_limits<int64_t>::min() / 2;

int64_t SteadyMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

HapticsConfig HapticsConfig::FromEnvironment()
{
	HapticsConfig c;
	int single = 0;
	c.double_block = !(EnvInt("CHIAKI_BT_HAPTICS_SINGLE", &single) && single != 0);
	c.controller_buffer = (uint8_t)EnvIntClamped("CHIAKI_BT_HAPTICS_CTRL_BUFFER", 48, 16, 128);
	c.prefill_ms = EnvIntClamped("CHIAKI_BT_HAPTICS_PREFILL_MS", 32, 0, 500);
	c.max_ms = EnvIntClamped("CHIAKI_BT_HAPTICS_MAX_MS", 96, 0, 1000);
	return c;
}

// ---------------------------------------------------------------------------

size_t HapticsPacer::MsToBytes(int ms)
{
	size_t frames = ((size_t)std::max(ms, 0) * kHapticsSampleRate + 999) / 1000;
	return frames * kHapticsFrameBytes;
}

HapticsPacer::HapticsPacer(const HapticsConfig &config)
{
	report_bytes_ = config.double_block ? 2 * kHapticsBlockSize : kHapticsBlockSize;
	prefill_bytes_ = MsToBytes(std::clamp(config.prefill_ms, 0, 500));
	max_bytes_ = std::max(MsToBytes(std::clamp(config.max_ms, 0, 1000)), prefill_bytes_ + report_bytes_);
	const int64_t frames_per_report = (int64_t)(report_bytes_ / kHapticsFrameBytes);
	period_ = frames_per_report * 1000000000LL / kHapticsSampleRate;
	prefill_wait_ = period_ * (Nanos)((prefill_bytes_ + report_bytes_ - 1) / report_bytes_ + 1);
}

void HapticsPacer::Push(const uint8_t *data, size_t size)
{
	queue_.insert(queue_.end(), data, data + size);
}

// Same decisions, in the same order, as DualSenseBtHaptics::SenderLoop():
//   not playing: wait for data, then up to prefill_wait for the prefill, then start
//   playing: trim a backlog above max to the prefill depth, stop on an empty queue,
//            otherwise send up to one report of samples, then wait one period.
HapticsPacer::Step HapticsPacer::Next(Nanos now, uint8_t *block)
{
	for(;;)
	{
		switch(state_)
		{
			case State::Idle:
				if(queue_.empty())
					return { Step::WaitForData, 0 };
				state_ = State::Prefill;
				prefill_deadline_ = now + prefill_wait_;
				[[fallthrough]];
			case State::Prefill:
				if(queue_.size() < prefill_bytes_ && now < prefill_deadline_)
					return { Step::WaitUntil, prefill_deadline_ };
				state_ = State::Playing;
				next_ = now;
				[[fallthrough]];
			case State::Playing:
				break;
		}
		if(now < next_)
			return { Step::WaitUntil, next_ };

		if(queue_.size() > max_bytes_)
		{
			size_t drop = queue_.size() - prefill_bytes_;
			drop -= drop % kHapticsFrameBytes;
			queue_.erase(queue_.begin(), queue_.begin() + (std::ptrdiff_t)drop);
			dropped_bytes_ += drop;
		}
		if(queue_.empty())
		{
			// Ran dry: stop sending and rebuild the jitter buffer when data returns.
			state_ = State::Idle;
			underruns_++;
			continue;
		}
		const size_t n = std::min(report_bytes_, queue_.size());
		std::copy(queue_.begin(), queue_.begin() + (std::ptrdiff_t)n, block);
		std::fill(block + n, block + report_bytes_, 0);
		queue_.erase(queue_.begin(), queue_.begin() + (std::ptrdiff_t)n);
		return { Step::Send, now };
	}
}

void HapticsPacer::Sent(Nanos now)
{
	next_ += period_;
	if(now > next_ + period_ * 4)
		next_ = now; // fell far behind (e.g. a stalled write): resync instead of bursting
}

// ---------------------------------------------------------------------------

HapticsSender::HapticsSender(OutputSink *sink, const HapticsConfig &config, bool time_constraint)
	: sink_(sink), config_(config), time_constraint_(time_constraint), pacer_(config), last_nonsilent_ms_(kNoNonSilent)
{
}

HapticsSender::~HapticsSender()
{
	Stop();
}

HapticsPacer::Nanos HapticsSender::Now()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

void HapticsSender::Start()
{
	if(thread_.joinable())
		return;
	started_ = true;
	thread_ = std::thread([this] { Run(); });
}

void HapticsSender::Stop()
{
	{
		std::lock_guard<std::mutex> lk(mutex_);
		stop_ = true;
	}
	cond_.notify_all();
	if(thread_.joinable())
		thread_.join();
	started_ = false;
}

bool HapticsSender::running() const
{
	return started_ && !failed_;
}

void HapticsSender::Push(const int8_t *interleaved, size_t bytes)
{
	if(!running() || !bytes)
		return;
	{
		std::lock_guard<std::mutex> lk(mutex_);
		pacer_.Push(reinterpret_cast<const uint8_t *>(interleaved), bytes);
	}
	cond_.notify_one();
}

void HapticsSender::PushPcm16(const int16_t *interleaved, size_t samples, float gain)
{
	if(!running() || !samples)
		return;
	// StreamSession::PushHapticsFrame(): 16-bit -> 8-bit, both at the console's 3 kHz.
	int8_t stack_buf[256];
	std::vector<int8_t> heap_buf;
	int8_t *out = stack_buf;
	if(samples > sizeof(stack_buf))
	{
		heap_buf.resize(samples);
		out = heap_buf.data();
	}
	bool nonsilent = false;
	for(size_t i = 0; i < samples; i++)
	{
		int32_t adjusted = static_cast<int32_t>(interleaved[i] * gain) / 256;
		adjusted = std::clamp(adjusted, (int32_t)INT8_MIN, (int32_t)INT8_MAX);
		if(adjusted)
			nonsilent = true;
		out[i] = (int8_t)adjusted;
	}
	if(nonsilent)
		last_nonsilent_ms_ = SteadyMs();
	Push(out, samples);
}

bool HapticsSender::RecentlyNonSilent(uint32_t window_ms) const
{
	return SteadyMs() - last_nonsilent_ms_.load() < (int64_t)window_ms;
}

HapticsStats HapticsSender::stats() const
{
	HapticsStats s;
	{
		std::lock_guard<std::mutex> lk(mutex_);
		s.underruns = pacer_.underruns();
		s.dropped_bytes = pacer_.dropped_bytes();
	}
	s.reports = reports_;
	s.write_failures = write_failures_;
	s.failed = failed_;
	return s;
}

void HapticsSender::Run()
{
	SetupDriverThread("plk.ds.haptics", time_constraint_, (uint64_t)pacer_.period(), kHapticsThreadComputationNs,
		kHapticsThreadConstraintNs);
	std::vector<uint8_t> block(pacer_.report_bytes());
	std::unique_lock<std::mutex> lk(mutex_);
	while(!stop_)
	{
		const HapticsPacer::Step step = pacer_.Next(Now(), block.data());
		switch(step.kind)
		{
			case HapticsPacer::Step::WaitForData:
				cond_.wait(lk, [this] { return stop_ || pacer_.queued() > 0; });
				break;
			case HapticsPacer::Step::WaitUntil: {
				const auto deadline = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(step.deadline));
				if(pacer_.Prefilling())
					cond_.wait_until(lk, deadline, [this] { return stop_ || pacer_.PrefillReached(); });
				else
					cond_.wait_until(lk, deadline, [this] { return stop_; });
				break;
			}
			case HapticsPacer::Step::Send: {
				lk.unlock();
				const bool ok = SendReport(block.data(), block.size());
				lk.lock();
				if(!ok && failed_)
					return;
				pacer_.Sent(Now());
				break;
			}
		}
	}
}

bool HapticsSender::SendReport(const uint8_t *samples, size_t size)
{
	uint8_t report[kBtHapticsSize];
	PackHapticsReport(seq_, packet_counter_, config_.controller_buffer, config_.double_block,
		samples, size, report);
	seq_ = (seq_ + 1) & 0x0F;
	packet_counter_ += config_.double_block ? 2 : 1;
	if(!sink_->Write(report, sizeof(report)))
	{
		write_failures_++;
		if(++consecutive_failures_ >= kMaxConsecutiveFailures)
			failed_ = true;
		return false;
	}
	consecutive_failures_ = 0;
	reports_++;
	return true;
}

} // namespace plk::dualsense
