// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

// PLE-715: when the Go cinema's render loop (src/vr/cpp/vr-cinema.cpp) starts a frame, and what
// VrApi's frame scheduler did with the last one. Plain C with no VrApi, so chiaki-unit covers it.
//
// The Go runtime (VrDriver 18.0, in-process timewarp; PhaseSync needs out-of-process composition,
// which the Go does not have) releases vrapi_SubmitFrame2 at a fixed phase of each refresh and
// gives the frame a vsync slot, clamp(previous slot + 1, now, now + 2). The slot's lead over "now"
// carries over unchanged while the app submits once per refresh: lead 0 is VrApi's Prd=32 ms at
// 72 Hz, lead 1 its every-frame Early, Prd=46 ms. A submit that comes late is let through at once,
// and so is the next one in the same refresh: two submits in one refresh add a refresh of lead.
// On the Go (PLE-715) the predicted display time sits (lead + 1.38) refreshes after the release.
//
// The loop latches the video frame right after the previous submit returns, so every frame waits
// most of a refresh inside the next submit. PLEIKKARI_VR_PACING_LATE moves that wait before the
// latch instead: the frame starts [budget] before the release VrApi would give it anyway, so its
// slot is unchanged and the video it shows is newer by the time moved. The release grid comes from
// the predicted display times (release = predicted - (lead + 1.38) refreshes), so it holds when
// VrApi stops throttling after a late frame. After a late frame the next one waits for that point
// rather than going into the same refresh, and when every frame of half a second shows a refresh
// early, one release is skipped (a repeated frame) so the lead falls back to 0.
//
// PLEIKKARI_VR_PACING_HOLD (PLE-753) keeps the hold and the drain but not the late latch: a frame
// still starts as soon as the last submit returns, so a room (about 8 ms of GPU) keeps the whole
// refresh, and only a frame after a late one, or a drain, waits for a release on the grid.

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
	PLEIKKARI_VR_PACING_HOLD = 2,  // start at once, but hold after a late frame and drain a lead of 1
} PleikkariVrPacingMode;

// vrapi_SubmitFrame2 never returns in under about 2 ms on the Go. A submit that waited at least
// this long returned at VrApi's own release, which then anchors the release grid; a shorter wait
// means the frame came after the release and was let through at once.
#define PLEIKKARI_VR_PACING_THROTTLED_NS 3000000LL
// The late start's default budget, frame start to VrApi's release. PLE-715's sweep on the Go at
// 72 Hz: a frame is shown in its slot while its submit comes at least ~3 ms before the release;
// the plain cinema's start to submit is 2.6-2.9 ms mean with spikes past 5 ms.
#define PLEIKKARI_VR_PACING_DEFAULT_BUDGET_NS 7500000LL
// A frame started this close to the release or closer would come late (start to submit plus the
// ~3 ms the submit needs before the release); the late start and the hold aim at the next one.
#define PLEIKKARI_VR_PACING_MIN_START_NS 6000000LL
// The hold and drain starts a held frame at a release on the grid at least this far after now: a
// late submit returns at once, about 2 ms after the call, and the frame after it must not be
// submitted in the same refresh as that call.
#define PLEIKKARI_VR_PACING_HOLD_GUARD_NS 3000000LL
// The predicted display time minus a throttled submit's return, in refreshes, at lead 0 (measured
// on the Go at 72 and 60 Hz, PLE-715).
#define PLEIKKARI_VR_PACING_LEAD0_REFRESHES 1.38
// The late start and the hold and drain drain a lead of 1 or more after this many frames in a row
// at it, at most once per PLEIKKARI_VR_PACING_DRAIN_INTERVAL_NS.
#define PLEIKKARI_VR_PACING_DRAIN_FRAMES 36
#define PLEIKKARI_VR_PACING_DRAIN_INTERVAL_NS 2000000000LL
// Debug experiments: one-shot stalls at given frames.
#define PLEIKKARI_VR_PACING_MAX_STALLS 8

typedef struct pleikkari_vr_pacing_config_t
{
	PleikkariVrPacingMode mode;
	int64_t period_ns;
	int64_t budget_ns;
	// Debug experiments (a debug build's debug.pleikkari.vr_pacing property), all off by default.
	bool trace;               // one GoPacing line per frame
	bool hold;                // in PLEIKKARI_VR_PACING_VRAPI too: hold the frame after a late one
	uint32_t drain_refreshes; // the skip out of a lead of 1 or more (late start, hold and drain); 0 turns it off
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
	uint32_t leads[3];     // frames at lead 0, 1, and 2 or more
	uint32_t drains;       // releases skipped to drain the lead
	int64_t period_ns;     // the measured refresh period at the window's end
} PleikkariVrPacingWindow;

typedef struct pleikkari_vr_pacing_t
{
	PleikkariVrPacingConfig config;
	uint64_t frames;         // frames recorded
	int64_t return_ns;       // the last submit's return; 0 before the first
	bool throttled;          // that submit waited for VrApi's release
	int64_t release_ns;      // a VrApi release on the grid, from the last predicted display time; 0 before
	int64_t period_ns;       // the refresh period, measured from predicted display times
	int64_t predicted_ns;    // the last frame's predicted display time
	int lead;                // the last frame's lead in refreshes, -1 before the first
	uint32_t lead_frames;    // frames in a row at a lead of 1 or more
	int64_t drain_ns;        // the last drain, 0 for none
	bool draining;           // the frame being started skips a release
	int64_t sweep_sleep_ns;  // the sweep's current sleep
	PleikkariVrPacingWindow window;
} PleikkariVrPacing;

void pleikkari_vr_pacing_config_default(PleikkariVrPacingConfig *config, float refresh_hz);
// "trace,hold,drain=N,sleep=US,sweep=STEP_US/FRAMES/MAX_US,stall=FRAME:US;FRAME:US,late,holddrain,budget=US":
// the debug property's experiments, on top of the config. Unknown items are ignored; returns how many were read.
int pleikkari_vr_pacing_config_parse(PleikkariVrPacingConfig *config, const char *spec);

void pleikkari_vr_pacing_init(PleikkariVrPacing *pacing, const PleikkariVrPacingConfig *config);
// At the top of the loop, before the video latch: the CLOCK_MONOTONIC time to sleep until, or 0.
int64_t pleikkari_vr_pacing_wake_ns(PleikkariVrPacing *pacing, int64_t now_ns);
// VrApi's next release after now on the measured grid, or 0 before the first throttled submit.
int64_t pleikkari_vr_pacing_next_release_ns(const PleikkariVrPacing *pacing, int64_t now_ns);
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
