// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "CameraWarp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace plk::app {

namespace {

// Over how long "does the stick explain the picture's motion" is judged. Seconds, not the
// half second this was: at the start of every push the picture is a frame early or late
// against the table, the last half second then explained little, and the warp was switched
// off for the first third of a second of each push, which is when it is wanted (2026-11-27).
constexpr double kFastUs = 4e6;
// The table's speed has to have been this much (mean square over that half second, in
// widths a second squared) for a ratio to mean anything.
constexpr double kDriveFloor = 1e-5;
// A picture moves when it does this many widths a second (the measuring's own noise on a
// still picture is a tenth of it).
constexpr double kMoves = 0.015;
constexpr double kDelayUnknownUs = 100e3;

int amountStep(double amount)
{
	return std::max(0, std::min(CameraWarp::kAmounts - 1, (int)(std::abs(amount) * CameraWarp::kAmounts)));
}

int holdStep(double held_us)
{
	static const double ends[CameraWarp::kHolds - 1] = { 35e3, 70e3, 105e3, 140e3, 175e3, 210e3, 300e3, 450e3 };
	int i = 0;
	while(i < CameraWarp::kHolds - 1 && held_us >= ends[i])
		i++;
	return i;
}

} // namespace

int CameraWarp::Axis::learned() const
{
	int n = 0;
	for(const auto &row : seen)
		for(uint32_t s : row)
			n += s ? 1 : 0;
	return n;
}

bool CameraWarp::delayKnown() const
{
	// Enough of the picture's motion seen, and a good part of it going with the stick.
	const double moved = lag_syy_[0] + lag_syy_[1];
	if(moved < 0.02)
		return false;
	double best = 0;
	for(const Lag &l : lags_)
		best = std::max(best, (l.sxx[0] > 1e-9 ? l.sxy[0] * l.sxy[0] / l.sxx[0] : 0.0) + (l.sxx[1] > 1e-9 ? l.sxy[1] * l.sxy[1] / l.sxx[1] : 0.0));
	return best > 0.3 * moved;
}

double CameraWarp::delayUs(int mode) const
{
	if(mode < 0)
		mode = stick_.empty() ? 0 : stick_.back().mode;
	// By stops in this mode; failing three of them, by stops in any (the nearest thing).
	const std::deque<double> *stops = &stop_delays_us_[mode];
	std::vector<double> sorted;
	if(stops->size() >= 3)
		sorted.assign(stops->begin(), stops->end());
	else
		for(const auto &other : stop_delays_us_)
			sorted.insert(sorted.end(), other.begin(), other.end());
	if(sorted.size() >= 3)
	{
		std::sort(sorted.begin(), sorted.end());
		return sorted[sorted.size() / 2];
	}
	// Until three stops have been seen: the comparison's delay, less what a camera's start
	// usually adds to it.
	return known_delay_us_ > 0 ? std::max(40e3, known_delay_us_ - 20e3) : kDelayUnknownUs;
}

double CameraWarp::delayNowUs() const
{
	double score[kLags];
	int best = 0;
	for(int i = 0; i < kLags; i++)
	{
		const Lag &l = lags_[(size_t)i];
		score[i] = (l.sxx[0] > 1e-9 ? l.sxy[0] * l.sxy[0] / l.sxx[0] : 0.0) + (l.sxx[1] > 1e-9 ? l.sxy[1] * l.sxy[1] / l.sxx[1] : 0.0);
		if(score[i] > score[best])
			best = i;
	}
	// Between the steps: the top of a parabola through the best and the two beside it.
	double between = 0;
	if(best > 0 && best < kLags - 1)
	{
		const double bend = score[best - 1] - 2 * score[best] + score[best + 1];
		if(bend < -1e-12)
			between = std::max(-0.5, std::min(0.5, 0.5 * (score[best - 1] - score[best + 1]) / bend));
	}
	return 40e3 + (best + between) * 10e3;
}

