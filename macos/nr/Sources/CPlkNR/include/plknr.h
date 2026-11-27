// plknr: the neural-rendering engine's C interface (libplknr.dylib).
//
// One engine holds one loaded model. A frame is offered with plk_nr_submit(); the engine
// takes it only when it is idle and never queues, so a caller that offers every decoded
// frame gets the newest result the GPU can deliver and is never made to wait. The result
// arrives through the callback on a thread of the engine's own.
//
// Pixel buffers in and out are IOSurface-backed, non-linear R'G'B' in [0, 1] (what an SDR
// display is sent): kCVPixelFormatType_32BGRA, or kCVPixelFormatType_64RGBAHalf for a
// picture that 8 bits are too coarse for (the SDR rendition of an HDR picture). Output
// has the input's size and pixel format.
#pragma once

#include <CoreVideo/CoreVideo.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bumped when a struct or a function below changes incompatibly; the app refuses a
// library whose version differs.
#define PLK_NR_ABI_VERSION 4u

typedef struct plk_nr_engine plk_nr_engine;
typedef struct plk_fg_engine plk_fg_engine;

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
	// Frame generation only: which of the `count` frames between the two this one is, from
	// 0 on (the earliest first). 0 and 1 for a neural rendering result, 0 and 0 for a pair
	// that failed.
	uint32_t index, count;
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

// ---- frame generation -------------------------------------------------------------------
//
// NVIDIA's DLSS frame generation network: of two frames, the picture halfway between them.
// The weights are a .safetensors file made from the user's own nvngx_dlssg.dll (MLX-DLSS's
// `mlxdlss-weights extract-fg`). It shares the GPU with a plk_nr_engine in the same
// process: the two never evaluate at the same time, one waits for the other.

// Loads the weights: call it off the render thread. Returns NULL and fills `error` on failure.
plk_fg_engine *plk_fg_create(const char *weights_path, int half_precision, char *error, size_t error_size);

// Offers two frames of equal size and pixel format (kCVPixelFormatType_32BGRA or
// kCVPixelFormatType_64RGBAHalf), IOSurface-backed, the earlier one first, and asks for
// `count` frames evenly spaced between them (1: the one halfway; 3: at a quarter, a half
// and three quarters; up to 15). Returns 1 when they were taken: the callback is then
// called once for each generated frame, in order (the result's index and count say which),
// or exactly once without an output when the pair failed. Returns 0 when the generator is
// busy or the buffers are unusable. Never blocks.
int plk_fg_interpolate(plk_fg_engine *engine, CVPixelBufferRef previous, CVPixelBufferRef current, int count,
		uint64_t seq, plk_nr_callback callback, void *context);

// Waits for a pair in flight (its callback still runs), then frees the generator.
void plk_fg_destroy(plk_fg_engine *engine);

#endif  // PLKNR_TYPES_ONLY

#ifdef __cplusplus
}
#endif
