// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// The consumer side of audio playout: everything the CoreAudio render callback does,
// kept free of CoreAudio so it runs against a fake clock in the unit tests.
//
// Two clocks meet here: the console produces 48 kHz audio by its clock, the Mac's
// output device consumes it by its own, and they differ by tens of ppm. Packets also
// arrive in 10 ms lumps with network jitter. Playout keeps the ring as empty as it can
// without running dry:
//
//   - margin: the frames left in the ring after a render, minimum over a ~200 ms window
//     (the low-water mark). The controller holds it at a target that starts at 3 ms,
//     grows by 3 ms on every underrun and shrinks by 0.5 ms after 20 s without a near miss.
//   - drift: a PI controller on (low-water - target) sets the resampling ratio, at most
//     +-0.5 % (8.6 cents) away from 1. The integral term settles at the clock mismatch.
//     The resampler is a 4-point Catmull-Rom interpolator; at ratio 1 it is a bit-exact copy.
//   - backlog: an excess beyond 20 ms (a burst after a network stall) is cut at once,
//     crossfading over 128 frames, instead of being played out late.
//   - underrun: the frames still there are played, then the last output sample decays
//     linearly to zero over 128 frames (the waveform bends, it does not jump), silence
//     until the ring holds the (raised) target again, then a fade-in. A flush ends the
//     same way. No clicks either way.
//   - start: input arrives in chunks (the producer's last write size, 480 frames from the
//     PS5), so playback starts once the ring covers every render until the next chunk can
//     arrive plus the margin; anything beyond that is skipped, not played late.
//
// Thread safety: render(), requestFlush() handling and the controller run on the consumer
// thread only. requestFlush(), setGain() and stats() may be called from any thread.

#ifndef PLK_HOST_PLAYOUT_H
#define PLK_HOST_PLAYOUT_H

#include "FrameRing.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace plk {

struct PlayoutConfig
{
	double sample_rate = 48000.0;      // the stream's rate (the ring's frames)
	double initial_margin_ms = 3.0;
	double min_margin_ms = 1.0;
	double max_margin_ms = 60.0;
	double underrun_margin_step_ms = 3.0;
	double margin_decay_step_ms = 0.5;
	double margin_decay_after_s = 20.0; // clean time before the target shrinks by one step
	double block_ms = 100.0;            // controller update period; low-water window is 2 blocks
	double kp = 0.5;                    // ratio adjustment per second of excess
	double ki = 0.1;                    // per second^2
	double max_ratio_adjust = 0.005;    // +-0.5 %
	double max_drift_adjust = 0.002;    // integral clamp, +-2000 ppm
	double hard_drop_ms = 20.0;         // excess over the target that is cut instead of resampled away
	unsigned fade_frames = 128;         // fade-in, fade-out and crossfade length
};

constexpr size_t kFillHistogramBuckets = 41; // 1 ms buckets of fill before a render; the last is >= 40 ms

struct PlayoutStats
{
	uint64_t render_calls = 0;
	uint64_t frames_rendered = 0;      // output frames produced, audio or silence
	uint64_t silent_frames = 0;        // output frames of silence while (re)buffering
	uint64_t underruns = 0;            // renders that ran out of input
	uint64_t starts = 0;               // transitions from buffering to playing
	uint64_t hard_drops = 0;           // backlog cuts
	uint64_t frames_dropped = 0;       // input frames skipped by backlog cuts and start-up trims
	uint64_t flushes = 0;
	double ratio = 1.0;                // input frames consumed per output frame
	double drift_ppm = 0.0;            // integral term: the estimated clock mismatch
	double margin_target_ms = 0.0;
	double low_water_ms = 0.0;         // last controller window's minimum margin
	double fill_ms = 0.0;              // ring fill before the last render
	double avg_fill_ms = 0.0;          // exponential average of fill_ms (~1 s)
	bool playing = false;
	std::array<uint64_t, kFillHistogramBuckets> fill_histogram{};
};

class Playout
{
public:
	Playout(FrameRing &ring, const PlayoutConfig &config = PlayoutConfig());

	Playout(const Playout &) = delete;
	Playout &operator=(const Playout &) = delete;

	/**
	 * Consumer thread. Writes `frames` interleaved frames of ring.channels() samples.
	 * @return false if the whole buffer is silence
	 */
	bool render(float *out, size_t frames);

	/**
	 * Forget the playout state (not the ring). Only while render() cannot run.
	 * @param keep_drift keep the clock-mismatch estimate (same output device)
	 */
	void reset(bool keep_drift);

	/** Any thread: the next render() discards everything in the ring and rebuffers. */
	void requestFlush() { flush_requested_.store(true, std::memory_order_release); }

	void setGain(float gain) { gain_.store(gain, std::memory_order_relaxed); }

	/** Any thread. Counters are individually consistent, not as a set. */
	PlayoutStats stats() const;

	const PlayoutConfig &config() const { return config_; }

private:
	enum class State { Buffering, Playing };

	static constexpr unsigned kTailChannels = 8;

	void enterBuffering();
	void endBlock();
	void recordFill(double fill_frames);
	void startTail(const float *from, unsigned channels);
	size_t writeTail(float *out, size_t frames, unsigned channels, unsigned fade);

	FrameRing &ring_;
	const PlayoutConfig config_;

	// consumer-thread state
	State state_ = State::Buffering;
	double phase_ = 1.0;            // read position relative to the ring head; frame 0 is history
	double ratio_ = 1.0;
	double integ_ = 0.0;
	double margin_frames_ = 0.0;
	double block_frames_ = 0.0;
	double block_elapsed_ = 0.0;
	double block_min_ = 0.0;
	double prev_block_min_ = 0.0;
	double hard_drop_frames_ = 0.0;
	unsigned clean_blocks_ = 0;
	unsigned decay_blocks_ = 0;
	unsigned fade_in_left_ = 0;
	unsigned xfade_left_ = 0;
	double xfade_offset_ = 0.0;
	double drop_pending_ = 0.0;
	bool drop_in_window_ = false;
	double avg_fill_ = 0.0;
	bool avg_fill_valid_ = false;
	unsigned tail_left_ = 0;                          // decay frames still to write
	std::array<float, kTailChannels> tail_value_{};   // the sample the decay starts from
	std::array<float, kTailChannels> last_out_{};     // last frame a playing render wrote

	// shared
	std::atomic<bool> flush_requested_{false};
	std::atomic<float> gain_{1.0f};

	std::atomic<uint64_t> st_render_calls_{0};
	std::atomic<uint64_t> st_frames_rendered_{0};
	std::atomic<uint64_t> st_silent_frames_{0};
	std::atomic<uint64_t> st_underruns_{0};
	std::atomic<uint64_t> st_starts_{0};
	std::atomic<uint64_t> st_hard_drops_{0};
	std::atomic<uint64_t> st_frames_dropped_{0};
	std::atomic<uint64_t> st_flushes_{0};
	std::atomic<double> st_ratio_{1.0};
	std::atomic<double> st_drift_{0.0};
	std::atomic<double> st_margin_ms_{0.0};
	std::atomic<double> st_low_water_ms_{0.0};
	std::atomic<double> st_fill_ms_{0.0};
	std::atomic<double> st_avg_fill_ms_{0.0};
	std::atomic<bool> st_playing_{false};
	std::array<std::atomic<uint64_t>, kFillHistogramBuckets> st_fill_hist_{};
};

} // namespace plk

#endif // PLK_HOST_PLAYOUT_H
