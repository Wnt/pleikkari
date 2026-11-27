// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "CameraWarp.h"

#include "TestHarness.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <random>

using namespace plk::app;

namespace {

// A made-up game: the picture slides at `answer(stick)` widths a second, `delay` after the
// stick was there; pictures come 30 times a second. The thumb rests, pushes the stick over,
// holds it, and wanders, in turn.
struct Game
{
	CameraWarp warp;
	double t_us = 1e6;
	double delay_us = 90000;
	// What the game makes of the stick: sideways and up and down, for free look and aiming.
	std::function<double(double, bool)> answer_x = [](double s, bool) { return 0.8 * s; };
	std::function<double(double, bool)> answer_y = [](double s, bool) { return 0.45 * s; };
	bool aiming = false;
	double last_picture_us = 0;
	std::mt19937 random{ 7 };
	struct Stick { double t; float x, y; bool aiming; };
	std::vector<Stick> stick_log;
	double phase = 0;

	Stick stickAt(double at_us) const
	{
		Stick s{ 0, 0.0f, 0.0f, false };
		for(const Stick &e : stick_log)
		{
			if(e.t > at_us)
				break;
			s = e;
		}
		return s;
	}

	// What the picture really does between `from_us` and `to_us`: widths, sideways.
	double trueShift(double from_us, double to_us) const
	{
		double sum = 0;
		for(double at = from_us; at < to_us; at += 1000.0)
		{
			const Stick s = stickAt(at - delay_us);
			sum += answer_x(s.x, s.aiming) * 0.001;
		}
		return sum;
	}

	// One picture's time of play. `moving`: the thumb is on the stick. `noise`: motion of
	// the picture that the stick did not cause, widths a second.
	void step(bool moving, double noise = 0.0)
	{
		const double picture_us = t_us + 33333.0;
		// The controller reports every 4 ms.
		for(double at = t_us; at < picture_us; at += 4000.0)
		{
			phase += 0.004;
			// Two seconds: at rest, pushed over and held, then wandering.
			const double in_cycle = std::fmod(phase, 2.0);
			const int cycle = (int)(phase / 2.0);
			float x = 0, y = 0;
			if(moving && in_cycle >= 0.35 && in_cycle < 0.9)
				x = cycle % 2 ? -0.85f : 0.85f;
			else if(moving && in_cycle >= 0.9)
			{
				x = (float)(0.75 * std::sin(phase * 2.3) + 0.2 * std::sin(phase * 7.1));
				y = (float)(0.5 * std::sin(phase * 1.7 + 1.0));
			}
			stick_log.push_back({ at, x, y, aiming });
			warp.onStick(at, x, y, aiming ? 1 : 0);
		}
		// What the game made of the stick between the last picture and this one, late.
		const double from = last_picture_us > 0 ? last_picture_us : t_us;
		double vx = 0, vy = 0;
		int n = 0;
		for(double at = from; at < picture_us; at += 2000.0)
		{
			const Stick s = stickAt(at - delay_us);
			vx += answer_x(s.x, s.aiming);
			vy += answer_y(s.y, s.aiming);
			n++;
		}
		std::normal_distribution<double> wobble(0.0, 1.0);
		const double dt = (picture_us - from) / 1e6;
		const double dx = (vx / n + noise * wobble(random)) * dt;
		const double dy = (vy / n + noise * wobble(random)) * dt;
		if(last_picture_us > 0)
			warp.onMotion(last_picture_us, picture_us, (float)dx, (float)dy, true);
		last_picture_us = picture_us;
		t_us = picture_us;
	}

	void play(double seconds, bool moving, double noise = 0.0)
	{
		for(int i = 0; i < (int)(seconds * 30); i++)
			step(moving, noise);
	}

	// Plays on and holds what the warp says against what the picture then does, for every
	// picture: the root mean square of what a perfect warp would show, and of what is left.
	void score(double seconds, double *needed, double *left)
	{
		double sum_needed = 0, sum_left = 0;
		int n = 0;
		for(int i = 0; i < (int)(seconds * 30); i++)
		{
			step(true, 0.002);
			float dx = 0, dy = 0;
			warp.predict(t_us, t_us + 8000.0, &dx, &dy);
			// What the picture will have done by the time the stick's present place is in one.
			const double truth = trueShift(t_us, t_us + delay_us + 8000.0);
			sum_needed += truth * truth;
			sum_left += (dx - truth) * (dx - truth);
			n++;
		}
		*needed = std::sqrt(sum_needed / n);
		*left = std::sqrt(sum_left / n);
	}
};

} // namespace

