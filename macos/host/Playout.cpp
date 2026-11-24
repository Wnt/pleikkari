// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "Playout.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace plk {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

/** 4-point Catmull-Rom between x1 (t = 0) and x2 (t = 1). Returns x1 exactly at t = 0. */
inline float catmullRom(float x0, float x1, float x2, float x3, float t)
{
	return x1 + 0.5f * t * (x2 - x0 + t * (2.0f * x0 - 5.0f * x1 + 4.0f * x2 - x3 + t * (3.0f * (x1 - x2) + x3 - x0)));
}

inline double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

} // namespace

Playout::Playout(FrameRing &ring, const PlayoutConfig &config)
	: ring_(ring), config_(config)
{
	const double fpms = config_.sample_rate / 1000.0;
	margin_frames_ = config_.initial_margin_ms * fpms;
	block_frames_ = std::max(1.0, config_.block_ms * fpms);
	hard_drop_frames_ = config_.hard_drop_ms * fpms;
	decay_blocks_ = (unsigned)std::max(1.0, std::ceil(config_.margin_decay_after_s * 1000.0 / config_.block_ms));
	block_min_ = kInf;
	prev_block_min_ = kInf;
	st_margin_ms_.store(config_.initial_margin_ms, std::memory_order_relaxed);
}

void Playout::enterBuffering()
{
	state_ = State::Buffering;
	phase_ = 1.0;
	fade_in_left_ = 0;
	xfade_left_ = 0;
	xfade_offset_ = 0.0;
	drop_pending_ = 0.0;
	drop_in_window_ = false;
	block_elapsed_ = 0.0;
	block_min_ = kInf;
	prev_block_min_ = kInf;
	clean_blocks_ = 0;
	ratio_ = 1.0 + integ_; // keep the drift estimate, drop the proportional part
	st_playing_.store(false, std::memory_order_relaxed);
}

void Playout::reset(bool keep_drift)
{
	if(!keep_drift)
		integ_ = 0.0;
	enterBuffering();
	tail_left_ = 0;
	last_out_.fill(0.0f);
	avg_fill_valid_ = false;
	st_ratio_.store(ratio_, std::memory_order_relaxed);
	st_drift_.store(integ_ * 1e6, std::memory_order_relaxed);
}

void Playout::recordFill(double fill_frames)
{
	const double ms = fill_frames * 1000.0 / config_.sample_rate;
	st_fill_ms_.store(ms, std::memory_order_relaxed);
	size_t bucket = ms <= 0.0 ? 0 : std::min((size_t)ms, kFillHistogramBuckets - 1);
	st_fill_hist_[bucket].fetch_add(1, std::memory_order_relaxed);
}

void Playout::startTail(const float *from, unsigned channels)
{
	for(unsigned c = 0; c < kTailChannels; c++)
		tail_value_[c] = c < channels ? from[c] : 0.0f;
	tail_left_ = std::max(1u, config_.fade_frames);
}

size_t Playout::writeTail(float *out, size_t frames, unsigned channels, unsigned fade)
{
	const size_t n = std::min<size_t>(tail_left_, frames);
	for(size_t j = 0; j < n; j++)
	{
		const float g = (float)tail_left_ / (float)(fade + 1);
		for(unsigned c = 0; c < channels; c++)
			out[j * channels + c] = c < kTailChannels ? tail_value_[c] * g : 0.0f;
		tail_left_--;
	}
	return n;
}

