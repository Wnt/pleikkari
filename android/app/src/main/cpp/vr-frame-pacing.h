// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

// PLE-715: when the Go cinema's render loop (src/vr/cpp/vr-cinema.cpp) starts a frame, and what
// VrApi's frame scheduler did with the last one. Plain C with no VrApi, so chiaki-unit covers it.
//
// The Go runtime (VrDriver 18.0, in-process timewarp; PhaseSync needs out-of-process composition,
// which the Go does not have) releases vrapi_SubmitFrame2 at a fixed phase of each refresh and
// gives the frame a vsync slot, clamp(previous slot + 1, now, now + 2). The slot's lead over "now"
// carries over unchanged while the app submits once per refresh: lead 0 is VrApi's Prd=32 ms at
// 72 Hz, lead 1 its every-frame Early, Prd=46 ms. The loop latches the video frame right after
// the previous submit returns, so every frame waits most of a refresh inside the next submit.
// PLEIKKARI_VR_PACING_LATE moves that wait before the latch instead: the frame starts [budget]
// before the release VrApi would give it anyway, so its slot is unchanged and the video it shows
// is newer by the time moved.

#ifndef PLEIKKARI_VR_FRAME_PACING_H
#define PLEIKKARI_VR_FRAME_PACING_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum pleikkari_vr_pacing_mode_t
{
	PLEIKKARI_VR_PACING_VRAPI = 0, // start each frame as soon as the last submit returns (the default)
	PLEIKKARI_VR_PACING_LATE = 1,  // start each frame [budget] before VrApi's next release
} PleikkariVrPacingMode;

// A submit that waited at least this long inside vrapi_SubmitFrame2 returned at VrApi's own
// release, one refresh after the last one; a shorter wait means the frame came late, and the
// next frame starts at once so that VrApi's throttle anchors it again.
#define PLEIKKARI_VR_PACING_THROTTLED_NS 1000000LL
// The late start's default budget: the plain cinema's latch to submit is 1.9 ms p50, 2.3 ms p95
// (PLE-698), and the GPU needs about 0.5 ms (VrApi App=) before the timewarp takes the frame.
#define PLEIKKARI_VR_PACING_DEFAULT_BUDGET_NS 5000000LL
// Debug experiments: one-shot stalls at given frames.
#define PLEIKKARI_VR_PACING_MAX_STALLS 8

typedef struct pleikkari_vr_pacing_config_t
{
	PleikkariVrPacingMode mode;
	int64_t period_ns;
	int64_t budget_ns;
	// Debug experiments (a debug build's debug.pleikkari.vr_pacing property), all off by default.
	bool trace;               // one GoPacing line per frame
	int64_t sleep_ns;         // a fixed sleep at the top of every frame
	int64_t sweep_step_ns;    // added to that sleep every sweep_frames frames, up to sweep_max_ns
	uint32_t sweep_frames;
	int64_t sweep_max_ns;
	uint32_t stalls;
	uint64_t stall_frame[PLEIKKARI_VR_PACING_MAX_STALLS];
	int64_t stall_ns[PLEIKKARI_VR_PACING_MAX_STALLS];
} PleikkariVrPacingConfig;

// One second's figures, in ns; ahead is predicted display minus submit call, lead_ns predicted
// display minus the submit's return.
typedef struct pleikkari_vr_pacing_window_t
{
	uint32_t frames;
	uint32_t throttled;    // returned at VrApi's release
	uint32_t late;         // returned at once: the frame came after VrApi's release
	int64_t slept_sum_ns;  // the loop's own sleep before the frame started
	int64_t slept_max_ns;
	int64_t work_max_ns;   // frame start to the submit call
	int64_t work_sum_ns;
	int64_t wait_min_ns;   // inside vrapi_SubmitFrame2
	int64_t wait_max_ns;
	int64_t ahead_min_ns;
	int64_t ahead_max_ns;
	int64_t ahead_sum_ns;
	int64_t lead_min_ns;
	int64_t lead_max_ns;
	int64_t start_to_photon_sum_ns; // frame start (the video latch) to predicted display
} PleikkariVrPacingWindow;

typedef struct pleikkari_vr_pacing_t
{
	PleikkariVrPacingConfig config;
	uint64_t frames;         // frames recorded
	int64_t return_ns;       // the last submit's return; 0 before the first
	bool throttled;          // that submit waited for VrApi's release
	int64_t sweep_sleep_ns;  // the sweep's current sleep
	PleikkariVrPacingWindow window;
} PleikkariVrPacing;

void pleikkari_vr_pacing_config_default(PleikkariVrPacingConfig *config, float refresh_hz);
// "trace,sleep=US,sweep=STEP_US/FRAMES/MAX_US,stall=FRAME:US;FRAME:US,late,budget=US": the debug
// property's experiments, on top of the config. Unknown items are ignored; returns how many were read.
int pleikkari_vr_pacing_config_parse(PleikkariVrPacingConfig *config, const char *spec);

void pleikkari_vr_pacing_init(PleikkariVrPacing *pacing, const PleikkariVrPacingConfig *config);
// At the top of the loop, before the video latch: the CLOCK_MONOTONIC time to sleep until, or 0.
int64_t pleikkari_vr_pacing_wake_ns(PleikkariVrPacing *pacing, int64_t now_ns);
// After vrapi_SubmitFrame2 returns: start is the frame's start after any sleep, submit and
// returned bracket the submit call, predicted is its vrapi_GetPredictedDisplayTime.
void pleikkari_vr_pacing_frame(PleikkariVrPacing *pacing, int64_t start_ns, int64_t slept_ns,
		int64_t submit_ns, int64_t returned_ns, int64_t predicted_ns);
// The window so far, then a new one.
void pleikkari_vr_pacing_take_window(PleikkariVrPacing *pacing, PleikkariVrPacingWindow *out);

const char *pleikkari_vr_pacing_mode_name(PleikkariVrPacingMode mode);

#ifdef __cplusplus
}
#endif

#endif
