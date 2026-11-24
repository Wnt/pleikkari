// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// FrameRing, Playout and an offline AudioOutput, against a simulated clock: the console
// delivers 480-frame (10 ms) packets by its clock, the "device" renders 128 frames at a
// time by another. Nothing here touches an audio device unless PLK_TEST_COREAUDIO_OPEN=1,
// and even then the unit is only opened, never started.
//
// Click detector: the source is a 440 Hz sine at amplitude 0.5, whose largest
// sample-to-sample step is 2*pi*440/48000*0.5 = 0.029. A fade adds at most 0.5/128 per
// sample and a crossfade 1/128, so any step above 0.05 is a discontinuity.

#include "AudioOutput.h"
#include "FrameRing.h"
#include "Playout.h"
#include "TestHarness.h"

#include <atomic>
#include <cmath>
#include <random>
#include <thread>
#include <vector>

using plk::FrameRing;
using plk::Playout;
using plk::PlayoutConfig;
using plk::PlayoutStats;

namespace {

constexpr double kRate = 48000.0;
constexpr double kClickStep = 0.05;

// ---------------------------------------------------------------- simulation

struct SimOptions
{
	double seconds = 60.0;
	double mismatch = 0.0;            // producer clock vs the consumer's: +0.001 = 0.1 % fast
	size_t io_frames = 128;
	size_t packet_frames = 480;
	double jitter_ms = 0.0;           // each packet late by U(0, jitter_ms), order kept
	double producer_stall_at_s = -1.0;
	double producer_stall_ms = 0.0;
	bool stall_burst = false;         // stalled packets arrive together at the end (else they are lost)
	double consumer_stall_at_s = -1.0;
	double consumer_stall_ms = 0.0;   // no renders at all for this long (the device stopped calling)
	double warmup_s = 10.0;           // the "after warm-up" figures start here
	PlayoutConfig config;
	uint32_t seed = 1;
	bool ramp_source = false;         // exact ramp values instead of the sine (bit-exactness test)
};

struct SimResult
{
	PlayoutStats at_warmup;
	PlayoutStats end;
	double max_step = 0.0;            // largest step anywhere (except across a consumer stall)
	double fill_avg_after_warmup_ms = 0.0;
	double fill_max_after_warmup_ms = 0.0;
	double fill_p99_after_warmup_ms = 0.0;
	double fill_avg_last5s_ms = 0.0;
	uint64_t overflow_packets = 0;
	uint64_t ramp_discontinuities = 0; // ramp_source: full-gain frames that are not previous + 1
	uint64_t ramp_checked = 0;
};

float sineSample(uint64_t n, unsigned channel)
{
	const double phase = 2.0 * M_PI * 440.0 * (double)n / kRate + (channel ? M_PI / 3.0 : 0.0);
	return (float)(0.5 * std::sin(phase));
}

/** Exactly representable, never zero, unique within 65536 frames. */
float rampSample(uint64_t n, unsigned channel)
{
	const float v = (float)((n & 0xffff) + 1) / 131072.0f;
	return channel ? -v : v;
}

SimResult runSim(const SimOptions &o)
{
	FrameRing ring(16384, 2);
	ring.setChannels(2);
	Playout playout(ring, o.config);
	std::mt19937 rng(o.seed);
	std::uniform_real_distribution<double> jitter(0.0, o.jitter_ms / 1000.0);

	const double packet_period = (double)o.packet_frames / (kRate * (1.0 + o.mismatch));
	const double render_period = (double)o.io_frames / kRate;
	const double stall_end = o.producer_stall_at_s + o.producer_stall_ms / 1000.0;
	const double cstall_end = o.consumer_stall_at_s + o.consumer_stall_ms / 1000.0;

	std::vector<float> packet(o.packet_frames * 2);
	std::vector<float> out(o.io_frames * 2);
	SimResult r;

	uint64_t pkt = 0;
	double last_delivery = 0.0;
	double next_delivery = 0.0;
	bool next_lost = false;
	auto schedule = [&]() {
		const double nominal = (double)pkt * packet_period;
		next_lost = false;
		double d = nominal + (o.jitter_ms > 0.0 ? jitter(rng) : 0.0);
		if(o.producer_stall_at_s >= 0.0 && nominal >= o.producer_stall_at_s && nominal < stall_end)
		{
			if(o.stall_burst)
				d = stall_end;
			else
				next_lost = true;
		}
		next_delivery = std::max(d, last_delivery);
	};
	schedule();

	float prev[2] = { 0.0f, 0.0f };
	bool prev_valid = true; // the output starts from silence
	double fill_sum = 0.0, fill_sum_last5 = 0.0;
	uint64_t fill_n = 0, fill_n_last5 = 0;
	std::vector<uint64_t> fill_hist(1000); // 0.1 ms buckets up to 100 ms
	uint64_t ramp_run = 0;
	uint32_t ramp_prev_idx = 0;

	const uint64_t renders = (uint64_t)(o.seconds / render_period);
	bool warm_captured = false;
	for(uint64_t j = 0; j < renders; j++)
	{
		const double t = (double)j * render_period;

		while(next_delivery <= t)
		{
			if(!next_lost)
			{
				const uint64_t base = pkt * o.packet_frames;
				for(size_t i = 0; i < o.packet_frames; i++)
					for(unsigned c = 0; c < 2; c++)
						packet[i * 2 + c] = o.ramp_source ? rampSample(base + i, c) : sineSample(base + i, c);
				if(!ring.writeF32(packet.data(), o.packet_frames))
					r.overflow_packets++;
				last_delivery = next_delivery;
			}
			pkt++;
			schedule();
		}

		if(o.consumer_stall_at_s >= 0.0 && t >= o.consumer_stall_at_s && t < cstall_end)
		{
			prev_valid = false; // the device output itself had a gap here
			continue;
		}

		if(!warm_captured && t >= o.warmup_s)
		{
			r.at_warmup = playout.stats();
			warm_captured = true;
		}

		playout.render(out.data(), o.io_frames);

		for(size_t i = 0; i < o.io_frames; i++)
		{
			for(unsigned c = 0; c < 2; c++)
			{
				const float x = out[i * 2 + c];
				if(prev_valid)
					r.max_step = std::max(r.max_step, (double)std::fabs(x - prev[c]));
				prev[c] = x;
			}
			prev_valid = true;
		}

		if(o.ramp_source)
		{
			// Once the fade-in is over, every frame must be exactly the next input frame.
			for(size_t i = 0; i < o.io_frames; i++)
			{
				const float v = out[i * 2];
				if(v == 0.0f)
				{
					ramp_run = 0;
					continue;
				}
				const uint32_t idx = (uint32_t)std::lround((double)v * 131072.0) - 1;
				if(ramp_run > o.config.fade_frames)
				{
					r.ramp_checked++;
					if(idx != ((ramp_prev_idx + 1) & 0xffff) || out[i * 2 + 1] != -v
						|| v != (float)(idx + 1) / 131072.0f)
						r.ramp_discontinuities++;
				}
				ramp_prev_idx = idx;
				ramp_run++;
			}
		}

		if(t >= o.warmup_s)
		{
			const double f = playout.stats().fill_ms;
			fill_sum += f;
			fill_n++;
			r.fill_max_after_warmup_ms = std::max(r.fill_max_after_warmup_ms, f);
			fill_hist[std::min<size_t>(fill_hist.size() - 1, (size_t)std::max(0.0, f * 10.0))]++;
			if(t >= o.seconds - 5.0)
			{
				fill_sum_last5 += f;
				fill_n_last5++;
			}
		}
	}
	r.end = playout.stats();
	if(!warm_captured)
		r.at_warmup = r.end;
	r.fill_avg_after_warmup_ms = fill_n ? fill_sum / (double)fill_n : 0.0;
	r.fill_avg_last5s_ms = fill_n_last5 ? fill_sum_last5 / (double)fill_n_last5 : 0.0;
	uint64_t acc = 0;
	for(size_t i = 0; i < fill_hist.size(); i++)
	{
		acc += fill_hist[i];
		if(fill_n && acc >= (uint64_t)(0.99 * (double)fill_n))
		{
			r.fill_p99_after_warmup_ms = (double)(i + 1) / 10.0;
			break;
		}
	}
	return r;
}

void noteResult(const char *what, const SimResult &r)
{
	plktest::note("%s: fill after warm-up avg %.2f ms, p99 %.1f ms, max %.2f ms; last 5 s avg %.2f ms; "
		"margin target %.1f ms; drift %.0f ppm; underruns %llu (%llu after warm-up); backlog cuts %llu; "
		"frames dropped %llu; max step %.4f",
		what, r.fill_avg_after_warmup_ms, r.fill_p99_after_warmup_ms, r.fill_max_after_warmup_ms, r.fill_avg_last5s_ms,
		r.end.margin_target_ms, r.end.drift_ppm, (unsigned long long)r.end.underruns,
		(unsigned long long)(r.end.underruns - r.at_warmup.underruns), (unsigned long long)r.end.hard_drops,
		(unsigned long long)r.end.frames_dropped, r.max_step);
}

} // namespace