PLK_TEST(camera_warp_learns_delay_and_a_stick_in_proportion)
{
	Game g;
	g.play(12.0, true, 0.002);
	const CameraWarp::State s = g.warp.state();
	CHECK(s.learned);
	// The first picture that moves after a push is at least the delay late, and at most a
	// picture's time more.
	// The thumb here never lets go from well over, so no stop times the delay: it is the
	// comparison's, less the 20 ms that a real camera's start adds to that (this made-up one
	// starts at once, so it comes out that much short).
	CHECK(s.delay_ms >= 65.0 && s.delay_ms <= 125.0);
	CHECK(s.explained > 0.8);
	double needed, left;
	g.score(6.0, &needed, &left);
	CHECK(needed > 0.01);
	CHECK(left < needed * 0.4);
	float dx = 0, dy = 0;
	g.warp.predict(g.t_us, g.t_us + 10000.0, &dx, &dy);
	CHECK(std::abs(dx) <= CameraWarp::kMaxShift + 1e-6 && std::abs(dy) <= CameraWarp::kMaxShift + 1e-6);
}

PLK_TEST(camera_warp_learns_a_dead_zone_a_curve_and_a_speed_that_rises)
{
	// As measured on a PS5 game: nothing up to a quarter of the way, then steeply more.
	Game g;
	g.answer_x = [](double s, bool) {
		const double a = std::abs(s);
		const double v = a < 0.25 ? 0.0 : 1.4 * std::pow((a - 0.25) / 0.75, 3.0);
		return s < 0 ? -v : v;
	};
	g.play(40.0, true, 0.002);
	double needed, left;
	g.score(8.0, &needed, &left);
	CHECK(left < needed * 0.45);
	// A stick held inside the dead zone moves nothing, and nothing is shown.
	for(int i = 0; i < 40; i++)
	{
		g.warp.onStick(g.t_us + i * 4000.0, 0.15f, 0.0f, 0);
	}
	float dx = 1, dy = 1;
	g.warp.predict(g.t_us + 160000.0, g.t_us + 168000.0, &dx, &dy);
	CHECK(std::abs(dx) < 0.002);
}

PLK_TEST(camera_warp_keeps_aiming_and_free_look_apart)
{
	// Aiming (L2) turns the camera a third as fast, and by another curve.
	Game g;
	g.answer_x = [](double s, bool aiming) { return aiming ? 0.27 * s * std::abs(s) : 0.8 * s; };
	g.play(15.0, true, 0.002);
	g.aiming = true;
	g.play(15.0, true, 0.002);
	double needed, left;
	g.score(5.0, &needed, &left);
	CHECK(g.warp.state().aiming);
	CHECK(left < needed * 0.45);
	// Back in free look what was learned of it is there at once: nothing has to be learned
	// again, and what aiming taught has not touched it.
	g.aiming = false;
	g.play(0.2, true, 0.002);
	g.score(3.0, &needed, &left);
	CHECK(!g.warp.state().aiming);
	CHECK(left < needed * 0.4);
}

PLK_TEST(camera_warp_stays_out_of_what_the_stick_does_not_move)
{
	Game g;
	g.play(12.0, true, 0.002);
	// A cutscene: the picture moves, and not by the stick.
	g.answer_x = [](double, bool) { return 0.0; };
	g.answer_y = [](double, bool) { return 0.0; };
	g.play(3.0, true, 0.4);
	// The stick moves nothing now: after the first push that showed it, the pushes are not
	// believed (a twentieth of the guess at most is let through until one moves the picture).
	float dx = 1, dy = 1;
	g.warp.predict(g.t_us, g.t_us + 10000.0, &dx, &dy);
	CHECK(std::abs(dx) < 0.006 && std::abs(dy) < 0.006);

	// Nothing before anything was seen, and nothing at strength 0.
	CameraWarp fresh;
	fresh.onStick(0, 1, 1);
	fresh.predict(100000, 110000, &dx, &dy);
	CHECK(dx == 0 && dy == 0);
	Game off;
	off.play(12.0, true, 0.002);
	off.warp.setStrength(0);
	off.warp.predict(off.t_us, off.t_us + 10000.0, &dx, &dy);
	CHECK(dx == 0 && dy == 0);
}

