// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Camera warp: the picture on screen moved by what the right stick has asked for and the
// console has not shown yet.
//
// A picture that arrives shows what the game made of the controller a while ago: the way to
// the console, the game's own frame, the encoder and the way back are all behind it. Where
// the right stick turns the camera, the whole picture slides, and how far it will have slid
// by the time the stick's newest position is in a picture can be said now: the picture is
// shown moved by that much, and a turn is on screen when the stick is moved, not some
// frames later. It is a guess about pictures that have not come, and only about the slide:
// what comes into view at the edge is not there to show.
//
// Nothing here knows the game. What the stick does to the picture is learned while playing,
// from the stick itself and from how far each new picture moved against the one before it
// (the video pipeline measures that):
//
//   - how late: the time from the stick being let go, from well over, to the picture standing
//     still. A stop is sudden where a start is not (a camera gathers speed), so it tells the
//     delay itself. Measured on a PS5 game over the home network, 2026-11-27: 142 ms in free
//     look, about 155 aiming, 165 to 175 in its closer look; each view has its own. Until
//     three stops have been seen it is the delay at which the stick, put through a plain
//     curve, goes best with the picture's speed over the last half minute, less 20 ms: that
//     comparison takes a camera's slow start for delay.
//   - what shape: a game does not turn the camera in proportion to the stick. There is a
//     dead zone, a curve that keeps small movements fine and large ones fast, and often a
//     speed that rises while the stick is held over. So what is learned is a table: the
//     picture's speed for each of 24 steps of how far the stick is over and nine of how
//     long it has been out of the middle on that side (not: at that step. A thumb passes
//     through the steps, and a camera that is turning already does not start again). It is
//     read smoothly between the steps. Measured on that game, in percent of the picture's
//     width a second, free look: nothing up to half way, 16 at 62 %, 25 at 70 %, 55 at
//     79 %, 131 at 95 %, reached over about 150 ms; aiming with L2 a third of that.
//   - in which mode: a table and a delay for each combination of the buttons that change
//     what the stick does: L2 (aiming), L1 (a wheel to pick from: the stick turns nothing),
//     and a view switched to with R3 while L2 is held. After a wheel the stick is not the
//     camera's until it has been back in the middle. A change no button tells (a vehicle)
//     is followed by the table itself, a fifth of the way with each picture at a step.
//   - whether at all: how much of the picture's motion the stick explains, over seconds;
//     whether the push that is on moves the picture at all (in a menu, or with the camera
//     at the end of its way, it does not: the warp is let go, and the table left alone);
//     and whether turns can be measured as a slide at all (looking straight down they turn
//     the picture round its middle). Where any of these fails the warp fades out.
//
// What was learned is kept between sittings (save(), load()).
//
// Every method may be called from any thread.

#ifndef PLK_APP_CAMERAWARP_H
#define PLK_APP_CAMERAWARP_H

#include <array>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace plk::app {

class CameraWarp
{
public:
	/**
	 * The right stick, -1...1 each way (right and down are positive), at `t_us`. `mode`: which
	 * of the buttons that change what the stick does are held: 1 for L2 (aiming), 2 for L1
	 * (a wheel to pick from: the stick then turns nothing), 4 for a view switched to with
	 * R3 while L2 is held (a scope, a closer look), and their sums.
	 */
	void onStick(double t_us, float x, float y, int mode = 0);
	static constexpr int kModes = 8;
	/**
	 * A new picture arrived at `picture_us`, the one before it at `previous_us`, and what is
	 * in it moved by `dx` of the picture's width and `dy` of its height in between. Not
	 * `valid`: no common shift was found, and nothing is learned of this pair.
	 */
	void onMotion(double previous_us, double picture_us, float dx, float dy, bool valid);
	/**
	 * How far the picture that arrived at `picture_us` is to be shown moved when it is on
	 * screen at `display_us`: of its width and of its height.
	 */
	void predict(double picture_us, double display_us, float *dx, float *dy) const;

	/** How much of the guess is shown, 0...1. */
	void setStrength(float strength);
	/** The most the picture is moved, of its width or height (kMaxShift unless set). */
	void setMaxShift(double share) { std::lock_guard<std::mutex> lk(mutex_); max_shift_ = share; }

	struct State
	{
		double delay_ms = 0;        // how late the picture follows the stick
		int delays_seen = 0;        // 1 once the delay has been measured
		double gain_x = 0, gain_y = 0; // the picture's speed at full tilt, held, each way: widths and heights a second, signed
		double explained = 0;       // 0...1: how much of the picture's motion the stick explains
		int steps_learned = 0;      // steps of the tables in use that the pictures have filled in
		double full_tilt = 0;       // the table's speed sideways at full tilt, held: widths a second
		bool aiming = false;        // L2 is held
		int mode = 0;               // which pair of tables is in use
		bool learned = false;       // enough was seen to warp at all
	};
	State state() const;
	/** The tables and ratios as text, for a look at what was learned. */
	std::string describe() const;
	/**
	 * What was learned, as text to keep between sittings, and back from it. What comes back
	 * is used from the first push on, and each step of it is replaced outright by the first
	 * picture that says something of that step: another game may be in the console now.
	 * load(): false, and nothing changed, if the text is not such a record.
	 */
	std::string save() const;
	bool load(const std::string &text);