bool Playout::render(float *out, size_t frames)
{
	if(frames == 0)
		return false;
	const unsigned ch = ring_.channels();
	const unsigned fade = std::max(1u, config_.fade_frames);
	const float gain = gain_.load(std::memory_order_relaxed);

	st_render_calls_.fetch_add(1, std::memory_order_relaxed);
	st_frames_rendered_.fetch_add(frames, std::memory_order_relaxed);

	if(flush_requested_.exchange(false, std::memory_order_acq_rel))
	{
		ring_.consume(ring_.readableFrames());
		if(state_ == State::Playing)
			startTail(last_out_.data(), ch);
		enterBuffering();
		st_flushes_.fetch_add(1, std::memory_order_relaxed);
	}

	size_t avail = ring_.readableFrames();

	if(state_ == State::Buffering)
	{
		// Finish a decay before starting again: a fade-in from zero must start at zero.
		const size_t tail = tail_left_ ? writeTail(out, frames, ch, fade) : 0;
		// Input arrives in chunks (480 frames from the PS5), so starting needs enough for every
		// render until the next chunk can arrive, with the margin left over just before it does:
		// one history frame + those renders + the interpolator's two frames of look-ahead + the margin.
		const double chunk = (double)ring_.lastWriteFrames();
		const double cover = (double)frames * std::max(1.0, std::ceil(chunk / (double)frames));
		const double need = 1.0 + cover * ratio_ + 2.0 + margin_frames_;
		if(tail || (double)avail < need)
		{
			std::fill(out + tail * ch, out + frames * ch, 0.0f);
			st_silent_frames_.fetch_add(frames - tail, std::memory_order_relaxed);
			return tail > 0;
		}
		// Coming out of silence nothing needs a crossfade, so a backlog is simply skipped.
		size_t excess = (size_t)((double)avail - need);
		if(excess)
		{
			ring_.consume(excess);
			avail -= excess;
			st_frames_dropped_.fetch_add(excess, std::memory_order_relaxed);
		}
		state_ = State::Playing;
		phase_ = 1.0;
		fade_in_left_ = fade;
		block_elapsed_ = 0.0;
		block_min_ = kInf;
		prev_block_min_ = kInf;
		st_starts_.fetch_add(1, std::memory_order_relaxed);
		st_playing_.store(true, std::memory_order_relaxed);
	}

	const double fill = (double)avail - phase_;
	recordFill(fill);
	{
		const double ms = fill * 1000.0 / config_.sample_rate;
		const double alpha = std::min(1.0, (double)frames / config_.sample_rate);
		avg_fill_ = avg_fill_valid_ ? avg_fill_ + (ms - avg_fill_) * alpha : ms;
		avg_fill_valid_ = true;
		st_avg_fill_ms_.store(avg_fill_, std::memory_order_relaxed);
	}

	// Start a backlog cut the controller asked for, sized to what is in the ring now.
	if(drop_pending_ > 0.0 && xfade_left_ == 0)
	{
		const double room = (double)avail - (phase_ + (double)(frames + fade) * ratio_ + 4.0);
		const double k = std::floor(std::min(drop_pending_, room));
		drop_pending_ = 0.0;
		if(k >= (double)fade)
		{
			xfade_offset_ = k;
			xfade_left_ = fade;
			drop_in_window_ = true;
		}
	}

	// Does this render have all the input it needs (both heads during a crossfade)?
	const double extra = xfade_left_ ? xfade_offset_ : 0.0;
	const bool underrun = std::floor(phase_ + (double)(frames - 1) * ratio_ + extra) + 2.0 >= (double)avail;

	size_t produce = frames;
	if(underrun)
	{
		xfade_left_ = 0;
		xfade_offset_ = 0.0;
		produce = 0;
		while(produce < frames && std::floor(phase_ + (double)produce * ratio_) + 2.0 < (double)avail)
			produce++;
	}

	double pos = phase_;
	for(size_t j = 0; j < produce; j++)
	{
		float g = gain;
		if(fade_in_left_)
		{
			g *= 1.0f - (float)fade_in_left_ / (float)fade;
			fade_in_left_--;
		}

		const double fi = std::floor(pos);
		const size_t i = (size_t)fi;
		const float t = (float)(pos - fi);
		const float *a0 = ring_.frame(i - 1), *a1 = ring_.frame(i), *a2 = ring_.frame(i + 1), *a3 = ring_.frame(i + 2);
		float *o = out + j * ch;
		if(xfade_left_)
		{
			const double pb = pos + xfade_offset_;
			const double fb = std::floor(pb);
			const size_t ib = (size_t)fb;
			const float tb = (float)(pb - fb);
			const float *b0 = ring_.frame(ib - 1), *b1 = ring_.frame(ib), *b2 = ring_.frame(ib + 1), *b3 = ring_.frame(ib + 2);
			const float w = 1.0f - ((float)xfade_left_ - 0.5f) / (float)fade;
			for(unsigned c = 0; c < ch; c++)
			{
				const float va = catmullRom(a0[c], a1[c], a2[c], a3[c], t);
				const float vb = catmullRom(b0[c], b1[c], b2[c], b3[c], tb);
				o[c] = g * (va + (vb - va) * w);
			}
			if(--xfade_left_ == 0)
			{
				pos += xfade_offset_;
				st_hard_drops_.fetch_add(1, std::memory_order_relaxed);
				st_frames_dropped_.fetch_add((uint64_t)xfade_offset_, std::memory_order_relaxed);
				xfade_offset_ = 0.0;
			}
		}
		else
		{
			for(unsigned c = 0; c < ch; c++)
				o[c] = g * catmullRom(a0[c], a1[c], a2[c], a3[c], t);
		}
		pos += ratio_;
	}
	if(produce < frames)
	{
		// Out of input: decay from the last sample written (this render's, or the previous one's).
		startTail(produce ? out + (produce - 1) * ch : last_out_.data(), ch);
		const size_t tail = writeTail(out + produce * ch, frames - produce, ch, fade);
		std::fill(out + (produce + tail) * ch, out + frames * ch, 0.0f);
		st_silent_frames_.fetch_add(frames - produce - tail, std::memory_order_relaxed);
	}
	else
	{
		for(unsigned c = 0; c < kTailChannels; c++)
			last_out_[c] = c < ch ? out[(frames - 1) * ch + c] : 0.0f;
	}

	// Frames left unplayed after this render, before consuming: the margin.
	const double margin_now = (double)avail - pos;

	// Keep frame floor(pos) - 1 as the interpolator's history.
	const double consumed = std::floor(pos) - 1.0;
	if(consumed > 0.0)
		ring_.consume((size_t)consumed);
	phase_ = pos - (consumed > 0.0 ? consumed : 0.0);

	if(underrun)
	{
		st_underruns_.fetch_add(1, std::memory_order_relaxed);
		const double fpms = config_.sample_rate / 1000.0;
		margin_frames_ = std::min(config_.max_margin_ms * fpms, margin_frames_ + config_.underrun_margin_step_ms * fpms);
		st_margin_ms_.store(margin_frames_ / fpms, std::memory_order_relaxed);
		enterBuffering();
		st_ratio_.store(ratio_, std::memory_order_relaxed);
		return true; // at least the decay was written
	}

	block_min_ = std::min(block_min_, margin_now);
	block_elapsed_ += (double)frames;
	if(block_elapsed_ >= block_frames_)
	{
		block_elapsed_ -= block_frames_;
		endBlock();
	}
	return true;
}