PLK_TEST(camera_warp_keeps_what_it_learned_between_sittings)
{
	Game g;
	g.play(15.0, true, 0.002);
	const std::string kept = g.warp.save();
	CHECK(kept.find("plk-camera-warp 2") == 0);
	// A new sitting: nothing has been seen, and the first push is already answered.
	Game again;
	CHECK(again.warp.load(kept));
	CHECK(again.warp.state().learned);
	for(int i = 0; i < 60; i++)
		again.warp.onStick(again.t_us + i * 4000.0, 0.85f, 0.0f, 0);
	float dx = 0, dy = 0;
	again.warp.predict(again.t_us + 240000.0, again.t_us + 248000.0, &dx, &dy);
	CHECK(std::abs(dx) > 0.02);
	// Not a record: nothing changes.
	CameraWarp fresh;
	CHECK(!fresh.load("something else"));
	CHECK(!fresh.state().learned);
}

// A camera that gathers speed over a tenth of a second and stops at once, as measured on a
// PS5 game: set against the stick as a whole it looks later than it is, and it is the stops
// that tell the delay.
PLK_TEST(camera_warp_times_the_delay_by_stops)
{
	CameraWarp warp;
	const double delay = 140000.0;
	double t = 1e6, last_picture = 0, speed = 0;
	std::vector<std::pair<double, float>> stick;
	auto stickAt = [&](double at) {
		float v = 0;
		for(const auto &e : stick)
		{
			if(e.first > at)
				break;
			v = e.second;
		}
		return v;
	};
	for(int push = 0; push < 30; push++)
	{
		const float to = push % 2 ? -0.9f : 0.9f;
		// 0.6 s over, 0.5 s at rest, a report every 4 ms and a picture every 33.
		for(int i = 0; i < 275; i++)
		{
			const float x = i < 150 ? to : 0.0f;
			stick.push_back({ t, x });
			// Told only when it changes, as a program at the controls tells it.
			if(i == 0 || i == 150)
				warp.onStick(t, x, 0.0f, 0);
			t += 4000.0;
			if(t - last_picture >= 33333.0)
			{
				// The camera's speed: towards 0.7 widths a second per unit of stick at 7 a
				// second squared, and to nothing at once.
				double moved = 0;
				for(double at = last_picture > 0 ? last_picture : t - 33333.0; at < t; at += 1000.0)
				{
					const double want = 0.7 * stickAt(at - delay);
					if(want == 0.0)
						speed = 0;
					else
						speed += std::max(-0.007, std::min(0.007, want - speed));
					moved += speed * 0.001;
				}
				if(last_picture > 0)
					warp.onMotion(last_picture, t, (float)moved, 0.0f, true);
				last_picture = t;
			}
		}
	}
	const CameraWarp::State s = warp.state();
	CHECK(s.learned);
	CHECK(s.delay_ms >= 125.0 && s.delay_ms <= 158.0);
}

// A wheel held with L1 is picked from with the stick: the camera does not turn then, nor
// after L1 is let go while the stick is still over.
PLK_TEST(camera_warp_leaves_a_wheel_alone)
{
	Game g;
	g.play(15.0, true, 0.002);
	double t = g.t_us;
	float dx = 0, dy = 0;
	// L1 held, the stick pushed over: mode 2, of which nothing has been learned.
	for(int i = 0; i < 100; i++, t += 4000.0)
		g.warp.onStick(t, 1.0f, 0.0f, 2);
	g.warp.predict(t, t + 8000.0, &dx, &dy);
	CHECK(dx == 0 && dy == 0);
	// L1 let go, the stick still over: not the camera's until it has been in the middle.
	for(int i = 0; i < 60; i++, t += 4000.0)
		g.warp.onStick(t, 1.0f, 0.0f, 0);
	g.warp.predict(t, t + 8000.0, &dx, &dy);
	CHECK(dx == 0 && dy == 0);
	for(int i = 0; i < 30; i++, t += 4000.0)
		g.warp.onStick(t, 0.0f, 0.0f, 0);
	for(int i = 0; i < 60; i++, t += 4000.0)
		g.warp.onStick(t, 0.85f, 0.0f, 0);
	g.warp.predict(t, t + 8000.0, &dx, &dy);
	CHECK(std::abs(dx) > 0.02);
}

// The camera swings by itself (behind a walking character, say) while the stick is at rest:
// that is not the stick's doing, and nothing is shown for it.
PLK_TEST(camera_warp_does_not_take_the_cameras_own_moves_for_the_sticks)
{
	Game g;
	g.play(12.0, true, 0.002);
	double t = g.t_us;
	g.warp.onStick(t, 0.0f, 0.0f, 0);
	for(int i = 0; i < 90; i++)
	{
		g.warp.onMotion(t, t + 33333.0, 0.012f, 0.0f, true);
		t += 33333.0;
	}
	float dx = 1, dy = 1;
	g.warp.predict(t, t + 8000.0, &dx, &dy);
	CHECK(dx == 0 && dy == 0);
}