	// The limits, for tests and for what the panel says.
	static constexpr double kMaxShift = 0.16;  // of the width or height, either way
	static constexpr int kAmounts = 24;         // steps of how far over (a game's curve is steep: 55 to 131 % of the width a second between 79 and 95 % of the way, measured)
	static constexpr int kHolds = 9;            // steps of how long: to 35, 70, 105, 140, 175, 210, 300, 450 ms and beyond

private:
	struct Sample
	{
		double t_us;
		float x, y;
		int mode;
		bool dead;                      // the stick is not the camera's just now: see onStick()
		double since_x_us, since_y_us;  // since when the stick is out of the middle that side, each way
	};
	// One way of one mode: the table and the ratio that goes with it.
	struct Axis
	{
		std::array<std::array<double, kHolds>, kAmounts> speed{};
		std::array<std::array<uint32_t, kHolds>, kAmounts> seen{};
		std::array<std::array<bool, kHolds>, kAmounts> kept{};  // from a sitting before: the first picture replaces it
		bool any = false;          // the pictures have filled in at least one step
		double sxx = 0, sxy = 0, syy = 0;  // the table's speed squared, times the picture's, the picture's squared
		double explained = 0;
		// How much of what the table says of the push that is on the pictures have shown,
		// lately: 1 as told, 0 when the picture stands though the stick is over (the camera
		// is at the end of its way up or down, or against a wall, or the game is not
		// listening). The warp is let go then, or it would hold the picture moved for as long
		// as the stick is held. A new push starts believed again.
		double flow = 1.0;
		int learned() const;
	};
	/** The table's speed for a stick `amount` over (signed) for `held_us`. Call with mutex_ held. */
	double tableSpeed(const Axis &axis, double amount, double held_us) const;
	/**
	 * What the tables make of the stick over [from_us, to_us], each way: the mean of the
	 * table's speed with the stick's sign. Call with mutex_ held.
	 */
	void drive(double from_us, double to_us, double *x, double *y) const;
	void learn(Axis &axis, bool vertical, double from_us, double to_us, double speed, double dt_us);
	/** The delay in `mode` (-1: the mode the stick is in now): each view has its own (measured
	 *  2026-11-27 in one game: 142 ms in free look, 160 aiming, 165 to 171 in its closer look). */
	double delayUs(int mode = -1) const;

	mutable std::mutex mutex_;
	std::deque<Sample> stick_;
	Axis axes_[kModes][2];  // [mode][vertical]
	// The delay from all of play: for each delay that may be, how well a
	// plain curve of the stick, that long ago, goes with the picture's speed.
	static constexpr int kLags = 29;           // 40 ms to 320 ms in steps of 10
	struct Lag { double sxx[2] = { 0, 0 }, sxy[2] = { 0, 0 }; };
	std::array<Lag, kLags> lags_{};
	double lag_syy_[2] = { 0, 0 };
	bool delayKnown() const;
	// And the delay as a stop tells it: the stick let go from well over, and the picture
	// standing still that long after. A stop is sudden where a start is not (a camera gathers
	// speed), so it tells the delay itself: 135 to 150 ms where the start, and with it the
	// comparison above, said 162 to 166 (measured 2026-11-27); with the longer one the warp
	// kept the picture moved for a frame after the camera had stopped.
	double let_go_us_ = 0;         // when the stick was let go from well over; 0: no stop is waited for
	double well_over_us_ = 0;      // when the stick was last well over
	double speed_before_ = 0;      // the picture's speed while it was, widths a second
	int let_go_mode_ = 0;          // the mode it was let go in
	std::deque<double> stop_delays_us_[kModes];
	double delayNowUs() const;     // from the pictures of the last half minute
	double known_delay_us_ = 0;    // the last delay that was known: kept through a long rest
	double table_delay_us_[kModes] = {};  // the delay each mode's tables were filled in at
	float strength_ = 1.0f;
	double max_shift_ = kMaxShift;
	// How many of the pictures taken while the stick was over could be measured at all, lately
	// (0...1). Where the camera looks straight down or up, turning it turns the picture round
	// its middle: no one shift fits, the pictures say so, and a picture moved sideways there
	// is simply wrong.
	double measurable_ = 1.0;
	uint64_t pairs_ = 0;
};

} // namespace plk::app

#endif // PLK_APP_CAMERAWARP_H
