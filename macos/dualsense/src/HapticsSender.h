// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Bluetooth haptics: the console's 3 kHz stereo haptics stream sent to the DualSense
// as HID output report 0x32. A port of the Qt GUI's DualSenseBtHaptics
// (gui/src/dualsensebthaptics.cpp) without Qt or SDL, with the same behaviour and
// tuning: the console's haptics arrive in network-paced bursts (30 frames every
// 10 ms), the controller plays them at a fixed rate, so a paced sender drains a small
// jitter buffer on a steady clock and bounds latency by dropping the oldest data.
//
// The pacing decisions live in HapticsPacer, a state machine driven by explicit
// timestamps so it can be tested against a fake clock; HapticsSender is the thread
// that runs it against the real clock and an OutputSink.
//
// Where the latency goes with the defaults (double block, 32 ms prefill, 96 ms cap):
//  * Prefill: playback starts once 96 frames are queued. With 30-frame packets that
//    is the 4th packet, so the first sample has waited ~30 ms before it is sent.
//  * Packetisation: one report carries 64 frames = 21.33 ms; its newest sample is
//    sent with the oldest, which waited up to one report period longer.
//  * Steady state: arrival and drain rates are equal (3 frames/ms), so the depth
//    stays where the start left it: ~90..120 frames queued before each send, ~72 on
//    average, i.e. a sample waits ~24 ms on average (10..40 ms) in the host queue,
//    plus the controller's own buffer (controller_buffer = 48).
//  * Nothing pulls the depth back down. A network burst that delivers 50 ms of
//    packets late but complete raises the depth by 50 ms for good; only crossing the
//    96 ms cap (a jump back to the 32 ms prefill depth) or an underrun (a 32 ms+
//    re-prefill gap) resets it. Clock drift between console and host (~100 ppm)
//    walks the depth too, ~6 ms per minute.
//  * In the Qt GUI every packet also crosses a queued Qt signal to the sender's
//    QThread; here PushHaptics*() writes the queue directly from the caller.
// What I would change, measured on the device before becoming a default:
//  1. Hold a target depth instead of only the hard limits: when the depth stays
//     above target + one packet for a few reports, drop a frame or two per report
//     (inaudible in a haptics signal) until it is back at target. This removes the
//     permanent latency step after a burst, which is the largest avoidable delay.
//  2. Size the prefill from measured arrival jitter (p99 inter-packet gap) instead
//     of 32 ms; on a wired LAN one report period plus a few ms covers it, ~10 ms
//     less latency on every start.
//  3. Single-block reports (32 frames, 10.67 ms) halve the packetisation delay at
//     twice the report rate (94/s instead of 47/s, ~13 kB/s); worth an A/B against
//     input report jitter on the same Bluetooth link.
// None of these is enabled; HapticsConfig defaults reproduce the GUI's behaviour.

#pragma once

#include "DualSenseHID.h"
#include "EffectsWriter.h" // OutputSink

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

namespace plk::dualsense {

class HapticsPacer {
public:
	using Nanos = int64_t;

	struct Step {
		enum Kind { WaitForData, WaitUntil, Send } kind;
		Nanos deadline; // WaitUntil: wake at this time
	};

	explicit HapticsPacer(const HapticsConfig &config);

	// Interleaved stereo s8 bytes. Caller serialises with Next()/Sent().
	void Push(const uint8_t *data, size_t size);

	// What to do at `now`. For Send, `block` (report_bytes() long) receives the next
	// samples, zero padded; the caller writes the report and then calls Sent().
	Step Next(Nanos now, uint8_t *block);
	// After the write returned (successful or not): schedule the next report.
	void Sent(Nanos now);

	// While waiting for the prefill, a push that completes it should wake the sender.
	bool Prefilling() const { return state_ == State::Prefill; }
	bool PrefillReached() const { return queue_.size() >= prefill_bytes_; }
	size_t queued() const { return queue_.size(); }

	size_t report_bytes() const { return report_bytes_; }
	size_t prefill_bytes() const { return prefill_bytes_; }
	size_t max_bytes() const { return max_bytes_; }
	Nanos period() const { return period_; }
	Nanos prefill_wait() const { return prefill_wait_; }
	uint64_t underruns() const { return underruns_; }
	uint64_t dropped_bytes() const { return dropped_bytes_; }

	static size_t MsToBytes(int ms);

private:
	enum class State { Idle, Prefill, Playing };

	size_t report_bytes_;
	size_t prefill_bytes_;
	size_t max_bytes_;
	Nanos period_;
	Nanos prefill_wait_;

	State state_ = State::Idle;
	Nanos prefill_deadline_ = 0;
	Nanos next_ = 0;
	std::deque<uint8_t> queue_;
	uint64_t underruns_ = 0;
	uint64_t dropped_bytes_ = 0;
};

class HapticsSender {
public:
	HapticsSender(OutputSink *sink, const HapticsConfig &config);
	~HapticsSender();

	void Start();
	void Stop();
	bool running() const; // started and not failed

	void Push(const int8_t *interleaved, size_t bytes);
	void PushPcm16(const int16_t *interleaved, size_t samples, float gain);
	bool RecentlyNonSilent(uint32_t window_ms) const;

	HapticsStats stats() const;

	static constexpr int kMaxConsecutiveFailures = 50;

private:
	void Run();
	bool SendReport(const uint8_t *samples, size_t size);
	static HapticsPacer::Nanos Now();

	OutputSink *const sink_;
	const HapticsConfig config_;

	mutable std::mutex mutex_;
	std::condition_variable cond_;
	HapticsPacer pacer_;
	bool stop_ = false;
	std::thread thread_;
	std::atomic<bool> started_{false};
	std::atomic<bool> failed_{false};
	std::atomic<int64_t> last_nonsilent_ms_;

	// sender thread only
	uint8_t seq_ = 0;
	uint8_t packet_counter_ = 0;
	int consecutive_failures_ = 0;
	std::atomic<uint64_t> reports_{0};
	std::atomic<uint64_t> write_failures_{0};
};

} // namespace plk::dualsense
