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
	// (and, on ProMotion, refreshes on demand). Lowest latency measured. In
	// fullscreen the system can hand the layer straight to the display, and
	// a frame is then switched in wherever the refresh is: the picture tears.
	Immediate,
	// Same, with displaySyncEnabled = YES: the drawable waits for a vsync in
	// the layer's swap queue. No tearing in direct-to-display fullscreen;
	// measured 2.5-20 ms slower than Immediate in a window, depending on
	// what else the compositor was showing. Straight to the display
	// (fullscreen, built-in 120 Hz panel, 60 fps with a console's jitter):
	// a frame is on screen 6 ms after its GPU work at the median and 11 ms at
	// p95, against 1.1 and 2.8 ms for Immediate. The panel takes a new frame
	// every 4.17 ms once the last one has had 8.33 ms, but the system shows
	// none sooner than about 3 ms after the GPU work, and most later than
	// the first chance. presentDrawable:afterMinimumDuration:, :atTime: and
	// presenting right after the commit all measured the same.
	ImmediateVsync,
	// Present the newest decoded frame at each CAMetalDisplayLink update
	// (preferredFrameLatency 1, frame rate range pinned to displayRefreshHz).
	// Measured in a window on macOS 27: targetPresentationTimestamp is 41.6 ms
	// (5 refreshes) after the callback; 40-50 ms submit-to-presented.
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

// One rendered frame's path through the pipeline. Times are mach_absolute_time in
// microseconds (hostTimeUs(), the CACurrentMediaTime base); 0 = not reached.
struct FrameTiming {
	uint16_t frameIndex = 0;
	bool keyframe = false;
	bool offscreen = false;    // rendered into the offscreen target, never presented
	bool presented = false;    // the drawable reported a presentation time
	double readyUs = 0;        // frame_ready_us of submitFrame(), translated to this clock
	double submitUs = 0;       // submitFrame() entry
	double decodedUs = 0;      // decoder output callback
	double renderStartUs = 0;  // render thread took the frame
	double committedUs = 0;    // command buffer committed (with presentDrawable)
	double gpuStartUs = 0;     // command buffer GPUStartTime
	double gpuEndUs = 0;       // command buffer GPUEndTime
	double presentedUs = 0;    // MTLDrawable.presentedTime (on glass), 0 if not presented
};
// Called once per rendered frame when both its GPU completion and its presentation
// (or the lack of one) are known, on a Metal callback thread. Must return quickly and
// must not call into the pipeline. Never called after ~VideoPipeline() has returned.
using FrameCallback = std::function<void(const FrameTiming &)>;

// mach_absolute_time() in microseconds: the clock of FrameTiming and Stats.
double hostTimeUs();
// A CLOCK_MONOTONIC microsecond time (chiaki's frame_ready_us; on macOS it includes
// sleep) translated to hostTimeUs(), the way submitFrame() does it.
double monotonicToHostUs(uint64_t monotonicUs);

struct Error {
	ErrorKind kind;
	int32_t status;       // OSStatus when applicable, else 0
	bool requestIdr;      // true: ask the console for a new IDR now
	std::string message;
};
using ErrorCallback = std::function<void(const Error &)>;

enum class HdrOutput {
	// BT.2020 + PQ code values in a BGR10A2 drawable, layer colour space
	// ITU-R 2100 PQ, CAEDRMetadata HDR10: the compositor tone maps to the
	// panel's current EDR headroom. No transfer math in the shader.
	PQ,
	// The shader decodes PQ to linear light (1.0 = 100 nits) into an
	// RGBA16Float drawable tagged extended-linear BT.2020, still with HDR10
	// CAEDRMetadata (opticalOutputScale 100). Alternative compositing path,
	// kept for A/B latency measurements.
	ExtendedLinear,
};

// How an SDR (BT.709) picture's colours are shown. HDR pictures are not affected.
enum class SdrColor {
	// The code values are taken as sRGB and colour-managed to the display: the colours
	// the console means.
	Accurate,
	// The code values are taken as Display P3: on a wide-gamut panel (every recent
	// MacBook and Apple display) each primary lands on the panel's own, more saturated
	// one. This is what a renderer without colour management shows: the Qt client's
	// OpenGL path, where a game picture measured about 12 % more saturated than the
	// accurate one. On an sRGB display the two look the same.
	Vivid,
};