// ---------------------------------------------------------------- FrameRing

PLK_TEST(ring_basic)
{
	FrameRing ring(1000, 2);
	CHECK(ring.capacity() == 1024);
	ring.setChannels(2);
	CHECK(ring.readableFrames() == 0);
	CHECK(ring.writableFrames() == 1024);

	std::vector<int16_t> pcm(600 * 2);
	for(size_t i = 0; i < pcm.size(); i++)
		pcm[i] = (int16_t)((i & 1) ? -16384 : 16384);
	CHECK(ring.writeS16(pcm.data(), 600));
	CHECK(ring.readableFrames() == 600);
	CHECK(ring.frame(0)[0] == 0.5f);
	CHECK(ring.frame(599)[1] == -0.5f);
	// all or nothing
	CHECK(!ring.writeS16(pcm.data(), 600));
	CHECK(ring.readableFrames() == 600);
	ring.consume(500);
	CHECK(ring.readableFrames() == 100);
	// wraps around the end of the buffer
	CHECK(ring.writeS16(pcm.data(), 600));
	CHECK(ring.readableFrames() == 700);
	CHECK(ring.frame(699)[0] == 0.5f);
	std::vector<int16_t> extreme = { -32768, 32767 };
	ring.consume(700);
	CHECK(ring.writeS16(extreme.data(), 1, 1.0f));
	CHECK(ring.frame(0)[0] == -1.0f);
	CHECK(ring.frame(0)[1] < 1.0f && ring.frame(0)[1] > 0.9999f);
	ring.reset();
	CHECK(ring.readableFrames() == 0);
}

