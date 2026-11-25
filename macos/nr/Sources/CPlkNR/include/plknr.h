// plknr: the neural-rendering engine's C interface (libplknr.dylib).
//
// One engine holds one loaded model. A frame is offered with plk_nr_submit(); the engine
// takes it only when it is idle and never queues, so a caller that offers every decoded
// frame gets the newest result the GPU can deliver and is never made to wait. The result
// arrives through the callback on a thread of the engine's own.
//
// Pixel buffers in and out are IOSurface-backed kCVPixelFormatType_32BGRA, non-linear
// R'G'B' code values in [0, 1] (what an SDR display is sent). Output has the input's size.
#pragma once

#include <CoreVideo/CoreVideo.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bumped when a struct or a function below changes incompatibly; the app refuses a
// library whose version differs.
#define PLK_NR_ABI_VERSION 1u

typedef struct plk_nr_engine plk_nr_engine;

// The look, as MLX-DLSS's rendering profiles name it.
enum {
	PLK_NR_PROFILE_STANDARD = 0,   // the strongest nominal profile
	PLK_NR_PROFILE_NATURAL = 1,    // gentler
	PLK_NR_PROFILE_CINEMATIC = 2,  // film-oriented
	PLK_NR_PROFILE_NEUTRAL = 3,    // tone and structure off (diagnostic)
};

// Controls that can change between frames.
typedef struct {
	float intensity;        // overall effect inside the network's own composition, 0...2
	float detail_strength;  // strength of fine changes, 0...8
	float colour_strength;  // strength of broad colour changes, 0...4
	int32_t profile;        // PLK_NR_PROFILE_*
	// 1: the network keeps its own history between the frames it is given (steadier
	// detail); 0: every frame is rendered on its own.
	int32_t temporal;
} plk_nr_controls;

typedef struct {
	uint32_t struct_size;    // sizeof(plk_nr_config)
	const char *model_path;  // a .dlssmodel directory
	int32_t half_precision;  // 1: float16 compute (the default of MLX-DLSS), 0: float32
	plk_nr_controls controls;
} plk_nr_config;

typedef struct {
	uint64_t seq;             // the value given to plk_nr_submit()
	CVPixelBufferRef output;  // NULL on error; valid during the callback, CFRetain to keep
	double total_ms;          // submit to result, wall clock
	const char *error;        // NULL on success; valid during the callback
} plk_nr_result;

typedef void (*plk_nr_callback)(void *context, const plk_nr_result *result);

#ifndef PLKNR_TYPES_ONLY

uint32_t plk_nr_abi_version(void);

// Loads the model (a few hundred milliseconds, plus kernel compilation on the first
// frame): call it off the render thread. Returns NULL and fills `error` on failure.
plk_nr_engine *plk_nr_create(const plk_nr_config *config, char *error, size_t error_size);

// A one-line description of the loaded model, valid until plk_nr_destroy().
const char *plk_nr_describe(const plk_nr_engine *engine);

// Offers a frame. Returns 1 when the engine took it: the callback is then called exactly
// once. Returns 0 when the engine is busy or the buffer is unusable: nothing happens and
// the callback is not called. Never blocks. `reset` != 0 tells the network that this
// frame does not continue the previous one (scene cut, a new stream). The engine is idle
// again before a result's callback is called, so the next frame can be offered from the
// callback or as soon as it has been seen to start; callbacks never overlap.
int plk_nr_submit(plk_nr_engine *engine, CVPixelBufferRef frame, uint64_t seq, int reset,
		plk_nr_callback callback, void *context);

// Takes effect with the next frame the engine takes.
void plk_nr_set_controls(plk_nr_engine *engine, const plk_nr_controls *controls);

// Waits for a frame in flight (its callback still runs), then frees the engine.
void plk_nr_destroy(plk_nr_engine *engine);

#endif  // PLKNR_TYPES_ONLY

#ifdef __cplusplus
}
#endif