double CameraWarp::tableSpeed(const Axis &axis, double amount, double held_us) const
{
	// Nothing until the pictures have said something: how a game answers the stick cannot
	// be guessed, not even its direction.
	if(!axis.any)
		return 0.0;
	// A stick in the middle asks for nothing, whatever the picture did while it was there:
	// a camera that swings round behind a walking character by itself would otherwise be
	// learned as the stick's doing, and the picture pushed aside while the stick is at rest.
	if(std::abs(amount) < 0.05)
		return 0.0;
	const int h = holdStep(held_us);
	// One step of how far over: what the pictures said of it at this time held, or at the
	// time held nearest to that. false: they have said nothing of this step at all.
	auto row = [&](int a, double *speed) {
		if(axis.seen[(size_t)a][(size_t)h])
		{
			*speed = axis.speed[(size_t)a][(size_t)h];
			return true;
		}
		for(int d = 1; d < kHolds; d++)
			for(int other : { h - d, h + d })
				if(other >= 0 && other < kHolds && axis.seen[(size_t)a][(size_t)other])
				{
					*speed = axis.speed[(size_t)a][(size_t)other];
					return true;
				}
		return false;
	};
	// Between two steps the stick is worth what lies between them: a thumb does not move in
	// steps, and a table read step by step makes a staircase of a smooth turn. A step the
	// pictures have said nothing of takes the nearest step that they have, the one less far
	// over first: a game's curve does not fall as the stick goes further.
	auto known = [&](int a) {
		double v = 0;
		for(int d = 0; d < kAmounts; d++)
			for(int other : { a - d, a + d })
				if(other >= 0 && other < kAmounts && row(other, &v))
					return v;
		return 0.0;
	};
	const double at = std::max(0.0, std::min((double)kAmounts - 1.0, std::abs(amount) * kAmounts - 0.5));
	const int lower = (int)at, upper = std::min(kAmounts - 1, lower + 1);
	const double part = at - lower;
	return known(lower) * (1.0 - part) + known(upper) * part;
}

void CameraWarp::onStick(double t_us, float x, float y, int mode)
{
	std::lock_guard<std::mutex> lk(mutex_);
	mode = std::max(0, std::min(kModes - 1, mode));
	// A stop to time the delay by: well over, and in the middle within 70 ms of that.
	{
		const double over = std::max(std::abs(x), std::abs(y));
		// The stick stays where it was last told to be until it is told again: if that was
		// well over, it has been well over up to now. (This counted on being told often; a
		// program at the controls, or a controller that only reports changes, tells once
		// for a push, and no stop was timed at all.)
		if(!stick_.empty() && std::max(std::abs(stick_.back().x), std::abs(stick_.back().y)) >= 0.6)
			well_over_us_ = t_us;
		if(over >= 0.6)
			well_over_us_ = t_us;
		else if(over < 0.12 && well_over_us_ > 0)
		{
			if(t_us - well_over_us_ <= 70e3 && speed_before_ > 0.1)
			{
				let_go_us_ = (t_us + well_over_us_) / 2;
				let_go_mode_ = mode;
			}
			well_over_us_ = 0;
		}
	}
	if(!stick_.empty() && stick_.back().x == x && stick_.back().y == y && stick_.back().mode == mode)
		return;
	const double over = std::max(std::abs(x), std::abs(y));
	// A wheel (L1) is picked from with the stick, and the stick is still over when L1 is let
	// go: the camera does not turn by it until it has been back in the middle.
	bool dead = false;
	if(!stick_.empty())
	{
		const Sample &last = stick_.back();
		const bool out_now = over >= 0.12;
		dead = out_now && (last.dead || ((last.mode & 2) && !(mode & 2)));
		(void)0;
	}
	// How long the camera has been asked to turn this way: since the stick left the middle
	// on this side (or the mode changed), whatever steps it has passed through since.
	double since_x = t_us, since_y = t_us;
	if(!stick_.empty())
	{
		const Sample &last = stick_.back();
		auto out = [](float v) { return std::abs(v) >= 0.12f; };
		if(last.mode == mode && out(last.x) && out(x) && (last.x >= 0) == (x >= 0))
			since_x = last.since_x_us;
		if(last.mode == mode && out(last.y) && out(y) && (last.y >= 0) == (y >= 0))
			since_y = last.since_y_us;
	}
	// A new push, either way, is believed afresh, unless the last one moved nothing: in a
	// menu or on a map the stick is not the camera's at all, and every push there would
	// first jerk the picture and then let it go. After such a push the next has to show
	// that it moves the picture before it is believed (a sixth of a second into it).
	if(since_x == t_us)
		for(auto &m : axes_)
			m[0].flow = m[0].flow < 0.3 ? 0.25 : 1.0;
	if(since_y == t_us)
		for(auto &m : axes_)
			m[1].flow = m[1].flow < 0.3 ? 0.25 : 1.0;
	stick_.push_back({ t_us, x, y, mode, dead, since_x, since_y });
	// As far back as the longest delay and a picture's time need, with room.
	while(stick_.size() > 2 && stick_[1].t_us < t_us - 4e6)
		stick_.pop_front();
}

