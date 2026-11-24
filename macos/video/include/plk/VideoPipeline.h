// VideoPipeline: compressed access units in, photons out.
//
// Native macOS video path for the PS5 Remote Play client: VideoToolbox hardware
// decode -> CVMetalTextureCache (zero copy) -> one Metal render pass (YCbCr->RGB,
// scaling, letterbox) -> CAMetalLayer, presented either immediately on arrival or
// at the next vsync through CAMetalDisplayLink. Never more than one decoded frame
// waits for the display; an older frame is dropped (and counted) when a newer one
// is decoded first.
//
// Threads
//   - submitConfig()/submitFrame() may be called from any one thread at a time
//     (the network thread). They never wait for the GPU or the display; with the
//     default asynchronous decode they return as soon as VideoToolbox has queued
//     the access unit.
//   - Decoded frames arrive on a VideoToolbox thread; rendering happens on one
//     internal render thread (QoS user-interactive) that also hosts the
//     CAMetalDisplayLink.
//   - Every other method is thread-safe. The error callback is invoked from the
//     submitting thread or a VideoToolbox thread, never with an internal lock
//     held; it may call stats() but should only *schedule* an IDR request.
//
// Clocks
//   All internal timestamps are mach_absolute_time() (the CACurrentMediaTime()
//   base, which excludes system sleep). frame_ready_us from chiaki is
//   CLOCK_MONOTONIC, which on macOS *includes* sleep and differs from
//   mach_absolute_time by the accumulated sleep time (hours on a laptop). The
//   pipeline translates it at submit time by sampling both clocks together.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace plk::video {

enum class Codec { Auto, H264, HEVC };

enum class PacingMode {
	// Render and present as soon as a frame is decoded; CAMetalLayer display
	// sync off. The compositor shows the newest drawable at its next refresh
	// (and, on ProMotion, refreshes on demand). Lowest latency measured.
	Immediate,
	// Same, with displaySyncEnabled = YES: the drawable waits for a vsync in
	// the layer's swap queue. No tearing in direct-to-display fullscreen;
	// measured ~18 ms slower than Immediate in a window.
	ImmediateVsync,
	// Present the newest decoded frame at each CAMetalDisplayLink update
	// (preferredFrameLatency 1, frame rate range pinned to displayRefreshHz).
	// Measured in a window: presents ~41.6 ms after the callback.
	DisplayLink,
};

enum class Scaler {
	Bilinear,  // one pass, hardware bilinear (luma and chroma)
	Bicubic,   // one pass, Catmull-Rom on luma and chroma (9 bilinear taps each)
	Lanczos,   // one pass, Lanczos-3 on luma (36 taps, anti-ringing), Catmull-Rom chroma
	MetalFX,   // YCbCr->RGB at source size, MTLFXSpatialScaler, copy into drawable.
	           // Upscaling only: falls back to Bilinear when the target is smaller.
};

enum class ErrorKind {
	BadConfig,            // parameter sets could not be parsed / format description failed
	SessionCreateFailed,  // VTDecompressionSessionCreate failed (no hardware decoder?)
	DecodeFailed,         // VideoToolbox rejected or failed to decode a frame
	SessionInvalidated,   // kVTInvalidSessionErr (sleep/wake, GPU reset); session rebuilt
	NoConfig,             // a frame arrived before any usable parameter sets
};

struct Error {
	ErrorKind kind;
	int32_t status;       // OSStatus when applicable, else 0
	bool requestIdr;      // true: ask the console for a new IDR now
	std::string message;
};
using ErrorCallback = std::function<void(const Error &)>;

struct PipelineConfig {
	Codec codec = Codec::Auto;          // Auto: detected from the parameter sets
	PacingMode pacing = PacingMode::Immediate;
	Scaler scaler = Scaler::Bilinear;
	float displayRefreshHz = 120.0f;    // DisplayLink frame rate range (min=max=preferred)
	// DisplayLink only: if no new frame is waiting when the display link fires,
	// wait for one until (targetTimestamp - latchMarginUs) before giving up on
	// this refresh. 0 disables the wait (render only what is already decoded).
	uint32_t latchMarginUs = 0;
	int maxDrawables = 3;               // CAMetalLayer.maximumDrawableCount (2 or 3)
	bool asyncDecode = true;            // kVTDecodeFrame_EnableAsynchronousDecompression
	// After a decode error or a rebuilt session, drop P-frames until the next
	// IDR instead of showing a corrupted picture.
	bool waitForIdrAfterError = true;
	// Request the decoder's lossless-compressed output formats (&8v0 / &xv0)
	// instead of plain bi-planar 420v / x420. Experimental: falls back to the
	// plain formats if Metal cannot wrap the compressed planes.
	bool compressedDecoderOutput = false;
	// > 0: render into a private offscreen texture of this size instead of the
	// layer's drawables and never present (GPU-time benchmarks at panel size
	// without covering the screen). Pacing is forced to Immediate.
	int offscreenWidth = 0;
	int offscreenHeight = 0;
	uint32_t statsWindow = 36000;       // samples kept per distribution (rolling)
};