PLK_TEST(ring_spsc_threads)
{
	// One producer, one consumer, ~2M frames through a 512-frame ring: every frame arrives
	// once, in order, with both channels intact.
	FrameRing ring(512, 2);
	ring.setChannels(2);
	constexpr uint64_t kFrames = 2000000;
	std::atomic<bool> producer_done{false};
	std::thread producer([&] {
		std::mt19937 rng(7);
		std::vector<float> chunk(97 * 2);
		uint64_t n = 0;
		while(n < kFrames)
		{
			size_t want = 1 + rng() % 97;
			want = (size_t)std::min<uint64_t>(want, kFrames - n);
			for(size_t i = 0; i < want; i++)
			{
				const float v = (float)((n + i) & 0xfffff);
				chunk[i * 2] = v;
				chunk[i * 2 + 1] = -v;
			}
			if(ring.writeF32(chunk.data(), want))
				n += want;
			else
				std::this_thread::yield();
		}
		producer_done = true;
	});
	uint64_t got = 0, errors = 0;
	std::mt19937 rng(11);
	while(got < kFrames)
	{
		size_t avail = ring.readableFrames();
		if(!avail)
		{
			std::this_thread::yield();
			continue;
		}
		size_t take = std::min<size_t>(avail, 1 + rng() % 131);
		for(size_t i = 0; i < take; i++)
		{
			const float *f = ring.frame(i);
			const float expect = (float)((got + i) & 0xfffff);
			if(f[0] != expect || f[1] != -expect)
				errors++;
		}
		ring.consume(take);
		got += take;
	}
	producer.join();
	CHECK(producer_done.load());
	CHECK(errors == 0);
	CHECK(ring.readableFrames() == 0);
}

// ---------------------------------------------------------------- Playout

PLK_TEST(playout_steady_state)
{
	SimOptions o;
	o.seconds = 30.0;
	o.warmup_s = 5.0;
	SimResult r = runSim(o);
	noteResult("matched clocks", r);
	CHECK(r.end.underruns == 0);
	CHECK(r.end.hard_drops == 0);
	CHECK(r.overflow_packets == 0);
	CHECK(r.end.starts == 1);
	CHECK(r.end.playing);
	CHECK_MSG(r.max_step < kClickStep, "max step %.4f", r.max_step);
	// 3 ms margin + a 2.7 ms render + a 10 ms packet sawtooth: about 10.7 ms on average.
	CHECK_MSG(r.fill_avg_after_warmup_ms < 13.0, "avg fill %.2f ms", r.fill_avg_after_warmup_ms);
	CHECK(std::fabs(r.end.drift_ppm) < 100.0);
}