void CameraWarp::drive(double from_us, double to_us, double *x, double *y) const
{
	*x = *y = 0;
	if(stick_.empty() || to_us <= from_us)
		return;
	// The stick holds its position between two reports; how long it has been there goes on
	// meanwhile, so a stretch is taken in pieces of at most 10 ms.
	double sum_x = 0, sum_y = 0;
	for(size_t i = 0; i < stick_.size(); i++)
	{
		const Sample &s = stick_[i];
		const double begin = std::max(from_us, s.t_us);
		const double end = std::min(to_us, i + 1 < stick_.size() ? stick_[i + 1].t_us : to_us);
		const Axis &ax = axes_[s.mode][0], &ay = axes_[s.mode][1];
		if(s.dead)
			continue;
		for(double at = begin; at < end; at += 10000.0)
		{
			const double piece = std::min(10000.0, end - at), middle = at + piece / 2;
			sum_x += (s.x < 0 ? -1.0 : 1.0) * tableSpeed(ax, s.x, middle - s.since_x_us) * piece;
			sum_y += (s.y < 0 ? -1.0 : 1.0) * tableSpeed(ay, s.y, middle - s.since_y_us) * piece;
		}
	}
	*x = sum_x / (to_us - from_us);
	*y = sum_y / (to_us - from_us);
}