void Playout::endBlock()
{
	const double fpms = config_.sample_rate / 1000.0;
	const double low = std::min(block_min_, prev_block_min_);
	prev_block_min_ = block_min_;
	block_min_ = kInf;

	if(drop_in_window_)
	{
		// Margins measured across a backlog cut describe neither side of it.
		prev_block_min_ = kInf;
		drop_in_window_ = xfade_left_ != 0;
		return;
	}
	if(!std::isfinite(low))
		return;
	st_low_water_ms_.store(low / fpms, std::memory_order_relaxed);

	const double excess = low - margin_frames_;
	if(excess > hard_drop_frames_)
	{
		drop_pending_ = excess;
	}
	else
	{
		const double e = excess / config_.sample_rate; // seconds of audio beyond the target
		const double block_s = block_frames_ / config_.sample_rate;
		integ_ = clampd(integ_ + config_.ki * e * block_s, -config_.max_drift_adjust, config_.max_drift_adjust);
		ratio_ = 1.0 + clampd(config_.kp * e + integ_, -config_.max_ratio_adjust, config_.max_ratio_adjust);
	}
	st_ratio_.store(ratio_, std::memory_order_relaxed);
	st_drift_.store(integ_ * 1e6, std::memory_order_relaxed);

	// Shrink the target after a long stretch with no near miss.
	if(low < 0.25 * margin_frames_)
		clean_blocks_ = 0;
	else if(++clean_blocks_ >= decay_blocks_)
	{
		clean_blocks_ = 0;
		margin_frames_ = std::max(config_.min_margin_ms * fpms, margin_frames_ - config_.margin_decay_step_ms * fpms);
	}
	st_margin_ms_.store(margin_frames_ / fpms, std::memory_order_relaxed);
}

PlayoutStats Playout::stats() const
{
	PlayoutStats s;
	s.render_calls = st_render_calls_.load(std::memory_order_relaxed);
	s.frames_rendered = st_frames_rendered_.load(std::memory_order_relaxed);
	s.silent_frames = st_silent_frames_.load(std::memory_order_relaxed);
	s.underruns = st_underruns_.load(std::memory_order_relaxed);
	s.starts = st_starts_.load(std::memory_order_relaxed);
	s.hard_drops = st_hard_drops_.load(std::memory_order_relaxed);
	s.frames_dropped = st_frames_dropped_.load(std::memory_order_relaxed);
	s.flushes = st_flushes_.load(std::memory_order_relaxed);
	s.ratio = st_ratio_.load(std::memory_order_relaxed);
	s.drift_ppm = st_drift_.load(std::memory_order_relaxed);
	s.margin_target_ms = st_margin_ms_.load(std::memory_order_relaxed);
	s.low_water_ms = st_low_water_ms_.load(std::memory_order_relaxed);
	s.fill_ms = st_fill_ms_.load(std::memory_order_relaxed);
	s.avg_fill_ms = st_avg_fill_ms_.load(std::memory_order_relaxed);
	s.playing = st_playing_.load(std::memory_order_relaxed);
	for(size_t i = 0; i < kFillHistogramBuckets; i++)
		s.fill_histogram[i] = st_fill_hist_[i].load(std::memory_order_relaxed);
	return s;
}

} // namespace plk