PLK_TEST(playout_bit_exact_at_unity)
{
	// With the controller off the ratio is exactly 1 and the interpolator must be a copy.
	SimOptions o;
	o.seconds = 10.0;
	o.warmup_s = 1.0;
	o.ramp_source = true;
	o.config.kp = 0.0;
	o.config.ki = 0.0;
	SimResult r = runSim(o);
	CHECK(r.end.underruns == 0);
	CHECK(r.end.hard_drops == 0);
	CHECK(r.end.ratio == 1.0);
	CHECK(r.ramp_checked > 400000);
	CHECK_MSG(r.ramp_discontinuities == 0, "%llu of %llu frames differ", (unsigned long long)r.ramp_discontinuities,
		(unsigned long long)r.ramp_checked);
}

static void clockMismatch(double mismatch)
{
	SimOptions o;
	o.seconds = 180.0;
	o.warmup_s = 30.0;
	o.mismatch = mismatch;
	SimResult r = runSim(o);
	noteResult(mismatch > 0 ? "console clock 0.1 % fast" : "console clock 0.1 % slow", r);
	const uint64_t underruns_after = r.end.underruns - r.at_warmup.underruns;
	const uint64_t drops_after = r.end.hard_drops - r.at_warmup.hard_drops;
	CHECK_MSG(underruns_after == 0, "%llu underruns after warm-up", (unsigned long long)underruns_after);
	CHECK_MSG(drops_after == 0, "%llu backlog cuts after warm-up", (unsigned long long)drops_after);
	CHECK(r.overflow_packets == 0);
	CHECK_MSG(std::fabs(r.end.drift_ppm - mismatch * 1e6) < 150.0, "drift estimate %.0f ppm", r.end.drift_ppm);
	CHECK_MSG(std::fabs(r.end.ratio - (1.0 + mismatch)) < 0.0005, "ratio %.6f", r.end.ratio);
	CHECK_MSG(r.max_step < kClickStep, "max step %.4f", r.max_step);
	CHECK_MSG(r.fill_avg_after_warmup_ms < 14.0, "avg fill %.2f ms", r.fill_avg_after_warmup_ms);
}

PLK_TEST(playout_clock_fast_0_1_percent)
{
	clockMismatch(+0.001);
}

PLK_TEST(playout_clock_slow_0_1_percent)
{
	clockMismatch(-0.001);
}

PLK_TEST(playout_starved_producer_gap)
{
	// The console's audio stops for 200 ms and resumes on time: the stalled packets are lost.
	SimOptions o;
	o.seconds = 20.0;
	o.warmup_s = 5.0;
	o.producer_stall_at_s = 8.0;
	o.producer_stall_ms = 200.0;
	SimResult r = runSim(o);
	noteResult("200 ms gap", r);
	CHECK(r.at_warmup.underruns == 0);
	CHECK_MSG(r.end.underruns == 1, "%llu underruns", (unsigned long long)r.end.underruns);
	CHECK(r.end.starts == 2);
	CHECK(r.end.playing);
	CHECK(r.end.silent_frames - r.at_warmup.silent_frames >= (uint64_t)(0.18 * kRate));
	CHECK_MSG(r.max_step < kClickStep, "max step %.4f", r.max_step);
	CHECK_MSG(r.fill_avg_last5s_ms < 16.0, "avg fill %.2f ms", r.fill_avg_last5s_ms);
}

PLK_TEST(playout_starved_producer_burst)
{
	// The console's audio stops for 200 ms, then everything held back arrives at once: the
	// backlog is skipped instead of played 200 ms late.
	SimOptions o;
	o.seconds = 20.0;
	o.warmup_s = 5.0;
	o.producer_stall_at_s = 8.0;
	o.producer_stall_ms = 200.0;
	o.stall_burst = true;
	SimResult r = runSim(o);
	noteResult("200 ms stall, burst resume", r);
	CHECK_MSG(r.end.underruns == 1, "%llu underruns", (unsigned long long)r.end.underruns);
	CHECK(r.end.starts == 2);
	CHECK(r.end.playing);
	CHECK(r.overflow_packets == 0);
	CHECK_MSG(r.end.frames_dropped - r.at_warmup.frames_dropped >= (uint64_t)(0.15 * kRate), "dropped %llu frames",
		(unsigned long long)(r.end.frames_dropped - r.at_warmup.frames_dropped));
	CHECK_MSG(r.max_step < kClickStep, "max step %.4f", r.max_step);
	CHECK_MSG(r.fill_avg_last5s_ms < 16.0, "avg fill %.2f ms", r.fill_avg_last5s_ms);
}

