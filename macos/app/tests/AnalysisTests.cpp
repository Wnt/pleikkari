// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "FrameAnalysis.h"
#include "TestHarness.h"

#include <cmath>

using namespace plk;
using namespace plk::app;

namespace {

bool near(double a, double b, double tolerance = 0.01)
{
	return std::fabs(a - b) <= tolerance;
}

// A frame that reached the decoder at `submit_us`: complete 0.2 ms earlier, decoded in
// 3 ms, 0.1 ms in the queue, 0.3 ms to commit, 0.1 ms until the GPU starts, `gpu_ms` on the
// GPU, on glass 4 ms after that.
video::FrameTiming frame(double submit_us, double gpu_ms = 1.0, double age_ms = -1, bool presented = true)
{
	video::FrameTiming t;
	t.readyUs = submit_us - 200;
	t.submitUs = submit_us;
	t.decodedUs = t.submitUs + 3000;
	t.renderStartUs = t.decodedUs + 100;
	t.committedUs = t.renderStartUs + 300;
	t.gpuStartUs = t.committedUs + 100;
	t.gpuEndUs = t.gpuStartUs + gpu_ms * 1000;
	t.presented = presented;
	t.presentedUs = presented ? t.gpuEndUs + 4000 : 0;
	t.enhanceAgeMs = age_ms;
	return t;
}

constexpr double kFrameUs = 1e6 / 60.0;

} // namespace

PLK_TEST(analysis_stages_add_up)
{
	FrameAnalysis a;
	double at = 1e6;
	for(int i = 0; i < 120; i++, at += kFrameUs)
		a.onFrame(frame(at));
	const AnalysisSnapshot s = a.snapshot(at);
	// One second of 60 fps.
	CHECK(s.frames == 60 || s.frames == 59 || s.frames == 61);
	CHECK(near(s.budget_ms, 16.667, 0.01));
	CHECK(near(s.fps, 60.0, 0.05));
	CHECK(near(s.low1_fps, 60.0, 0.05));
	CHECK(near(s.stage[AnalysisSnapshot::ToDecoder].mean_ms, 0.2));
	CHECK(near(s.stage[AnalysisSnapshot::Decode].mean_ms, 3.0));
	CHECK(near(s.stage[AnalysisSnapshot::Queue].mean_ms, 0.1));
	CHECK(near(s.stage[AnalysisSnapshot::Render].mean_ms, 0.3));
	CHECK(near(s.stage[AnalysisSnapshot::GpuWait].mean_ms, 0.1));
	CHECK(near(s.stage[AnalysisSnapshot::Gpu].mean_ms, 1.0));
	CHECK(near(s.stage[AnalysisSnapshot::Scanout].mean_ms, 4.0));
	// The stages are consecutive: they add up to the two sums.
	double sum = 0;
	for(int i = 0; i < AnalysisSnapshot::kStages; i++)
		sum += s.stage[i].mean_ms;
	CHECK(near(s.total.mean_ms, sum));
	CHECK(near(s.processing.mean_ms, sum - 4.0));
	CHECK(near(s.total.p95_ms, sum));
	CHECK(s.not_presented == 0 && !s.offscreen);
	CHECK(!s.enhancer_active && !s.baseline && s.enhanced_share == 0 && s.results_per_s == 0);
	CHECK(s.nr_network.samples == 0 && s.nr_age.samples == 0);
	CHECK(s.intervals_ms.size() >= 100 && near(s.intervals_ms.back(), 16.667, 0.01));

	const std::string line = formatAnalysisLine(s);
	CHECK(line.find("analysis: ") == 0);
	CHECK(line.find(" decode=3.00") != std::string::npos);
	CHECK(line.find(" total=8.70") != std::string::npos);
	CHECK(line.find("nr:") == std::string::npos);
}