// One way of the picture's motion over one pair of pictures: how well the table told it,
// and what it says about the table.
void CameraWarp::learn(Axis &axis, bool vertical, double from_us, double to_us, double speed, double dt_us)
{
	// How much of the picture's motion the table explains, over the last half second.
	const double keep = std::exp(-dt_us / kFastUs);
	double dx, dy;
	drive(from_us, to_us, &dx, &dy);
	const double told = vertical ? dy : dx;
	if(std::abs(told) > 0.05)
		axis.flow += (std::max(0.0, std::min(1.5, speed / told)) - axis.flow) * (1.0 - std::exp(-dt_us / 100e3));
	// A stick that moves nothing just now (a menu; see `flow`) says nothing about how well
	// the table explains the game: counted, a minute in a menu left the warp off for the
	// first seconds back in the game.
	const bool blocked = axis.flow < 0.3 && std::abs(told) > 0.05;
	if(!blocked)
	{
		axis.sxx = axis.sxx * keep + told * told;
		axis.sxy = axis.sxy * keep + told * speed;
		axis.syy = axis.syy * keep + speed * speed;
	}
	if(blocked)
		;
	else if(axis.sxx * (1.0 - keep) > kDriveFloor)
		// Not the correlation, which a table twice too large would pass with full marks: one
		// minus what is left of the motion when the table's is taken from it.
		axis.explained = axis.syy > 1e-12 ? std::max(0.0, 1.0 - (axis.syy - 2.0 * axis.sxy + axis.sxx) / axis.syy) : 0.0;
	else if(axis.syy * (1.0 - keep) > kMoves * kMoves)
		// The table says the picture stands, and it moves: not by the stick. Within a second.
		axis.explained *= std::exp(-dt_us / 1e6);

	// The table: what the stick was at over this pair's time (as the picture shows it, the
	// delay ago) is worth the picture's speed, with the stick's sign taken out. The stick
	// need not have stood still: a thumb seldom does. Its mean place over the pair is taken,
	// if it stayed on one side, in one mode and within a step and a half of that mean; the
	// further it wandered, the less the pair is believed.
	const Sample *first = nullptr;
	double sum = 0, weight = 0, least = 2, most = -2;
	bool usable = true;
	for(size_t i = 0; i < stick_.size() && usable; i++)
	{
		const double begin = std::max(from_us, stick_[i].t_us), end = std::min(to_us, i + 1 < stick_.size() ? stick_[i + 1].t_us : to_us);
		if(end <= begin)
			continue;
		const double here = vertical ? stick_[i].y : stick_[i].x;
		if(!first)
			first = &stick_[i];
		else if(stick_[i].mode != first->mode)
			usable = false;
		if(stick_[i].dead)
			usable = false;
		sum += here * (end - begin);
		weight += end - begin;
		least = std::min(least, here);
		most = std::max(most, here);
	}
	if(!first || !usable || weight <= 0 || &axis != &axes_[first->mode][vertical ? 1 : 0])
		return;
	const double amount = sum / weight, wandered = (most - least) * kAmounts;
	if(wandered > 1.5 || (least < -0.02 && most > 0.02) || std::abs(amount) < 0.05)
		return;
	const int a = amountStep(amount), h = holdStep((from_us + to_us) / 2 - (vertical ? first->since_y_us : first->since_x_us));
	double &cell = axis.speed[(size_t)a][(size_t)h];
	uint32_t &seen = axis.seen[(size_t)a][(size_t)h];
	// No camera turns faster than three pictures' width a second: more is a pair measured wrongly.
	const double worth = std::max(-3.0, std::min(3.0, speed * (amount < 0 ? -1.0 : 1.0)));
	// The first time a step is seen it is believed outright; after that a fifth of the way
	// each time (less if the stick wandered), which follows a game that changes its answer
	// within a few pictures at that step and is not thrown by one picture measured badly.
	bool &kept = axis.kept[(size_t)a][(size_t)h];
	// While the stick moves nothing that it is known to move (a menu, the camera against its
	// stop), that is not the game's new answer to the stick, and the table is left alone.
	if(seen >= 3 && !kept && axis.flow < 0.3 && std::abs(cell) > 0.05)
		return;
	if(seen == 0 || kept)
		cell = worth;
	else
		cell += (worth - cell) * (wandered > 0.5 ? 0.1 : 0.2);
	kept = false;
	seen++;
	axis.any = true;
}