struct Distribution {
	uint64_t count = 0;
	double mean = 0, p50 = 0, p95 = 0, p99 = 0, max = 0;  // milliseconds
};

struct Stats {
	// Counters since creation or the last resetStats().
	uint64_t configsSubmitted = 0, configChanges = 0, sessionsCreated = 0, sessionRecoveries = 0;
	uint64_t framesSubmitted = 0, framesDecoded = 0, framesRendered = 0, framesPresented = 0;
	uint64_t framesSuperseded = 0;       // decoded, replaced by a newer frame before it was rendered
	uint64_t framesNotPresented = 0;     // rendered, but presentedTime == 0 (compositor skipped it)
	uint64_t framesSkippedAwaitingIdr = 0, framesRecovered = 0, framesLostReported = 0;
	uint64_t decodeErrors = 0, idrRequests = 0;
	uint64_t displayLinkTicks = 0, ticksWithoutNewFrame = 0, latchWaits = 0, latchHits = 0;

	Distribution submitCallMs;         // time spent inside submitFrame() (network thread blocked)
	Distribution decodeMs;             // submitFrame() entry -> decoder output callback
	Distribution queueMs;              // decoder output -> render start (thread hop / vsync wait)
	Distribution renderCpuMs;          // render start -> command buffer committed
	Distribution renderGpuMs;          // command buffer GPUEndTime - GPUStartTime
	Distribution submitToPresentedMs;  // submitFrame() entry -> drawable presentedTime
	Distribution readyToPresentedMs;   // frame_ready_us (translated) -> presentedTime
	Distribution decodedToPresentedMs; // decoder output -> presentedTime
	Distribution submitIntervalMs;     // between consecutive submitFrame() calls
	Distribution presentIntervalMs;    // between consecutive presented frames
	Distribution linkLeadMs;           // DisplayLink: callback -> targetPresentationTimestamp
	Distribution linkDeadlineMs;       // DisplayLink: callback -> targetTimestamp
	Distribution presentVsTargetMs;    // DisplayLink: presentedTime - targetPresentationTimestamp

	// Current stream / pipeline state.
	int width = 0, height = 0, bitDepth = 0;
	bool hdr = false, hardwareDecoder = false;
	std::string codec, pixelFormat, colorDescription, scaler, pacing;
	int targetWidth = 0, targetHeight = 0;
};

class VideoPipeline {
public:
	// `metalLayer` is a CAMetalLayer* (bridged: `(__bridge void *)layer`). The
	// pipeline sets its device, pixel format, colour space, EDR flags and
	// drawable count; the caller keeps it in the view hierarchy and reports size
	// changes with setDrawableSize(). Returns nullptr (and fills *error) if there
	// is no Metal device or the shaders fail to compile.
	static std::unique_ptr<VideoPipeline> create(void *metalLayer, const PipelineConfig &config,
			ErrorCallback onError, std::string *error = nullptr);
	// Teardown: waits for in-flight decodes and GPU work, stops the render thread.
	~VideoPipeline();

	// Parameter sets (VPS/SPS/PPS or SPS/PPS) as Annex-B. Identical repeats are
	// ignored; a change rebuilds the format description and decoder session and
	// the next frame must be an IDR.
	bool submitConfig(const uint8_t *data, size_t size);
	// One Annex-B access unit. Returns false only on immediate failure (no
	// session, unparsable, rejected by VideoToolbox); asynchronous decode
	// failures go to the error callback. In-band parameter sets are handled like
	// submitConfig(). The data is copied; the caller may reuse the buffer.
	bool submitFrame(const uint8_t *data, size_t size, uint16_t frameIndex, uint64_t frameReadyUs,
			bool recovered, int32_t framesLost = 0);

	void setPacingMode(PacingMode mode);
	void setScaler(Scaler scaler);
	void setLatchMarginUs(uint32_t marginUs);
	void setDrawableSize(int widthPixels, int heightPixels);

	Stats stats() const;
	void resetStats();

	// Debug/test hooks.
	void debugInvalidateSession();  // next decode sees kVTInvalidSessionErr
	// Offscreen mode only: read back pixel (x, y) of the last rendered frame as
	// normalized RGBA. Blocks until the render thread has done it.
	bool debugReadPixel(int x, int y, float rgba[4]);

	struct Impl;
private:
	explicit VideoPipeline(std::unique_ptr<Impl> impl);
	std::unique_ptr<Impl> impl_;
};

const char *toString(PacingMode mode);
const char *toString(Scaler scaler);
const char *toString(Codec codec);
bool parsePacingMode(const std::string &s, PacingMode *out);
bool parseScaler(const std::string &s, Scaler *out);

}  // namespace plk::video
