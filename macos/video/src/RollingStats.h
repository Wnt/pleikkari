// Fixed-capacity rolling sample window with percentile summaries.
#pragma once

#include "plk/VideoPipeline.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace plk::video::detail {

class RollingWindow {
public:
	explicit RollingWindow(size_t capacity = 36000) : capacity_(capacity ? capacity : 1) {}

	void add(double v)
	{
		if(!std::isfinite(v))
			return;
		if(samples_.size() < capacity_)
			samples_.push_back(v);
		else
			samples_[next_] = v;
		next_ = (next_ + 1) % capacity_;
	}
	void clear() { samples_.clear(); next_ = 0; }

	Distribution summarize() const
	{
		Distribution d;
		d.count = samples_.size();
		if(samples_.empty())
			return d;
		std::vector<double> s(samples_);
		std::sort(s.begin(), s.end());
		double sum = 0;
		for(double v : s)
			sum += v;
		d.mean = sum / s.size();
		auto pct = [&](double p) {
			// Nearest-rank percentile.
			size_t rank = (size_t)std::ceil(p / 100.0 * s.size());
			rank = std::clamp<size_t>(rank, 1, s.size());
			return s[rank - 1];
		};
		d.p50 = pct(50);
		d.p95 = pct(95);
		d.p99 = pct(99);
		d.max = s.back();
		return d;
	}

private:
	size_t capacity_;
	size_t next_ = 0;
	std::vector<double> samples_;
};

}  // namespace plk::video::detail
