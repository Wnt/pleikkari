// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// What each stage of a frame's way costs, while the stream runs: the numbers behind the
// analysis panel (key I) and the "analysis:" line of the log.
//
// The pipeline reports every rendered frame (video::FrameTiming) and every result of the
// enhancer (video::EnhanceTiming). From those this keeps, over the last second:
//
//   the stages of a frame, consecutive, adding up to "frame complete -> on glass"
//     to decoder   frame complete (the lib reassembled it) -> VideoPipeline::submitFrame()
//     decode       -> VideoToolbox output
//     queue        -> the render thread took it
//     render       -> command buffer committed (CPU)
//     GPU wait     -> the GPU started on it
//     GPU          -> the GPU finished it
//     scanout      -> on glass (compositor, wait for the display)
//   the enhancer, which runs beside the stream and never holds a frame back
//     input pass   GPU time of drawing the frame the network is given
//     network      what the engine reports for one frame
//     round trip   frame handed over -> result back
//     result age   how old the result mixed into a frame is, at that frame
//
// and the frame rate, its 1 % low (from the last ten seconds) and the last present
// intervals. The frame budget is the stream's own frame interval, measured from the frames'
// arrival.
//
// The enhancer costs the stream nothing directly; what it can cost is GPU time taken from
// the frame's own pass. To show that, the means of the last full second in which the
// enhancer was off are kept as a baseline, and a stage carries it next to its value while
// the enhancer is on.

#ifndef PLK_APP_FRAMEANALYSIS_H
#define PLK_APP_FRAMEANALYSIS_H

#include <plk/VideoPipeline.h>

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace plk::app {

struct StageCost
{
	double mean_ms = 0, p95_ms = 0;
	uint64_t samples = 0;     // 0: not measured in the window
	double baseline_ms = -1;  // the mean with the enhancer off; negative: not known
};

struct AnalysisSnapshot
{
	enum Stage { ToDecoder, Decode, Queue, Render, GpuWait, Gpu, Scanout, kStages };
	StageCost stage[kStages];
	StageCost processing;  // frame complete -> the GPU finished it: the app's own share
	StageCost total;       // frame complete -> on glass
	StageCost nr_input, nr_network, nr_round_trip, nr_age;
	double budget_ms = 0;        // the stream's frame interval
	double fps = 0;              // frames on glass per second (headless: rendered)
	double low1_fps = 0;         // from the 99th percentile interval of the last ten seconds
	double enhanced_share = 0;   // of the window's frames: the ones with a result mixed in
	double results_per_s = 0;
	uint64_t frames = 0, not_presented = 0, nr_failed = 0;
	bool offscreen = false;      // headless: nothing is presented, scanout is not measured
	bool enhancer_active = false;
	bool baseline = false;       // the stages carry baseline_ms
	std::vector<float> intervals_ms;  // the last present intervals, oldest first
};

/** What the frame rate overlay shows (FrameHud.h): asked ten times a second, so a small part of a snapshot. */
struct PaceSnapshot
{
	double fps = 0;        // frames on glass over the last second (headless: rendered)
	double frame_ms = 0;   // the mean of the last quarter second's intervals
	// What a frame took from being complete off the network to being on glass (headless: to
	// the GPU having finished it), as the mean of the last quarter second: decoding, a wait
	// for the neural rendering, rendering, scan-out. 0: not known yet.
	double latency_ms = 0;
	// Neural rendering over the last second. nr_result_ms: what a result takes; 0: there
	// was none. nr_age_ms: how old the result on a frame was; 0: each frame's own.
	double nr_result_ms = 0, nr_age_ms = 0;
	double budget_ms = 0;  // the stream's frame interval; 0: not known yet
	std::vector<float> intervals_ms;  // the last present intervals, oldest first
};

const char *stageName(AnalysisSnapshot::Stage stage);

/** The sizes a frame goes through, and what it is: for the panels' own wording. */
struct SignalInfo
{
	int stream_width = 0, stream_height = 0;    // 0: no frame yet
	int nr_width = 0, nr_height = 0;            // what the neural rendering is given; 0: nothing
	int picture_width = 0, picture_height = 0;  // the picture on the target
	int target_width = 0, target_height = 0;    // the whole target (the window's pixels)
	int bit_depth = 0;
	bool hdr = false;
	std::string codec, scaler, pacing;
};

/** What the panel shows: the numbers, and the words around them. */
struct AnalysisReport
{
	AnalysisSnapshot timing;
	SignalInfo info;
	std::vector<std::pair<std::string, std::string>> signal;  // label, value
	std::string enhancer;  // one line about the enhancer: what it is working with
	std::string status;
	bool warning = false;  // the status says something is not keeping up
};

/** "analysis: ..." for the log: the stage means of the snapshot on one line. */
std::string formatAnalysisLine(const AnalysisSnapshot &s);

class FrameAnalysis
{
public:
	explicit FrameAnalysis(double window_s = 1.0);

	/** The pipeline's callbacks: any thread, quick. */
	void onFrame(const video::FrameTiming &t);
	void onEnhance(const video::EnhanceTiming &t, double now_us);

	/** The enhancer is attached and switched on: frames from now do not count as baseline. */
	void setEnhancerActive(bool active);
	/** Something else that changes the cost of a frame was switched (the scaler, HDR). */
	void clearBaseline();

	AnalysisSnapshot snapshot(double now_us) const;
	PaceSnapshot pace(double now_us) const;

private:
	struct Frame
	{
		double at_us = 0;  // on glass, or the GPU's end for a frame that was not presented
		float stage[AnalysisSnapshot::kStages];
		float processing = -1, total = -1, interval = -1, age = -1;
		bool presented = false, offscreen = false;
	};
	struct Result
	{
		double at_us = 0;
		float input = 0, network = 0, round_trip = 0;
		bool ok = false;
	};
	struct Baseline
	{
		bool valid = false;
		double stage[AnalysisSnapshot::kStages];
		double processing = -1, total = -1;
	};
	void updateBaseline(double now_us);

	const double window_us_;
	mutable std::mutex mutex_;
	std::deque<Frame> frames_;      // ten seconds
	std::deque<Result> results_;    // ten seconds
	double budgetLocked() const;
	uint16_t last_index_ = 0;
	std::deque<float> arrivals_;    // intervals between frames reaching the decoder
	double last_at_us_ = 0, last_submit_us_ = 0, last_result_us_ = 0, last_baseline_check_us_ = 0;
	double inactive_since_us_ = 0;  // 0 while the enhancer is active
	bool active_ = false;
	Baseline baseline_;
};

} // namespace plk::app

#endif // PLK_APP_FRAMEANALYSIS_H
