// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "FrameAnalysis.h"

#include <algorithm>
#include <cstdio>

namespace plk::app {

namespace {

constexpr double kHistoryUs = 10e6;         // what the 1 % low is taken from
constexpr size_t kArrivals = 240;           // frame arrivals the budget is the median of
constexpr size_t kIntervals = 120;          // present intervals handed to the panel
constexpr size_t kBaselineMinFrames = 20;
constexpr double kBaselineCheckUs = 250000.0;
constexpr double kBaselineSettleUs = 250000.0;  // after the enhancer's last work, before a window counts

float ms(double from_us, double to_us)
{
	if(from_us <= 0 || to_us <= 0)
		return -1.0f;
	return (float)(std::max(0.0, to_us - from_us) / 1000.0);
}

// The cost of the values that were measured (>= 0).
StageCost cost(std::vector<float> &values)
{
	StageCost c;
	values.erase(std::remove_if(values.begin(), values.end(), [](float v) { return v < 0; }), values.end());
	c.samples = values.size();
	if(values.empty())
		return c;
	double sum = 0;
	for(float v : values)
		sum += v;
	c.mean_ms = sum / (double)values.size();
	const size_t at = std::min(values.size() - 1, (size_t)((double)values.size() * 0.95));
	std::nth_element(values.begin(), values.begin() + (long)at, values.end());
	c.p95_ms = values[at];
	return c;
}

double percentile(std::vector<float> values, double share)
{
	if(values.empty())
		return 0;
	const size_t at = std::min(values.size() - 1, (size_t)((double)values.size() * share));
	std::nth_element(values.begin(), values.begin() + (long)at, values.end());
	return values[at];
}

} // namespace

const char *stageName(AnalysisSnapshot::Stage stage)
{
	switch(stage)
	{
		case AnalysisSnapshot::ToDecoder: return "To decoder";
		case AnalysisSnapshot::Decode: return "Decode";
		case AnalysisSnapshot::Queue: return "Queue";
		case AnalysisSnapshot::Render: return "Render";
		case AnalysisSnapshot::GpuWait: return "GPU wait";
		case AnalysisSnapshot::Gpu: return "GPU";
		case AnalysisSnapshot::Scanout: return "Scanout";
		case AnalysisSnapshot::kStages: break;
	}
	return "";
}

FrameAnalysis::FrameAnalysis(double window_s) : window_us_(window_s * 1e6), inactive_since_us_(1.0) {}

void FrameAnalysis::onFrame(const video::FrameTiming &t)
{
	Frame f;
	const double start = t.readyUs > 0 ? t.readyUs : t.submitUs;
	f.stage[AnalysisSnapshot::ToDecoder] = t.readyUs > 0 ? ms(t.readyUs, t.submitUs) : -1.0f;
	f.stage[AnalysisSnapshot::Decode] = ms(t.submitUs, t.decodedUs);
	f.stage[AnalysisSnapshot::Queue] = ms(t.decodedUs, t.renderStartUs);
	f.stage[AnalysisSnapshot::Render] = ms(t.renderStartUs, t.committedUs);
	f.stage[AnalysisSnapshot::GpuWait] = ms(t.committedUs, t.gpuStartUs);
	f.stage[AnalysisSnapshot::Gpu] = ms(t.gpuStartUs, t.gpuEndUs);
	f.stage[AnalysisSnapshot::Scanout] = t.presented ? ms(t.gpuEndUs, t.presentedUs) : -1.0f;
	f.processing = ms(start, t.gpuEndUs);
	f.total = t.presented ? ms(start, t.presentedUs) : -1.0f;
	f.presented = t.presented;
	f.offscreen = t.offscreen;
	f.age = (float)t.enhanceAgeMs;
	f.at_us = t.presented ? t.presentedUs : t.gpuEndUs > 0 ? t.gpuEndUs : t.committedUs;

	std::lock_guard<std::mutex> lk(mutex_);
	if(t.presented || t.offscreen)
	{
		if(last_at_us_ > 0 && f.at_us > last_at_us_)
			f.interval = (float)((f.at_us - last_at_us_) / 1000.0);
		if(f.at_us > last_at_us_)
			last_at_us_ = f.at_us;
	}
	if(last_submit_us_ > 0 && t.submitUs > last_submit_us_ && t.submitUs - last_submit_us_ < 1e6)
	{
		arrivals_.push_back((float)((t.submitUs - last_submit_us_) / 1000.0));
		if(arrivals_.size() > kArrivals)
			arrivals_.pop_front();
	}
	last_submit_us_ = t.submitUs;
	frames_.push_back(f);
	while(!frames_.empty() && frames_.front().at_us < f.at_us - kHistoryUs)
		frames_.pop_front();
	if(f.at_us - last_baseline_check_us_ >= kBaselineCheckUs)
	{
		last_baseline_check_us_ = f.at_us;
		updateBaseline(f.at_us);
	}
}

void FrameAnalysis::onEnhance(const video::EnhanceTiming &t, double now_us)
{
	Result r;
	r.at_us = now_us;
	r.ok = t.ok;
	r.input = (float)t.inputGpuMs;
	r.network = (float)t.enhancerMs;
	r.round_trip = (float)t.totalMs;
	std::lock_guard<std::mutex> lk(mutex_);
	results_.push_back(r);
	last_result_us_ = now_us;
	while(!results_.empty() && results_.front().at_us < now_us - kHistoryUs)
		results_.pop_front();
}

void FrameAnalysis::setEnhancerActive(bool active)
{
	std::lock_guard<std::mutex> lk(mutex_);
	if(active == active_)
		return;
	active_ = active;
	inactive_since_us_ = active ? 0.0 : std::max(1.0, last_at_us_);
}

void FrameAnalysis::clearBaseline()
{
	std::lock_guard<std::mutex> lk(mutex_);
	baseline_ = Baseline();
	// The frames already in the window were drawn the old way.
	if(!active_)
		inactive_since_us_ = std::max(1.0, last_at_us_);
}

// With mutex_ held. The last full window in which the enhancer did nothing becomes the baseline.
void FrameAnalysis::updateBaseline(double now_us)
{
	if(active_)
		return;
	const double quiet_since = std::max(inactive_since_us_, last_result_us_) + kBaselineSettleUs;
	if(now_us - window_us_ < quiet_since)
		return;
	std::vector<float> values[AnalysisSnapshot::kStages], processing, total;
	size_t frames = 0;
	for(auto it = frames_.rbegin(); it != frames_.rend() && it->at_us > now_us - window_us_; ++it)
	{
		frames++;
		for(int i = 0; i < AnalysisSnapshot::kStages; i++)
			values[i].push_back(it->stage[i]);
		processing.push_back(it->processing);
		total.push_back(it->total);
	}
	if(frames < kBaselineMinFrames)
		return;
	Baseline b;
	b.valid = true;
	for(int i = 0; i < AnalysisSnapshot::kStages; i++)
	{
		const StageCost c = cost(values[i]);
		b.stage[i] = c.samples ? c.mean_ms : -1.0;
	}
	StageCost c = cost(processing);
	b.processing = c.samples ? c.mean_ms : -1.0;
	c = cost(total);
	b.total = c.samples ? c.mean_ms : -1.0;
	baseline_ = b;
}

AnalysisSnapshot FrameAnalysis::snapshot(double now_us) const
{
	AnalysisSnapshot s;
	std::vector<float> values[AnalysisSnapshot::kStages], processing, total, age, intervals, history;
	std::lock_guard<std::mutex> lk(mutex_);
	uint64_t enhanced = 0;
	for(const Frame &f : frames_)
	{
		if(f.interval >= 0 && f.at_us > now_us - kHistoryUs)
			history.push_back(f.interval);
		if(f.at_us <= now_us - window_us_ || f.at_us > now_us)
			continue;
		s.frames++;
		s.offscreen = s.offscreen || f.offscreen;
		if(!f.presented && !f.offscreen)
			s.not_presented++;
		for(int i = 0; i < AnalysisSnapshot::kStages; i++)
			values[i].push_back(f.stage[i]);
		processing.push_back(f.processing);
		total.push_back(f.total);
		intervals.push_back(f.interval);
		if(f.age >= 0)
		{
			enhanced++;
			age.push_back(f.age);
		}
	}
	for(int i = 0; i < AnalysisSnapshot::kStages; i++)
		s.stage[i] = cost(values[i]);
	s.processing = cost(processing);
	s.total = cost(total);
	s.nr_age = cost(age);
	s.enhanced_share = s.frames ? (double)enhanced / (double)s.frames : 0.0;
	const StageCost interval = cost(intervals);
	if(interval.samples >= 2 && interval.mean_ms > 0)
		s.fps = 1000.0 / interval.mean_ms;
	else
		s.fps = (double)(s.frames - s.not_presented) / (window_us_ / 1e6);
	const double slow = percentile(history, 0.99);
	s.low1_fps = slow > 0 ? 1000.0 / slow : 0.0;
	const size_t first = history.size() > kIntervals ? history.size() - kIntervals : 0;
	s.intervals_ms.assign(history.begin() + (long)first, history.end());
	if(arrivals_.size() >= 5)
		s.budget_ms = percentile(std::vector<float>(arrivals_.begin(), arrivals_.end()), 0.5);

	std::vector<float> input, network, round_trip;
	for(const Result &r : results_)
	{
		if(r.at_us <= now_us - window_us_ || r.at_us > now_us)
			continue;
		if(!r.ok)
		{
			s.nr_failed++;
			continue;
		}
		input.push_back(r.input);
		network.push_back(r.network);
		round_trip.push_back(r.round_trip);
	}
	s.results_per_s = (double)network.size() / (window_us_ / 1e6);
	s.nr_input = cost(input);
	s.nr_network = cost(network);
	s.nr_round_trip = cost(round_trip);

	s.enhancer_active = active_;
	s.baseline = active_ && baseline_.valid;
	if(s.baseline)
	{
		for(int i = 0; i < AnalysisSnapshot::kStages; i++)
			s.stage[i].baseline_ms = baseline_.stage[i];
		s.processing.baseline_ms = baseline_.processing;
		s.total.baseline_ms = baseline_.total;
	}
	return s;
}

std::string formatAnalysisLine(const AnalysisSnapshot &s)
{
	static const char *const keys[AnalysisSnapshot::kStages] = { "to_decoder", "decode", "queue", "render", "gpu_wait", "gpu",
		"scanout" };
	char buf[160];
	auto value = [&](const char *name, const StageCost &c) {
		if(!c.samples)
			return std::string(" ") + name + "=-";
		snprintf(buf, sizeof(buf), " %s=%.2f", name, c.mean_ms);
		return std::string(buf);
	};
	snprintf(buf, sizeof(buf), "analysis: mean ms of the last second: budget=%.2f fps=%.1f low1=%.1f", s.budget_ms, s.fps,
			s.low1_fps);
	std::string line = buf;
	for(int i = 0; i < AnalysisSnapshot::kStages; i++)
		line += value(keys[i], s.stage[i]);
	line += value("processing", s.processing);
	line += value("total", s.total);
	if(s.enhancer_active)
	{
		line += " | nr:";
		line += value("input", s.nr_input);
		line += value("network", s.nr_network);
		line += value("round_trip", s.nr_round_trip);
		line += value("age", s.nr_age);
		snprintf(buf, sizeof(buf), " results_per_s=%.1f enhanced=%.0f%%", s.results_per_s, s.enhanced_share * 100.0);
		line += buf;
	}
	if(s.baseline)
	{
		line += " | with nr off:";
		for(int i = 0; i < AnalysisSnapshot::kStages; i++)
			if(s.stage[i].baseline_ms >= 0)
			{
				snprintf(buf, sizeof(buf), " %s=%.2f", keys[i], s.stage[i].baseline_ms);
				line += buf;
			}
		if(s.processing.baseline_ms >= 0)
		{
			snprintf(buf, sizeof(buf), " processing=%.2f", s.processing.baseline_ms);
			line += buf;
		}
		if(s.total.baseline_ms >= 0)
		{
			snprintf(buf, sizeof(buf), " total=%.2f", s.total.baseline_ms);
			line += buf;
		}
	}
	return line;
}

} // namespace plk::app