// Who fits an HDR (PQ) picture to what the display can show right now.
enum class HdrToneMap {
	// The app, in its shader: HDR reference white (203 nits, BT.2408) is shown as bright as
	// the display's SDR white, and highlights above the display's headroom are rolled off.
	// The layer carries no HDR metadata, so macOS does nothing to the picture and can hand
	// a fullscreen one straight to the display, as it does with an SDR picture.
	App,
	// macOS, from HDR10 metadata on the layer (CAEDRMetadata). Its mapping follows the
	// headroom on its own, but every frame goes through the compositor, and in fullscreen
	// the picture stops for about 55 ms every 64 refreshes of the display (measured).
	System,
};

struct PipelineConfig {
	Codec codec = Codec::Auto;          // Auto: detected from the parameter sets
	PacingMode pacing = PacingMode::Immediate;
	Scaler scaler = Scaler::Bilinear;
	HdrOutput hdrOutput = HdrOutput::PQ;
	SdrColor sdrColor = SdrColor::Accurate;
	HdrToneMap hdrToneMap = HdrToneMap::App;
	// Brightness of an HDR (PQ) picture, a factor on its linear light. 1 is HdrToneMap::App's
	// reference (203 nits as bright as SDR white), or with HdrToneMap::System whatever macOS
	// makes of the picture as the console sends it (about 1.7 times brighter in the midtones).
	float hdrBrightness = 1.0f;
	// HdrToneMap::App: the largest value the display shows now, in units of SDR white
	// (NSScreen.maximumExtendedDynamicRangeColorComponentValue). 0: not known, nothing is
	// rolled off.
	float displayHeadroom = 0.0f;
	float displayRefreshHz = 120.0f;    // DisplayLink frame rate range (min=max=preferred)
	// DisplayLink only: if no new frame is waiting when the display link fires,
	// wait for one until (targetTimestamp - latchMarginUs) before giving up on
	// this refresh. 0 disables the wait (render only what is already decoded).
	// 1500 measured: about half the waits catch a frame, ~7 ms less latency.
	uint32_t latchMarginUs = 0;
	int maxDrawables = 3;               // CAMetalLayer.maximumDrawableCount (2 or 3)
	bool asyncDecode = true;            // kVTDecodeFrame_EnableAsynchronousDecompression
	// After a decode error or a rebuilt session, drop P-frames until the next
	// IDR instead of showing a corrupted picture.
	bool waitForIdrAfterError = true;
	// Request the decoder's lossless-compressed 8-bit output (&8v0) instead
	// of plain 420v. 10-bit streams always use x420 (&xv0 is packed and has
	// no Metal view format). Falls back to 420v if Metal cannot wrap it.
	bool compressedDecoderOutput = false;
	// > 0: render into a private offscreen texture of this size instead of the
	// layer's drawables and never present (GPU-time benchmarks at panel size
	// without covering the screen). Pacing is forced to Immediate.
	int offscreenWidth = 0;
	int offscreenHeight = 0;
	uint32_t statsWindow = 36000;       // samples kept per distribution (rolling)
	FrameCallback onFrame;              // optional per-frame timeline (see FrameTiming)
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
	std::string layerColorSpace;  // what the layer was last tagged with (a CGColorSpace name)
	std::string hdrMetadata;  // what CAEDRMetadata was built from (SEI values or defaults)
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

	// These take effect with the next frame; none of them touches the decoder.
	void setPacingMode(PacingMode mode);
	void setScaler(Scaler scaler);
	void setSdrColor(SdrColor color);
	void setHdrOutput(HdrOutput output);
	void setHdrBrightness(float gain); // see PipelineConfig::hdrBrightness; 0.05 to 4
	void setHdrToneMap(HdrToneMap toneMap);
	/** Any thread. See PipelineConfig::displayHeadroom. */
	void setDisplayHeadroom(float headroom);
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
const char *toString(SdrColor color);
const char *toString(HdrOutput output);
const char *toString(HdrToneMap toneMap);
bool parsePacingMode(const std::string &s, PacingMode *out);
bool parseScaler(const std::string &s, Scaler *out);
bool parseSdrColor(const std::string &s, SdrColor *out);
bool parseHdrOutput(const std::string &s, HdrOutput *out);
bool parseHdrToneMap(const std::string &s, HdrToneMap *out);

/**
 * The headroom to give setDisplayHeadroom(), from NSScreen's current
 * (maximumExtendedDynamicRangeColorComponentValue) and potential
 * (maximumPotentialExtendedDynamicRangeColorComponentValue) values. The current value is 1
 * until the system has made room for an HDR picture, so on a display that can do more the
 * picture is never fitted to less than twice SDR white (or the potential, if that is
 * less): were it fitted to 1, nothing on screen would ask for the room.
 */
float usableDisplayHeadroom(float current, float potential);

}  // namespace plk::video