void CameraWarp::onMotion(double previous_us, double picture_us, float dx, float dy, bool valid)
{
	const double dt_us = picture_us - previous_us;
	if(dt_us < 5000.0 || dt_us > 120000.0)
		return;
	std::lock_guard<std::mutex> lk(mutex_);
	{
		// Was the stick over, as this pair shows it?
		const double at = (previous_us + picture_us) / 2 - delayUs();
		const Sample *then = nullptr;
		for(const Sample &s : stick_)
		{
			if(s.t_us > at)
				break;
			then = &s;
		}
		// Only a stick that is over but not flat out: at full tilt the picture is blurred by
		// its own speed and often cannot be measured, which says nothing about whether a
		// turn slides it. (Counted, it switched the warp off in every fast turn, where there
		// is the most to hide: a quarter of what was needed was shown. 2026-11-27.)
		const double over = then ? std::max(std::abs(then->x), std::abs(then->y)) : 0.0;
		if(then && !then->dead && over >= 0.5 && over <= 0.85)
			measurable_ += ((valid ? 1.0 : 0.0) - measurable_) * (1.0 - std::exp(-dt_us / 1.5e6));
	}
	if(!valid)
		return;  // no common shift: nothing is learned of this pair
	const double seconds = dt_us / 1e6;
	const double vx = dx / seconds, vy = dy / seconds;
	pairs_++;

	// How late, by a stop: the stick was let go from well over, and this is the first picture
	// that no longer moves as before. Where in the pair the stop fell is told by how much of
	// the speed before is left in it.
	const double speed_now = std::hypot(vx, vy);
	if(let_go_us_ > 0)
	{
		if(picture_us > let_go_us_ && speed_now < 0.5 * speed_before_)
		{
			const double stop = previous_us + dt_us * std::min(1.0, speed_now / speed_before_);
			const double delay = stop - let_go_us_;
			if(delay > 40e3 && delay < 350e3)
			{
				std::deque<double> &stops = stop_delays_us_[let_go_mode_];
				stops.push_back(delay);
				if(stops.size() > 15)
					stops.pop_front();
			}
			let_go_us_ = 0;
		}
		else if(picture_us - let_go_us_ > 450e3)
			let_go_us_ = 0;
	}
	else if(well_over_us_ > 0 && speed_now > 0.1)
		speed_before_ += (speed_now - speed_before_) * (speed_before_ > 0 ? 0.5 : 1.0);
	else if(well_over_us_ == 0 && let_go_us_ == 0)
		speed_before_ = 0;

	// How late: every delay that may be is tried against this pair. The stick through a
	// plain curve: nothing in the inner quarter, then rising with the square.
	{
		const double keep = std::exp(-dt_us / 30e6);
		lag_syy_[0] = lag_syy_[0] * keep + vx * vx;
		lag_syy_[1] = lag_syy_[1] * keep + vy * vy;
		auto curve = [](double v) {
			const double a = std::max(0.0, std::abs(v) - 0.25) / 0.75;
			return v < 0 ? -a * a : a * a;
		};
		for(int i = 0; i < kLags; i++)
		{
			const double d = 40e3 + i * 10e3, from = previous_us - d, to = picture_us - d;
			double fx = 0, fy = 0;
			for(size_t k = 0; k < stick_.size(); k++)
			{
				const double begin = std::max(from, stick_[k].t_us), end = std::min(to, k + 1 < stick_.size() ? stick_[k + 1].t_us : to);
				if(end <= begin)
					continue;
				fx += curve(stick_[k].x) * (end - begin);
				fy += curve(stick_[k].y) * (end - begin);
			}
			fx /= dt_us;
			fy /= dt_us;
			Lag &l = lags_[(size_t)i];
			l.sxx[0] = l.sxx[0] * keep + fx * fx;
			l.sxy[0] = l.sxy[0] * keep + fx * vx;
			l.sxx[1] = l.sxx[1] * keep + fy * fy;
			l.sxy[1] = l.sxy[1] * keep + fy * vy;
		}
	}
	// Nothing is learned before the delay is known: a table filled in with the stick and the
	// pictures out of step is worse than none. And a table that was filled in at another
	// delay (the connection changed) is of no use at this one.
	if(delayKnown())
		known_delay_us_ = delayNowUs();
	if(known_delay_us_ <= 0)
		return;
	// The mode the pair belongs to: the one the stick was in, the delay before its middle.
	// (Each mode has its own delay, so the mode is found with the present one's and the
	// pair is then placed with its own.)
	auto modeAt = [&](double at) {
		int mode = 0;
		for(const Sample &s : stick_)
		{
			if(s.t_us > at)
				break;
			mode = s.mode;
		}
		return mode;
	};
	const int mode = modeAt((previous_us + picture_us) / 2 - delayUs());
	const double delay = delayUs(mode);
	// A table that was filled in at another delay (the connection changed) is of no use at
	// this one: this mode's are begun again.
	if(table_delay_us_[mode] > 0 && std::abs(delay - table_delay_us_[mode]) > 25e3)
		for(Axis &axis : axes_[mode])
			axis = Axis();
	if(table_delay_us_[mode] == 0 || std::abs(delay - table_delay_us_[mode]) > 25e3)
		table_delay_us_[mode] = delay;
	const double from = previous_us - delay, to = picture_us - delay;
	learn(axes_[mode][0], false, from, to, vx, dt_us);
	learn(axes_[mode][1], true, from, to, vy, dt_us);
}