PLK_TEST(playout_consumer_stall_backlog_cut)
{
	// The device stops calling for 100 ms (a device switch, a stalled IO thread) while the
	// console keeps sending: the 100 ms backlog is cut with a crossfade, without an underrun.
	SimOptions o;
	o.seconds = 20.0;
	o.warmup_s = 5.0;
	o.consumer_stall_at_s = 8.0;
	o.consumer_stall_ms = 100.0;
	SimResult r = runSim(o);
	noteResult("100 ms device stall", r);
	CHECK(r.end.underruns == 0);
	CHECK_MSG(r.end.hard_drops >= 1, "%llu backlog cuts", (unsigned long long)r.end.hard_drops);
	CHECK(r.end.frames_dropped >= (uint64_t)(0.08 * kRate));
	CHECK_MSG(r.max_step < kClickStep, "max step %.4f", r.max_step);
	CHECK_MSG(r.fill_avg_last5s_ms < 14.0, "avg fill %.2f ms", r.fill_avg_last5s_ms);
}

PLK_TEST(playout_jittered_producer)
{
	// Packets up to 8 ms late: the margin target grows on the first underruns, then holds.
	SimOptions o;
	o.seconds = 180.0;
	o.warmup_s = 60.0;
	o.jitter_ms = 8.0;
	SimResult r = runSim(o);
	noteResult("8 ms jitter", r);
	const uint64_t underruns_after = r.end.underruns - r.at_warmup.underruns;
	CHECK_MSG(underruns_after <= 2, "%llu underruns in the last 120 s", (unsigned long long)underruns_after);
	CHECK(r.end.playing);
	CHECK_MSG(r.max_step < kClickStep, "max step %.4f", r.max_step);
	CHECK_MSG(r.fill_avg_after_warmup_ms < 8.0 + 16.0, "avg fill %.2f ms", r.fill_avg_after_warmup_ms);
}

PLK_TEST(playout_flush_decays)
{
	FrameRing ring(4096, 2);
	ring.setChannels(2);
	Playout playout(ring);
	std::vector<float> pkt(480 * 2), out(128 * 2);
	uint64_t n = 0;
	float prev[2] = { 0, 0 };
	double max_step = 0.0;
	auto feed = [&] {
		for(size_t i = 0; i < 480; i++, n++)
			for(unsigned c = 0; c < 2; c++)
				pkt[i * 2 + c] = sineSample(n, c);
		ring.writeF32(pkt.data(), 480);
	};
	auto render = [&] {
		playout.render(out.data(), 128);
		for(size_t i = 0; i < 128; i++)
			for(unsigned c = 0; c < 2; c++)
			{
				max_step = std::max(max_step, (double)std::fabs(out[i * 2 + c] - prev[c]));
				prev[c] = out[i * 2 + c];
			}
	};
	// packet k arrives once k * 480 frames have been rendered, as in the simulation above
	uint64_t rendered = 0, fed = 0;
	auto step = [&] {
		while(fed <= rendered)
		{
			feed();
			fed += 480;
		}
		render();
		rendered += 128;
	};
	for(int k = 0; k < 40; k++)
		step();
	CHECK(playout.stats().playing);
	playout.requestFlush();
	render(); // a new stream: the old audio decays instead of stopping dead
	rendered += 128;
	CHECK(playout.stats().flushes == 1);
	CHECK(!playout.stats().playing);
	CHECK(ring.readableFrames() == 0);
	for(int k = 0; k < 40; k++)
		step();
	CHECK(playout.stats().playing);
	CHECK(playout.stats().starts == 2);
	CHECK(playout.stats().underruns == 0);
	CHECK_MSG(max_step < kClickStep, "max step %.4f", max_step);
}

// ---------------------------------------------------------------- AudioOutput