PLK_TEST(analysis_baseline_shows_what_the_enhancer_costs)
{
	FrameAnalysis a;
	double at = 1e6;
	// Two seconds without the enhancer: the GPU takes 1 ms.
	for(int i = 0; i < 120; i++, at += kFrameUs)
		a.onFrame(frame(at, 1.0));
	CHECK(!a.snapshot(at).baseline);

	// Switched on: a result every third frame, the GPU pass takes 1.5 ms, and every frame
	// carries a result that is 40 ms old.
	a.setEnhancerActive(true);
	for(int i = 0; i < 120; i++, at += kFrameUs)
	{
		a.onFrame(frame(at, 1.5, 40.0));
		if(i % 3 == 0)
		{
			video::EnhanceTiming e;
			e.ok = true;
			e.inputGpuMs = 0.4;
			e.enhancerMs = 44.0;
			e.totalMs = 45.0;
			a.onEnhance(e, at);
		}
	}
	AnalysisSnapshot s = a.snapshot(at);
	CHECK(s.enhancer_active && s.baseline);
	CHECK(near(s.stage[AnalysisSnapshot::Gpu].mean_ms, 1.5));
	CHECK(near(s.stage[AnalysisSnapshot::Gpu].baseline_ms, 1.0));
	CHECK(near(s.stage[AnalysisSnapshot::Decode].baseline_ms, 3.0));
	CHECK(near(s.total.mean_ms - s.total.baseline_ms, 0.5));
	CHECK(near(s.processing.mean_ms - s.processing.baseline_ms, 0.5));
	CHECK(near(s.enhanced_share, 1.0));
	CHECK(near(s.results_per_s, 20.0, 1.0));
	CHECK(near(s.nr_input.mean_ms, 0.4) && near(s.nr_network.mean_ms, 44.0) && near(s.nr_round_trip.mean_ms, 45.0));
	CHECK(near(s.nr_age.mean_ms, 40.0));
	const std::string line = formatAnalysisLine(s);
	CHECK(line.find("| nr: input=0.40 network=44.00 round_trip=45.00 age=40.00") != std::string::npos);
	CHECK(line.find("| with nr off:") != std::string::npos);

	// Off again: no comparison is shown, and the baseline is measured anew only once a
	// whole second has passed without the enhancer.
	a.setEnhancerActive(false);
	for(int i = 0; i < 30; i++, at += kFrameUs)
		a.onFrame(frame(at, 2.0));
	a.setEnhancerActive(true);
	for(int i = 0; i < 6; i++, at += kFrameUs)
		a.onFrame(frame(at, 2.5));
	s = a.snapshot(at);
	CHECK(s.baseline && near(s.stage[AnalysisSnapshot::Gpu].baseline_ms, 1.0)); // half a second was not enough
	a.setEnhancerActive(false);
	for(int i = 0; i < 150; i++, at += kFrameUs)
		a.onFrame(frame(at, 2.0));
	a.setEnhancerActive(true);
	a.onFrame(frame(at, 2.5));
	s = a.snapshot(at + 1);
	CHECK(s.baseline && near(s.stage[AnalysisSnapshot::Gpu].baseline_ms, 2.0));

	// Another scaler: what was measured with the old one says nothing any more.
	a.clearBaseline();
	CHECK(!a.snapshot(at + 1).baseline);
}

PLK_TEST(analysis_counts_what_was_not_shown)
{
	FrameAnalysis a;
	double at = 1e6;
	for(int i = 0; i < 60; i++, at += kFrameUs)
		a.onFrame(frame(at, 1.0, -1, i % 10 != 0)); // every tenth frame is not presented
	AnalysisSnapshot s = a.snapshot(at);
	CHECK(s.not_presented >= 5 && s.not_presented <= 6);
	// The gap a missing frame leaves is two frame intervals: the 1 % low sees it.
	CHECK(near(s.low1_fps, 30.0, 0.1));
	CHECK(s.fps < 60.0 && s.fps > 50.0);
	CHECK(s.stage[AnalysisSnapshot::Scanout].samples == s.frames - s.not_presented);

	// A failed result is counted, not averaged.
	a.setEnhancerActive(true);
	video::EnhanceTiming e;
	e.ok = false;
	a.onEnhance(e, at);
	s = a.snapshot(at);
	CHECK(s.nr_failed == 1 && s.nr_network.samples == 0 && s.results_per_s == 0);

	// A headless run: nothing is presented, the frame rate is that of the GPU's ends.
	FrameAnalysis off;
	at = 1e6;
	for(int i = 0; i < 90; i++, at += kFrameUs)
	{
		video::FrameTiming t = frame(at, 1.0, -1, false);
		t.offscreen = true;
		off.onFrame(t);
	}
	s = off.snapshot(at);
	CHECK(s.offscreen && s.not_presented == 0);
	CHECK(near(s.fps, 60.0, 0.05));
	CHECK(s.stage[AnalysisSnapshot::Scanout].samples == 0 && s.total.samples == 0);
	CHECK(near(s.processing.mean_ms, 4.7));
	CHECK(formatAnalysisLine(s).find(" scanout=-") != std::string::npos);
}