void CameraWarp::predict(double picture_us, double display_us, float *dx, float *dy) const
{
	*dx = *dy = 0;
	std::lock_guard<std::mutex> lk(mutex_);
	if(pairs_ < 20 || strength_ <= 0 || stick_.empty())
		return;
	// The picture shows the stick as it was until the delay before it arrived. What the
	// stick has asked for since then, up to the moment the picture is on screen, is to come.
	const double from_us = picture_us - delayUs(), to_us = display_us;
	if(to_us <= from_us)
		return;
	double sx, sy;
	drive(from_us, to_us, &sx, &sy);
	const double seconds = (to_us - from_us) / 1e6;
	// Each way by how well the stick explains the picture that way, in the mode it is in:
	// nothing below a third of the motion explained, all of it from two thirds on.
	const Axis *axes = axes_[stick_.back().mode];
	auto trust = [](const Axis &axis) {
		if(!axis.any)
			return 0.0;
		const double t = std::min(1.0, std::max(0.0, (axis.explained - 0.33) / 0.33));
		// And whether the camera is going where the stick sends it at all, just now.
		const double f = std::min(1.0, std::max(0.0, (axis.flow - 0.2) / 0.4));
		return t * t * (3.0 - 2.0 * t) * f * f * (3.0 - 2.0 * f);
	};
	// And by how many of the pictures of a turn can be measured: none of the warp under
	// half of them, all of it from four in five.
	const double seen = std::min(1.0, std::max(0.0, (measurable_ - 0.5) / 0.3));
	const double limit = max_shift_;
	const double gx = sx * seconds, gy = sy * seconds;
	*dx = (float)std::max(-limit, std::min(limit, gx * trust(axes[0]) * seen * strength_));
	*dy = (float)std::max(-limit, std::min(limit, gy * trust(axes[1]) * seen * strength_));
}

void CameraWarp::setStrength(float strength)
{
	std::lock_guard<std::mutex> lk(mutex_);
	strength_ = std::max(0.0f, std::min(1.0f, strength));
}

std::string CameraWarp::describe() const
{
	std::lock_guard<std::mutex> lk(mutex_);
	std::string out;
	char line[256];
	double best = 0;
	for(const Lag &l : lags_)
		best = std::max(best, (l.sxx[0] > 1e-9 ? l.sxy[0] * l.sxy[0] / l.sxx[0] : 0.0) + (l.sxx[1] > 1e-9 ? l.sxy[1] * l.sxy[1] / l.sxx[1] : 0.0));
	snprintf(line, sizeof(line), "delay %.0f ms (%s; %zu stops timed), %llu pairs, %zu stick reports kept (last %.2f %.2f); picture's motion %.3f, of it with the stick %.3f\n",
			delayUs() / 1000.0, known_delay_us_ > 0 ? "measured" : "not known yet", stop_delays_us_[stick_.empty() ? 0 : stick_.back().mode].size(), (unsigned long long)pairs_, stick_.size(),
			stick_.empty() ? 0.0 : (double)stick_.back().x, stick_.empty() ? 0.0 : (double)stick_.back().y, lag_syy_[0] + lag_syy_[1], best);
	out += line;
	static const char *const names[kModes] = { "free look", "aiming (L2)", "wheel (L1)", "L1 + L2", "free look", "L2, other view (R3)",
		"wheel (L1)", "L1 + L2, other view" };
	for(int mode = 0; mode < kModes; mode++)
		for(int vertical = 0; vertical < 2; vertical++)
		{
			const Axis &axis = axes_[mode][vertical];
			if(!axis.any)
				continue;
			snprintf(line, sizeof(line), "%s, %s: explained %.2f; %% of the picture a second by how far over (rows) and how long (columns)\n",
					names[mode], vertical ? "up and down" : "sideways", axis.explained);
			out += line;
			for(int a = 0; a < kAmounts; a++)
			{
				snprintf(line, sizeof(line), "  %3d %%:", (int)((a + 0.5) * 100 / kAmounts));
				out += line;
				for(int h = 0; h < kHolds; h++)
				{
					if(axis.seen[(size_t)a][(size_t)h])
						snprintf(line, sizeof(line), " %7.1f(%u)", axis.speed[(size_t)a][(size_t)h] * 100.0, axis.seen[(size_t)a][(size_t)h]);
					else
						snprintf(line, sizeof(line), "       -   ");
					out += line;
				}
				out += "\n";
			}
		}
	return out;
}