PLK_TEST(audio_output_offline)
{
	plk::AudioOutput out;
	REQUIRE(out.openOffline(2, 48000));
	// The library announces the format it already has: a flush, and a start of the (offline) output.
	out.onAudioFormat(2, 48000);
	plk::AudioOutputStats s = out.stats();
	CHECK(s.open && s.running);
	CHECK(s.channels == 2 && s.rate == 48000);
	CHECK(s.format_changes == 1);

	auto renderOut = [&](std::vector<float> &buf, UInt32 frames, UInt32 channels, AudioUnitRenderActionFlags &flags) {
		buf.assign(frames * channels, 1.0f);
		AudioBufferList list;
		list.mNumberBuffers = 1;
		list.mBuffers[0].mNumberChannels = channels;
		list.mBuffers[0].mDataByteSize = frames * channels * sizeof(float);
		list.mBuffers[0].mData = buf.data();
		flags = 0;
		return plk::AudioOutput::renderCallback(&out, &flags, nullptr, 0, frames, &list);
	};

	std::vector<float> buf;
	AudioUnitRenderActionFlags flags = 0;
	// Nothing queued: silence, flagged as such.
	CHECK(renderOut(buf, 128, 2, flags) == noErr);
	CHECK(flags & kAudioUnitRenderAction_OutputIsSilence);
	CHECK(buf[0] == 0.0f && buf[255] == 0.0f);

	std::vector<int16_t> pcm(480 * 2);
	uint64_t n = 0;
	auto deliver = [&] {
		for(size_t i = 0; i < 480; i++, n++)
			for(unsigned c = 0; c < 2; c++)
				pcm[i * 2 + c] = (int16_t)(sineSample(n, c) * 32767.0f);
		out.onPcm(pcm.data(), 480);
	};
	// packet k arrives once k * 480 frames have been rendered
	uint64_t rendered = 0, delivered = 0;
	double energy = 0.0;
	for(int k = 0; k < 40; k++)
	{
		while(delivered <= rendered)
		{
			deliver();
			delivered += 480;
		}
		CHECK(renderOut(buf, 128, 2, flags) == noErr);
		for(float x : buf)
			energy += (double)x * x;
		rendered += 128;
	}
	CHECK(energy > 1.0);
	s = out.stats();
	CHECK(s.pcm_frames_in == delivered);
	CHECK(s.overflow_events == 0);
	CHECK(s.playout.starts == 1);
	CHECK(s.playout.underruns == 0);
	CHECK(s.render_errors == 0);

	// A layout we did not ask for: silence and a counted error, never a crash.
	CHECK(renderOut(buf, 128, 1, flags) == noErr);
	CHECK(flags & kAudioUnitRenderAction_OutputIsSilence);
	CHECK(buf[0] == 0.0f && buf[127] == 0.0f);
	CHECK(out.stats().render_errors == 1);

	// Overflow: more than the 16384-frame ring holds with nothing rendering.
	for(int k = 0; k < 40; k++)
		deliver();
	s = out.stats();
	CHECK(s.overflow_events > 0);
	CHECK(s.overflow_frames == s.overflow_events * 480);

	// A different format re-opens (offline stays offline).
	out.onAudioFormat(1, 48000);
	s = out.stats();
	CHECK(s.channels == 1);
	CHECK(s.format_changes == 2);
	CHECK(s.open && s.running);
	CHECK(renderOut(buf, 128, 1, flags) == noErr);
	CHECK(out.stats().render_errors == 1);

	out.close();
	s = out.stats();
	CHECK(!s.open && !s.running);
	// After close the producer is refused, not crashed into.
	deliver();
	CHECK(out.stats().pcm_frames_in == s.pcm_frames_in);
}

PLK_TEST(audio_output_coreaudio_open)
{
	if(!plktest::envFlag("PLK_TEST_COREAUDIO_OPEN"))
		SKIP("set PLK_TEST_COREAUDIO_OPEN=1 to open (never start) the default output device");
	plk::AudioOutputConfig config;
	config.preferred_io_frames = 0; // leave the device's IO size alone in a test
	plk::AudioOutput out(config);
	std::string error;
	REQUIRE(out.open(2, 48000, &error));
	plk::AudioOutputStats s = out.stats();
	CHECK(s.open);
	CHECK(!s.running);
	CHECK(s.latency.device != kAudioObjectUnknown);
	CHECK(s.latency.device_rate > 0.0);
	CHECK(s.latency.io_buffer_frames > 0);
	CHECK(s.latency.total_ms > 0.0);
	plktest::note("default output: %.0f Hz, IO buffer %u, safety offset %u, device latency %u, stream latency %u frames, "
		"AU latency %.3f ms: %.2f ms after the ring",
		s.latency.device_rate, s.latency.io_buffer_frames, s.latency.safety_offset_frames, s.latency.device_latency_frames,
		s.latency.stream_latency_frames, s.latency.unit_latency_s * 1000.0, s.latency.total_ms);
	out.close();
	CHECK(!out.stats().open);
}