std::string CameraWarp::save() const
{
	std::lock_guard<std::mutex> lk(mutex_);
	std::string out;
	char line[96];
	snprintf(line, sizeof(line), "plk-camera-warp 2 %d %d %d\ndelay %.0f\n", kModes, kAmounts, kHolds, known_delay_us_);
	out += line;
	for(int mode = 0; mode < kModes; mode++)
		if(stop_delays_us_[mode].size() >= 3)
		{
			snprintf(line, sizeof(line), "stops %d %.0f\n", mode, delayUs(mode));
			out += line;
		}
	for(int mode = 0; mode < kModes; mode++)
		for(int vertical = 0; vertical < 2; vertical++)
		{
			const Axis &axis = axes_[mode][vertical];
			for(int a = 0; a < kAmounts; a++)
				for(int h = 0; h < kHolds; h++)
					if(axis.seen[(size_t)a][(size_t)h])
					{
						snprintf(line, sizeof(line), "step %d %d %d %d %.6f\n", mode, vertical, a, h, axis.speed[(size_t)a][(size_t)h]);
						out += line;
					}
		}
	return out;
}

bool CameraWarp::load(const std::string &text)
{
	int modes = 0, amounts = 0, holds = 0, used = 0;
	if(sscanf(text.c_str(), "plk-camera-warp 2 %d %d %d\n%n", &modes, &amounts, &holds, &used) != 3 || modes != kModes
			|| amounts != kAmounts || holds != kHolds)
		return false;
	std::lock_guard<std::mutex> lk(mutex_);
	for(auto &m : axes_)
		for(Axis &axis : m)
			axis = Axis();
	size_t at = (size_t)used;
	while(at < text.size())
	{
		size_t end = text.find('\n', at);
		if(end == std::string::npos)
			end = text.size();
		const std::string line = text.substr(at, end - at);
		at = end + 1;
		double delay = 0, stops = 0, speed = 0;
		int mode, vertical, a, h;
		if(sscanf(line.c_str(), "delay %lf", &delay) == 1)
		{
			known_delay_us_ = delay > 0 ? std::max(20e3, std::min(400e3, delay)) : 0;
			for(auto &d : stop_delays_us_)
				d.clear();
			for(double &d : table_delay_us_)
				d = 0;
		}
		else if(sscanf(line.c_str(), "stops %d %lf", &mode, &stops) == 2 && mode >= 0 && mode < kModes)
		{
			// A stop-timed delay is kept as three such stops: the next real ones replace them.
			if(stops > 40e3 && stops < 350e3)
				stop_delays_us_[mode].assign(3, stops);
		}
		else if(sscanf(line.c_str(), "step %d %d %d %d %lf", &mode, &vertical, &a, &h, &speed) == 5 && mode >= 0 && mode < kModes
				&& vertical >= 0 && vertical < 2 && a >= 0 && a < kAmounts && h >= 0 && h < kHolds && std::abs(speed) <= 3.0)
		{
			Axis &axis = axes_[mode][vertical];
			axis.speed[(size_t)a][(size_t)h] = speed;
			axis.seen[(size_t)a][(size_t)h] = 1;
			axis.kept[(size_t)a][(size_t)h] = true;
			axis.any = true;
			// Believed until the pictures say otherwise: they will within a push or two.
			axis.explained = 0.8;
		}
	}
	pairs_ = std::max<uint64_t>(pairs_, 20);
	return true;
}

CameraWarp::State CameraWarp::state() const
{
	std::lock_guard<std::mutex> lk(mutex_);
	State s;
	s.mode = stick_.empty() ? 0 : stick_.back().mode;
	s.aiming = (s.mode & 1) != 0;
	const Axis *axes = axes_[s.mode];
	s.delay_ms = delayUs() / 1000.0;
	s.delays_seen = known_delay_us_ > 0 ? 1 : 0;
	s.gain_x = axes[0].any ? tableSpeed(axes[0], 1.0, 1e6) : 0.0;
	s.gain_y = axes[1].any ? tableSpeed(axes[1], 1.0, 1e6) : 0.0;
	s.explained = std::max(axes[0].explained, axes[1].explained);
	s.steps_learned = axes[0].learned() + axes[1].learned();
	s.full_tilt = std::abs(s.gain_x);
	s.learned = pairs_ >= 20 && (axes[0].any || axes[1].any);
	return s;
}

} // namespace plk::app
