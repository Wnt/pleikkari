// VideoPipeline implementation. See include/plk/VideoPipeline.h for the contract.
//
// Data flow and threads
//
//   network thread           VideoToolbox thread         render thread (CFRunLoop)
//   submitFrame()            output handler              Immediate: woken by a run loop
//    Annex-B -> 4-byte        -> stamp decodedUs            source, renders the slot
//    length prefixes         -> put CVPixelBuffer in     DisplayLink: CAMetalDisplayLink
//    VTDecompressionSession     the one-frame slot           callback renders the slot
//    DecodeFrame (async)        (older frame there =      -> CVMetalTextureCache wraps the
//                               superseded, counted)         planes (no copy), one render
//                                                            pass, presentDrawable, commit
//
// Latency-relevant choices (measured by the harness, see README):
//  - Decode flags: asynchronous decompression only. No
//    kVTDecodeFrame_EnableTemporalProcessing (so VideoToolbox emits each frame
//    in decode order as soon as it is done: the stream has no B-frames) and no
//    kVTDecodeFrame_1xRealTimePlayback (that flag allows lower-power, slower
//    decoding). kVTDecompressionPropertyKey_RealTime = true.
//  - One frame slot between decoder and display; nothing else queues frames.
//  - The command buffer presents the drawable as soon as it is scheduled
//    (presentDrawable), so GPU work and presentation are not separated by a
//    CPU wait.

#import "plk/VideoPipeline.h"

#import "plk/FrameEnhancer.h"

#import "AnnexB.h"
#import "RollingStats.h"
#import "ToneMap.h"

#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#import <QuartzCore/QuartzCore.h>
#import <VideoToolbox/VideoToolbox.h>
#include <simd/simd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <mach/mach_time.h>
#include <mutex>
#include <pthread.h>
#include <thread>
#include <time.h>
#include <vector>

#include "ShaderSource.inc"  // generated from Shaders.metal: kPlkShaderSource

@interface PLKDisplayLinkTarget : NSObject <CAMetalDisplayLinkDelegate>
@property(nonatomic, assign) plk::video::VideoPipeline::Impl *impl;
@end

namespace plk::video {

namespace {

// ---- clocks ---------------------------------------------------------------

const mach_timebase_info_data_t &timebase()
{
	static mach_timebase_info_data_t tb = [] {
		mach_timebase_info_data_t t;
		mach_timebase_info(&t);
		return t;
	}();
	return tb;
}

// mach_absolute_time in microseconds: the same base as CACurrentMediaTime(),
// MTLDrawable.presentedTime, GPUStartTime and CAMetalDisplayLink timestamps.
double nowUs()
{
	const auto &tb = timebase();
	return (double)mach_absolute_time() * tb.numer / tb.denom / 1000.0;
}

double monotonicUs()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

std::string fourcc(OSType t)
{
	char s[5] = {(char)(t >> 24), (char)(t >> 16), (char)(t >> 8), (char)t, 0};
	for(int i = 0; i < 4; i++)
		if(s[i] < 32 || s[i] > 126)
			s[i] = '?';
	return s;
}

// ---- colour ---------------------------------------------------------------

enum class Matrix { BT601, BT709, BT2020 };
enum class Transfer { BT709, SRGB, PQ, HLG };
enum class Primaries { BT709, BT2020, P3 };

struct ColorInfo {
	OSType pixelFormat = 0;
	int bits = 8;
	bool fullRange = false;
	Matrix matrix = Matrix::BT709;
	Transfer transfer = Transfer::BT709;
	Primaries primaries = Primaries::BT709;
	bool chromaLeft = true;  // chroma co-sited with the left luma column (MPEG-2/H.264/HEVC default)
	bool chromaTop = false;  // co-sited with the top row
	bool hdr() const { return transfer == Transfer::PQ || transfer == Transfer::HLG; }
	bool sameOutput(const ColorInfo &o) const
	{
		return transfer == o.transfer && primaries == o.primaries;
	}
	bool operator==(const ColorInfo &o) const
	{
		return pixelFormat == o.pixelFormat && bits == o.bits && fullRange == o.fullRange && matrix == o.matrix
			&& transfer == o.transfer && primaries == o.primaries && chromaLeft == o.chromaLeft
			&& chromaTop == o.chromaTop;
	}
	std::string describe() const
	{
		std::string s;
		s += primaries == Primaries::BT2020 ? "BT.2020" : primaries == Primaries::P3 ? "P3" : "BT.709";
		s += transfer == Transfer::PQ ? "/PQ" : transfer == Transfer::HLG ? "/HLG" : transfer == Transfer::SRGB ? "/sRGB" : "/BT.709";
		s += matrix == Matrix::BT2020 ? " matrix BT.2020-NCL" : matrix == Matrix::BT601 ? " matrix BT.601" : " matrix BT.709";
		s += fullRange ? " full" : " limited";
		s += " " + std::to_string(bits) + "-bit";
		s += chromaLeft ? (chromaTop ? " chroma top-left" : " chroma left") : " chroma center";
		return s;
	}
};

bool cfEq(CFTypeRef a, CFStringRef b) { return a && CFGetTypeID(a) == CFStringGetTypeID() && CFEqual(a, b); }

bool isTenBit(OSType f)
{
	return f == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange || f == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange
		|| f == kCVPixelFormatType_Lossless_420YpCbCr10PackedBiPlanarVideoRange
		|| f == kCVPixelFormatType_Lossless_420YpCbCr10PackedBiPlanarFullRange;
}
bool isFullRange(OSType f)
{
	return f == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange || f == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange
		|| f == kCVPixelFormatType_Lossless_420YpCbCr8BiPlanarFullRange
		|| f == kCVPixelFormatType_Lossless_420YpCbCr10PackedBiPlanarFullRange;
}

// Colour description of a decoded buffer, from the attachments VideoToolbox
// copies out of the VUI. Missing tags default by bit depth: 10-bit is treated
// as the PS5's HDR10 (BT.2020 + PQ), 8-bit as BT.709.
ColorInfo colorInfoFor(CVPixelBufferRef buf)
{
	ColorInfo c;
	c.pixelFormat = CVPixelBufferGetPixelFormatType(buf);
	c.bits = isTenBit(c.pixelFormat) ? 10 : 8;
	c.fullRange = isFullRange(c.pixelFormat);
	bool ten = c.bits > 8;
	c.matrix = ten ? Matrix::BT2020 : Matrix::BT709;
	c.transfer = ten ? Transfer::PQ : Transfer::BT709;
	c.primaries = ten ? Primaries::BT2020 : Primaries::BT709;

	CFTypeRef m = CVBufferCopyAttachment(buf, kCVImageBufferYCbCrMatrixKey, nullptr);
	if(cfEq(m, kCVImageBufferYCbCrMatrix_ITU_R_709_2)) c.matrix = Matrix::BT709;
	else if(cfEq(m, kCVImageBufferYCbCrMatrix_ITU_R_601_4) || cfEq(m, kCVImageBufferYCbCrMatrix_SMPTE_240M_1995)) c.matrix = Matrix::BT601;
	else if(cfEq(m, kCVImageBufferYCbCrMatrix_ITU_R_2020)) c.matrix = Matrix::BT2020;
	if(m) CFRelease(m);

	CFTypeRef t = CVBufferCopyAttachment(buf, kCVImageBufferTransferFunctionKey, nullptr);
	if(cfEq(t, kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ)) c.transfer = Transfer::PQ;
	else if(cfEq(t, kCVImageBufferTransferFunction_ITU_R_2100_HLG)) c.transfer = Transfer::HLG;
	else if(cfEq(t, kCVImageBufferTransferFunction_sRGB)) c.transfer = Transfer::SRGB;
	else if(cfEq(t, kCVImageBufferTransferFunction_ITU_R_709_2) || cfEq(t, kCVImageBufferTransferFunction_ITU_R_2020)
		|| cfEq(t, kCVImageBufferTransferFunction_SMPTE_240M_1995)) c.transfer = Transfer::BT709;
	if(t) CFRelease(t);

	CFTypeRef p = CVBufferCopyAttachment(buf, kCVImageBufferColorPrimariesKey, nullptr);
	if(cfEq(p, kCVImageBufferColorPrimaries_ITU_R_709_2) || cfEq(p, kCVImageBufferColorPrimaries_EBU_3213)
		|| cfEq(p, kCVImageBufferColorPrimaries_SMPTE_C)) c.primaries = Primaries::BT709;
	else if(cfEq(p, kCVImageBufferColorPrimaries_ITU_R_2020)) c.primaries = Primaries::BT2020;
	else if(cfEq(p, kCVImageBufferColorPrimaries_P3_D65) || cfEq(p, kCVImageBufferColorPrimaries_DCI_P3)) c.primaries = Primaries::P3;
	if(p) CFRelease(p);

	CFTypeRef cl = CVBufferCopyAttachment(buf, kCVImageBufferChromaLocationTopFieldKey, nullptr);
	if(cfEq(cl, kCVImageBufferChromaLocation_Center) || cfEq(cl, kCVImageBufferChromaLocation_Top)
		|| cfEq(cl, kCVImageBufferChromaLocation_Bottom)) c.chromaLeft = false;
	c.chromaTop = cfEq(cl, kCVImageBufferChromaLocation_TopLeft) || cfEq(cl, kCVImageBufferChromaLocation_Top);
	if(cl) CFRelease(cl);
	return c;
}

struct alignas(16) ShaderParams {
	simd_float4 r0, r1, r2;
	simd_float2 lumaSize, chromaSize, uvScale, uvOffset, chromaShift, linearize, toneMap, sdrMap, warp, unusedWarp;
};
static_assert(sizeof(ShaderParams) == 128, "must match Params in Shaders.metal");

// Where SDR white is in the SDR rendition of a PQ picture that an enhancer is given
// (sdr_rendition() in Shaders.metal): HDR reference white, ITU-R BT.2408.
const float kEnhancerSdrWhiteNits = 203.0f;

struct alignas(16) NrShaderParams {
	simd_float4 lumaWeights;
	simd_float2 keySize, motion;
	float strength;
	float split;
	float guided;
	float unused;
};
struct alignas(16) GridShaderParams {
	simd_float4 lumaWeights;
	float blend, unused0, unused1, unused2;
};
static_assert(sizeof(GridShaderParams) == 32, "must match GridParams in Shaders.metal");
// The grid's size: kGridCols, kGridRows and kGridBins in Shaders.metal.
constexpr NSUInteger kGridWidth = 32 * 8, kGridHeight = 18;
static_assert(sizeof(NrShaderParams) == 48, "must match NrParams in Shaders.metal");

struct alignas(16) OverlayShaderParams {
	simd_float2 mode, unused;
};
static_assert(sizeof(OverlayShaderParams) == 16, "must match OverlayParams in Shaders.metal");

// The overlay's white on an HDR target: what the display shows as SDR white, which is 100
// nits in a PQ layer and 1.0 in an extended linear one (see renderFrame()).
constexpr float kOverlayWhiteNits = 100.0f;

// setContentInsets(): what the insets must leave of the target, either way, to be used.
constexpr long kMinInsetContent = 64;

void lumaCoefficients(Matrix m, double &kr, double &kb)
{
	switch(m)
	{
		case Matrix::BT601: kr = 0.299; kb = 0.114; break;
		case Matrix::BT2020: kr = 0.2627; kb = 0.0593; break;
		default: kr = 0.2126; kb = 0.0722; break;
	}
}

// Folds bit-depth container, range and YCbCr->R'G'B' matrix into three rows
// applied to raw normalized texture values.
void colorRows(const ColorInfo &c, ShaderParams &p)
{
	double kr, kb;
	lumaCoefficients(c.matrix, kr, kb);
	double kg = 1.0 - kr - kb;
	// A * (Y', Cb', Cr')
	double A[3][3] = {
		{1.0, 0.0, 2.0 * (1.0 - kr)},
		{1.0, -2.0 * kb * (1.0 - kb) / kg, -2.0 * kr * (1.0 - kr) / kg},
		{1.0, 2.0 * (1.0 - kb), 0.0},
	};
	// Normalized texture value -> integer code value. 10-bit lives in the MSBs
	// of a 16-bit container: n = code * 64 / 65535.
	double container = c.bits > 8 ? 65535.0 / 64.0 : 255.0;
	double k = (double)(1 << (c.bits - 8));
	double maxCode = (double)((1 << c.bits) - 1);
	double yOff, yRange, cOff, cRange;
	if(c.fullRange)
	{
		yOff = 0; yRange = maxCode; cOff = 128.0 * k; cRange = maxCode;
	}
	else
	{
		yOff = 16.0 * k; yRange = 219.0 * k; cOff = 128.0 * k; cRange = 224.0 * k;
	}
	double s[3] = {container / yRange, container / cRange, container / cRange};
	double o[3] = {yOff / yRange, cOff / cRange, cOff / cRange};
	simd_float4 *rows[3] = {&p.r0, &p.r1, &p.r2};
	for(int i = 0; i < 3; i++)
	{
		double off = 0;
		for(int j = 0; j < 3; j++)
			off += A[i][j] * o[j];
		*rows[i] = simd_make_float4((float)(A[i][0] * s[0]), (float)(A[i][1] * s[1]), (float)(A[i][2] * s[2]), (float)-off);
	}
}

bool linearOutput(const ColorInfo &c, HdrOutput mode) { return mode == HdrOutput::ExtendedLinear && c.transfer == Transfer::PQ; }
bool brightnessScaled(float gain) { return gain < 0.999f || gain > 1.001f; }

CFStringRef colorSpaceName(const ColorInfo &c, HdrOutput mode, SdrColor sdr)
{
	CFStringRef name;
	if(linearOutput(c, mode)) name = c.primaries == Primaries::P3 ? kCGColorSpaceExtendedLinearDisplayP3 : kCGColorSpaceExtendedLinearITUR_2020;
	else if(c.transfer == Transfer::PQ) name = kCGColorSpaceITUR_2100_PQ;
	else if(c.transfer == Transfer::HLG) name = kCGColorSpaceITUR_2100_HLG;
	else if(c.primaries == Primaries::BT2020) name = kCGColorSpaceITUR_2020;
	else if(c.primaries == Primaries::P3) name = kCGColorSpaceDisplayP3;
	// An SDR BT.709 picture is shown with its code values taken as sRGB, which is what the
	// console's picture looks like on a computer display and what the Qt client's OpenGL path
	// shows. Tagged kCGColorSpaceITUR_709, macOS decodes it with a pure 2.4 power curve where
	// sRGB has a linear toe: a code value of 10 % comes out 2.5 times darker, 5 % five times,
	// 2 % nineteen times (measured with ColorSync on macOS 27), and shadow detail disappears.
	// Vivid: the same transfer curve with the P3 primaries (see SdrColor).
	else name = sdr == SdrColor::Vivid ? kCGColorSpaceDisplayP3 : kCGColorSpaceSRGB;
	return name;
}

MTLPixelFormat outputFormatFor(const ColorInfo &c, HdrOutput mode)
{
	// HDR: 10 bits per channel, PQ/HLG code values; the compositor tone maps
	// to the panel's current EDR headroom using CAEDRMetadata. SDR: 8 bits.
	if(linearOutput(c, mode))
		return MTLPixelFormatRGBA16Float;
	return c.hdr() ? MTLPixelFormatBGR10A2Unorm : MTLPixelFormatBGRA8Unorm;
}

struct HdrMeta {
	annexb::HdrSei sei;
};

// MDCV max_display_mastering_luminance, in 0.0001 cd/m2 units (D.2.28); a PS5 sends none.
float masteringPeakNits(const std::shared_ptr<const HdrMeta> &hdr)
{
	if(hdr && hdr->sei.hasMasteringDisplay)
	{
		const auto &md = hdr->sei.masteringDisplay;
		const uint32_t raw = (uint32_t)md[16] << 24 | (uint32_t)md[17] << 16 | (uint32_t)md[18] << 8 | (uint32_t)md[19];
		if(raw)
			return raw / 10000.0f;
	}
	return kDefaultMasteringPeakNits;
}

// ---- per-frame record -----------------------------------------------------

struct FrameRecord {
	uint64_t seq = 0;
	uint16_t frameIndex = 0;
	bool keyframe = false;
	double readyUs = 0, submitUs = 0, decodedUs = 0, renderStartUs = 0, committedUs = 0;
	double gpuStartUs = 0, gpuEndUs = 0, presentedUs = 0, targetPresentUs = 0;
	double enhanceAgeMs = -1;
	// When the picture this frame shows arrived: of a repeat, when its picture did; of a
	// generated picture, when a picture at its place in time would have. 0: decodedUs.
	double pictureUs = 0;
	bool enhanceOffered = false;  // the enhancer was given this frame before it was rendered
	bool repeat = false;          // the same picture as the frame before it (EnhanceConfig::skipRepeats)
	bool presentExpected = false;
	std::atomic<int> pending{0};
	std::shared_ptr<const HdrMeta> hdr;
};

struct DecodedFrame {
	CVPixelBufferRef buf = nullptr;  // retained
	std::shared_ptr<FrameRecord> rec;
};

// A rendered frame kept for PipelineConfig::captureDir.
struct CapturedFrame {
	id<MTLBuffer> bytes;
	NSUInteger width = 0, height = 0, bytesPerPixel = 0;
	MTLPixelFormat format = MTLPixelFormatInvalid;
	uint64_t seq = 0, keySeq = 0;
	uint16_t frameIndex = 0;
	double decodedUs = 0, enhanceAgeMs = -1;
};

// ---- enhancer -------------------------------------------------------------

// A frame the enhancer was given: the R'G'B' copy of one decoded frame, BGRA8 of an SDR
// picture, RGBA half float (the SDR rendition) of a PQ picture.
struct KeyFrame {
	CVPixelBufferRef buf = nullptr;   // retained
	CVMetalTextureRef tex = nullptr;  // retained: `buf` as the render target it was drawn into
	uint64_t seq = 0;
	double decodedUs = 0;             // when the frame it was drawn from was decoded
	~KeyFrame()
	{
		if(tex) CFRelease(tex);
		if(buf) CVPixelBufferRelease(buf);
	}
};

// A result: a key frame and what the enhancer made of it.
struct EnhancedFrame {
	std::shared_ptr<KeyFrame> key;
	CVPixelBufferRef out = nullptr;      // retained
	CVMetalTextureRef outTex = nullptr;  // retained; made by the render thread on first use
	~EnhancedFrame()
	{
		if(outTex) CFRelease(outTex);
		if(out) CVPixelBufferRelease(out);
	}
};

// What the enhancer's callback writes. Its callbacks hold it, so it can outlive the Impl.
struct EnhanceState {
	std::mutex m;
	std::shared_ptr<EnhancedFrame> latest;  // m
	uint64_t generation = 0;                // m: a result of an older generation is dropped
	std::atomic<bool> busy{false};          // a frame is being drawn for, or is with, the enhancer
	std::atomic<bool> needReset{true};      // the next frame offered does not continue the last one
	std::atomic<double> holdUntilUs{0.0};   // nothing is offered before this (after a refusal)
	std::atomic<double> roundTripMs{0.0};   // what a result takes, smoothed; 0 before the first
	// m: wakes the render thread when the enhancer has become idle (a result, a refusal) or
	// a held frame's wait is over. Cleared before the Impl it points into goes away.
	std::function<void()> wake;
	void signal()
	{
		std::lock_guard<std::mutex> lk(m);
		if(wake)
			wake();
	}
};

// Statistics shared with Metal/VT handlers that may outlive the Impl.
struct Shared {
	explicit Shared(size_t window)
		: submitCall(window), decode(window), queue(window), renderCpu(window), renderGpu(window), submitToPresented(window),
		  readyToPresented(window), decodedToPresented(window), submitInterval(window), presentInterval(window),
		  linkLead(window), linkDeadline(window), presentVsTarget(window), enhance(window), enhanceInputGpu(window),
		  enhanceAge(window)
	{
	}
	mutable std::mutex m;
	Stats c;  // counters and stream info only
	// Per-frame callback; cleared under cbMutex by ~VideoPipeline so that it is never
	// called after the destructor returns (presented handlers can outlive the Impl).
	std::mutex cbMutex;
	FrameCallback onFrame;
	EnhanceCallback onEnhance;
	detail::RollingWindow submitCall, decode, queue, renderCpu, renderGpu, submitToPresented, readyToPresented,
		decodedToPresented, submitInterval, presentInterval, linkLead, linkDeadline, presentVsTarget, enhance,
		enhanceInputGpu, enhanceAge;
	double lastSubmitUs = 0, lastPresentedUs = 0;

	void report(const FrameRecord &r)
	{
		std::lock_guard<std::mutex> cl(cbMutex);
		if(!onFrame)
			return;
		FrameTiming t;
		t.frameIndex = r.frameIndex;
		t.keyframe = r.keyframe;
		t.offscreen = !r.presentExpected;
		t.presented = r.presentExpected && r.presentedUs > 0;
		t.readyUs = r.readyUs;
		t.submitUs = r.submitUs;
		t.decodedUs = r.decodedUs;
		t.renderStartUs = r.renderStartUs;
		t.committedUs = r.committedUs;
		t.gpuStartUs = r.gpuStartUs;
		t.gpuEndUs = r.gpuEndUs;
		t.presentedUs = t.presented ? r.presentedUs : 0;
		t.enhanceAgeMs = r.enhanceAgeMs;
		onFrame(t);
	}

	void report(const EnhanceTiming &t)
	{
		std::lock_guard<std::mutex> cl(cbMutex);
		if(onEnhance)
			onEnhance(t);
	}

	// From a frame's rendering to its being on the glass, lately: what the camera warp aims
	// at when a frame has no set time to be shown at (in a window that is 13 to 18 ms, not
	// the one refresh that was assumed).
	std::atomic<double> renderToGlassUs{8000.0};

	void finish(const FrameRecord &r)
	{
		report(r);
		std::lock_guard<std::mutex> lk(m);
		if(r.gpuEndUs > r.gpuStartUs && r.gpuStartUs > 0)
			renderGpu.add((r.gpuEndUs - r.gpuStartUs) / 1000.0);
		if(!r.presentExpected)
			return;
		if(r.presentedUs <= 0)
		{
			c.framesNotPresented++;
			return;
		}
		c.framesPresented++;
		submitToPresented.add((r.presentedUs - r.submitUs) / 1000.0);
		decodedToPresented.add((r.presentedUs - r.decodedUs) / 1000.0);
		if(r.readyUs > 0)
			readyToPresented.add((r.presentedUs - r.readyUs) / 1000.0);
		if(r.targetPresentUs > 0)
			presentVsTarget.add((r.presentedUs - r.targetPresentUs) / 1000.0);
		if(lastPresentedUs > 0 && r.presentedUs > lastPresentedUs)
			presentInterval.add((r.presentedUs - lastPresentedUs) / 1000.0);
		if(r.presentedUs > lastPresentedUs)
			lastPresentedUs = r.presentedUs;
	}
	void reset()
	{
		std::lock_guard<std::mutex> lk(m);
		Stats fresh;
		// keep stream info
		fresh.width = c.width; fresh.height = c.height; fresh.bitDepth = c.bitDepth; fresh.hdr = c.hdr;
		fresh.hardwareDecoder = c.hardwareDecoder; fresh.codec = c.codec; fresh.pixelFormat = c.pixelFormat;
		fresh.colorDescription = c.colorDescription; fresh.hdrMetadata = c.hdrMetadata; fresh.scaler = c.scaler; fresh.pacing = c.pacing;
		fresh.layerColorSpace = c.layerColorSpace;
		fresh.targetWidth = c.targetWidth; fresh.targetHeight = c.targetHeight;
		fresh.enhancer = c.enhancer; fresh.enhanceWidth = c.enhanceWidth; fresh.enhanceHeight = c.enhanceHeight;
		c = fresh;
		for(auto *w : {&submitCall, &decode, &queue, &renderCpu, &renderGpu, &submitToPresented, &readyToPresented,
				&decodedToPresented, &submitInterval, &presentInterval, &linkLead, &linkDeadline, &presentVsTarget,
				&enhance, &enhanceInputGpu, &enhanceAge})
			w->clear();
		lastSubmitUs = 0;
		lastPresentedUs = 0;
	}
};

constexpr double kIdrRequestMinIntervalUs = 100000.0;

}  // namespace

double hostTimeUs() { return nowUs(); }

double monotonicToHostUs(uint64_t t)
{
	if(!t)
		return 0;
	return (double)t - (monotonicUs() - nowUs());
}

const char *toString(PacingMode m)
{
	switch(m)
	{
		case PacingMode::Immediate: return "immediate";
		case PacingMode::ImmediateVsync: return "immediate-vsync";
		case PacingMode::DisplayLink: return "displaylink";
	}
	return "?";
}
const char *toString(Scaler s)
{
	switch(s)
	{
		case Scaler::Bilinear: return "bilinear";
		case Scaler::Bicubic: return "bicubic";
		case Scaler::Lanczos: return "lanczos";
		case Scaler::MetalFX: return "metalfx";
	}
	return "?";
}
const char *toString(Codec c) { return c == Codec::H264 ? "h264" : c == Codec::HEVC ? "hevc" : "auto"; }
const char *toString(SdrColor c) { return c == SdrColor::Vivid ? "vivid" : "accurate"; }
const char *toString(HdrOutput o) { return o == HdrOutput::ExtendedLinear ? "linear" : "pq"; }
const char *toString(HdrToneMap t) { return t == HdrToneMap::System ? "system" : "app"; }
bool parsePacingMode(const std::string &s, PacingMode *out)
{
	if(s == "immediate" || s == "arrival") { *out = PacingMode::Immediate; return true; }
	if(s == "immediate-vsync") { *out = PacingMode::ImmediateVsync; return true; }
	if(s == "displaylink" || s == "vsync") { *out = PacingMode::DisplayLink; return true; }
	return false;
}
bool parseSdrColor(const std::string &s, SdrColor *out)
{
	if(s == "accurate" || s == "srgb") { *out = SdrColor::Accurate; return true; }
	if(s == "vivid" || s == "p3") { *out = SdrColor::Vivid; return true; }
	return false;
}
bool parseHdrToneMap(const std::string &s, HdrToneMap *out)
{
	if(s == "app") { *out = HdrToneMap::App; return true; }
	if(s == "system" || s == "macos") { *out = HdrToneMap::System; return true; }
	return false;
}
float usableDisplayHeadroom(float current, float potential)
{
	if(!(potential > 1.0f))
		return 1.0f;
	const float floor = potential < 2.0f ? potential : 2.0f;
	return current > floor ? current : floor;
}
bool parseHdrOutput(const std::string &s, HdrOutput *out)
{
	if(s == "pq") { *out = HdrOutput::PQ; return true; }
	if(s == "linear") { *out = HdrOutput::ExtendedLinear; return true; }
	return false;
}
bool parseScaler(const std::string &s, Scaler *out)
{
	if(s == "bilinear") { *out = Scaler::Bilinear; return true; }
	if(s == "bicubic" || s == "catmullrom") { *out = Scaler::Bicubic; return true; }
	if(s == "lanczos") { *out = Scaler::Lanczos; return true; }
	if(s == "metalfx") { *out = Scaler::MetalFX; return true; }
	return false;
}

// ===========================================================================

struct VideoPipeline::Impl {
	PipelineConfig cfg;
	ErrorCallback onError;
	std::shared_ptr<Shared> shared;
	std::atomic<bool> stopping{false};

	// ---- decoder state (decMutex) ----
	std::mutex decMutex;
	bool codecKnown = false;
	annexb::Codec codec = annexb::Codec::HEVC;
	annexb::ParameterSets params;
	annexb::HdrSei hdrSei;
	std::shared_ptr<const HdrMeta> hdrMeta;
	CMVideoFormatDescriptionRef format = nullptr;
	VTDecompressionSessionRef session = nullptr;
	CMMemoryPoolRef memPool = nullptr;
	std::vector<annexb::Nal> nals;
	uint64_t submitSeq = 0;
	bool useCompressedOutput = false;
	std::atomic<bool> awaitingIdr{true};
	std::atomic<bool> sessionInvalid{false};
	std::atomic<bool> compressedUnsupported{false};
	std::atomic<double> lastIdrRequestUs{0.0};

	// ---- frame slot ----
	std::mutex slotMutex;
	std::condition_variable slotCv;
	DecodedFrame slot;

	// ---- render thread ----
	std::thread renderThread;
	CFRunLoopRef runLoop = nullptr;
	CFRunLoopSourceRef wakeSource = nullptr;
	std::atomic<bool> renderStop{false};
	std::mutex cmdMutex;
	std::vector<std::function<void()>> cmds;
	std::atomic<int> pacing{0};  // PacingMode, read by the decoder thread
	std::atomic<uint32_t> latchMarginUs{0};
	std::atomic<float> displayHeadroom{0.0f};

	// Render-thread-only state.
	id<MTLDevice> device;
	id<MTLCommandQueue> queue;
	id<MTLLibrary> library;
	CVMetalTextureCacheRef texCache = nullptr;
	CAMetalLayer *layer;
	CAMetalDisplayLink *link;
	PLKDisplayLinkTarget *linkTarget;
	std::map<std::pair<int, MTLPixelFormat>, id<MTLRenderPipelineState>> pipelines;
	bool layerConfigured = false;
	ColorInfo layerColor;
	std::shared_ptr<const HdrMeta> layerHdr;
	float layerBrightness = 1.0f; // the hdrBrightness the layer's HDR metadata was made for
	PacingMode activePacing = PacingMode::DisplayLink;
	Scaler scaler = Scaler::Bilinear;
	id<MTLFXSpatialScaler> fx;
	id<MTLTexture> fxIn, fxOut;
	id<MTLTexture> offscreen;
	std::vector<CapturedFrame> captured;  // render thread, then the destructor
	uint64_t captureSeen = 0;
	id<MTLTexture> lastTarget;
	id<MTLCommandBuffer> lastCommandBuffer;
	uint64_t framesSinceFlush = 0;

	// The enhancer: render-thread-only too, except `enhance`, which its callbacks share.
	std::shared_ptr<EnhanceState> enhance = std::make_shared<EnhanceState>();
	std::shared_ptr<FrameEnhancer> enhancer;
	EnhanceConfig enhanceCfg;
	CVPixelBufferPoolRef keyPool = nullptr;
	size_t keyPoolW = 0, keyPoolH = 0;
	OSType keyPoolFormat = 0;
	uint64_t keySeq = 0;
	double lastOfferUs = 0;
	id<MTLCommandBuffer> lastKeyCommandBuffer;
	// EnhanceConfig::wait: frames that wait for the enhancer's result of themselves, oldest
	// first. At most two: the one the enhancer is working on and the one that is next.
	struct Held {
		DecodedFrame f;
		uint64_t keySeq = 0;   // the offer's sequence number; 0 with `tried`: could not be offered
		bool tried = false;
		double deadlineUs = 0;
	};
	std::deque<Held> held;
	// Without wait: the newest frame that found the enhancer busy. It is offered the moment
	// the enhancer is idle, so the enhancer works back to back whatever the frames' cadence.
	DecodedFrame nrPending;
	double lastArrivalUs = 0, arrivalIntervalUs = 16667.0;
	// Frame generation (setInterpolator, setFrameGeneration): render-thread-only.
	// A picture as frame generation wants it: R'G'B' code values in one texture.
	struct RgbPicture {
		CVPixelBufferRef buf = nullptr;
		CVMetalTextureRef tex = nullptr;
		~RgbPicture()
		{
			if(tex) CFRelease(tex);
			if(buf) CVPixelBufferRelease(buf);
		}
	};
	std::shared_ptr<FrameInterpolator> interpolator;
	int generating = 0;                      // pictures made between two of the stream's; 0: off
	uint64_t generation = 0;                 // a picture generated for an older one is not shown
	std::shared_ptr<RgbPicture> lastPicture; // the newest picture of the stream
	double lastPictureUs = 0;
	CVPixelBufferPoolRef rgbPool = nullptr;
	size_t rgbPoolW = 0, rgbPoolH = 0;
	OSType rgbPoolFormat = 0;
	uint64_t rgbSeq = 0;
	id<MTLCommandBuffer> lastRgbCommandBuffer;
	// The picture that waits for its turn after the generated one before it.
	struct DuePicture {
		std::shared_ptr<RgbPicture> picture;
		DecodedFrame f;
		double dueUs = 0;
		bool generated = false;
	};
	std::deque<DuePicture> due;
	double lastDueUs = 0;
	// A picture of the stream on its way to the screen: with the generated picture that
	// comes before it, once that is made or given up.
	struct Stage {
		std::shared_ptr<RgbPicture> picture;
		std::vector<std::shared_ptr<RgbPicture>> made;  // what comes before it, earliest first
		DecodedFrame f;
		bool done = false, shown = false;
	};
	std::deque<std::shared_ptr<Stage>> order;
	bool generationBusy = false;
	double lastNewPictureUs = 0, pictureIntervalUs = 33333.0;
	// Pictures that wait for the enhancer's result of themselves before all that (EnhanceConfig::wait).
	std::deque<Held> awaiting;

	// Camera warp (setWarp): render-thread-only.
	std::function<void(const PictureMotion &)> motionSink;
	std::function<void(double, double, float *, float *)> warpSource;
	double lastPictureArrivalUs = 0;   // when the newest picture of the game arrived

	// EnhanceConfig::guided: the enhancer's change as a table of place and brightness, made
	// of each result once. Two textures: the table in use and the one the next is drawn into.
	id<MTLTexture> gridTex[2];
	int gridIndex = 0;
	uint64_t gridSeq = 0;      // the result the table in use was made of
	uint64_t gridGeneration = 0;
	bool gridValid = false;

	// EnhanceConfig::skipRepeats: the luma of the last frame at a few hundred points, and
	// the largest difference between two frames lately, which says what a new picture is.
	std::vector<uint16_t> lastSignature;
	std::deque<double> recentChanges;
	bool lastWasRepeat = false;
	std::atomic<bool> mixStatesBuilt{false};  // any thread: setEnhancer() builds them once

	enum Frag { FragBilinear = 0, FragBicubic, FragLanczos, FragCopy, FragBilinearNr, FragBicubicNr, FragLanczosNr,
		FragOverlay, FragRgb, FragGridBuild };
	// What a pass needs on top of the plane textures to mix an enhancer's result in.
	struct NrDraw {
		id<MTLTexture> key, out, grid;
		NrShaderParams params;
	};
	// A second draw in the pass that fills the target: the overlay, blended over the frame.
	struct OverlayDraw {
		id<MTLRenderPipelineState> pso;
		id<MTLTexture> tex;
		MTLRegion at;
		OverlayShaderParams params;
	};

	// setContentInsets() and setOverlay(): render-thread-only, set through post().
	int insetLeft = 0, insetTop = 0, insetRight = 0, insetBottom = 0;
	id<MTLTexture> overlayTex;
	int overlayX = 0, overlayY = 0;
	std::atomic<bool> overlayStatesBuilt{false};  // any thread: setOverlay() builds them once

	// ------------------------------------------------------------------------
	bool offscreenMode() const { return cfg.offscreenWidth > 0 && cfg.offscreenHeight > 0; }

	void deliver(const Error &e)
	{
		if(stopping.load())
			return;
		if(e.requestIdr)
		{
			std::lock_guard<std::mutex> lk(shared->m);
			shared->c.idrRequests++;
		}
		if(onError)
			onError(e);
	}

	bool claimIdrRequest()
	{
		double now = nowUs();
		double last = lastIdrRequestUs.load();
		if(now - last < kIdrRequestMinIntervalUs)
			return false;
		return lastIdrRequestUs.compare_exchange_strong(last, now);
	}

	// ======================= decoder =======================================

	void destroySession()
	{
		if(session)
		{
			VTDecompressionSessionWaitForAsynchronousFrames(session);
			VTDecompressionSessionInvalidate(session);
			CFRelease(session);
			session = nullptr;
		}
		if(format)
		{
			CFRelease(format);
			format = nullptr;
		}
	}

	bool createSession(std::vector<Error> &errors)
	{
		destroySession();
		OSStatus st;
		std::vector<const uint8_t *> ptrs;
		std::vector<size_t> sizes;
		auto push = [&](const std::vector<std::vector<uint8_t>> &l) {
			for(const auto &v : l) { ptrs.push_back(v.data()); sizes.push_back(v.size()); }
		};
		if(codec == annexb::Codec::HEVC)
		{
			push(params.vps); push(params.sps); push(params.pps);
			st = CMVideoFormatDescriptionCreateFromHEVCParameterSets(kCFAllocatorDefault, ptrs.size(), ptrs.data(),
					sizes.data(), 4, nullptr, &format);
		}
		else
		{
			push(params.sps); push(params.pps);
			st = CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault, ptrs.size(), ptrs.data(),
					sizes.data(), 4, &format);
		}
		if(st != noErr || !format)
		{
			errors.push_back({ErrorKind::BadConfig, st, false, "format description from parameter sets failed"});
			format = nullptr;
			return false;
		}

		CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(format);
		int bits = 8;
		if(CFTypeRef b = CMFormatDescriptionGetExtension(format, kCMFormatDescriptionExtension_BitsPerComponent))
		{
			if(CFGetTypeID(b) == CFNumberGetTypeID())
				CFNumberGetValue((CFNumberRef)b, kCFNumberIntType, &bits);
		}
		else
		{
			int prof = params.profileIdc();
			if(codec == annexb::Codec::HEVC ? prof == 2 : (prof == 110 || prof == 122 || prof == 244))
				bits = 10;
		}
		bool full = CMFormatDescriptionGetExtension(format, kCMFormatDescriptionExtension_FullRangeVideo) == kCFBooleanTrue;

		// Lossless-compressed output only for 8-bit: &8v0 planes read correctly
		// through R8/RG8 views (verified by read-back), but &xv0 is packed
		// 10-bit without padding and has no public Metal pixel format.
		bool tryCompressed = useCompressedOutput && !compressedUnsupported.load() && bits == 8;
		for(int attempt = 0; attempt < 2; attempt++)
		{
			bool compressed = tryCompressed && attempt == 0;
			OSType pf;
			if(bits > 8 && compressed)
				pf = full ? (OSType)kCVPixelFormatType_Lossless_420YpCbCr10PackedBiPlanarFullRange
				          : (OSType)kCVPixelFormatType_Lossless_420YpCbCr10PackedBiPlanarVideoRange;
			else if(bits > 8)
				pf = full ? (OSType)kCVPixelFormatType_420YpCbCr10BiPlanarFullRange
				          : (OSType)kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange;
			else if(compressed)
				pf = full ? (OSType)kCVPixelFormatType_Lossless_420YpCbCr8BiPlanarFullRange
				          : (OSType)kCVPixelFormatType_Lossless_420YpCbCr8BiPlanarVideoRange;
			else
				pf = full ? (OSType)kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
				          : (OSType)kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
			NSDictionary *spec = @{(__bridge NSString *)kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder: @YES};
			NSDictionary *attrs = @{
				(__bridge NSString *)kCVPixelBufferPixelFormatTypeKey: @(pf),
				(__bridge NSString *)kCVPixelBufferMetalCompatibilityKey: @YES,
				(__bridge NSString *)kCVPixelBufferIOSurfacePropertiesKey: @{},
			};
			st = VTDecompressionSessionCreate(kCFAllocatorDefault, format, (__bridge CFDictionaryRef)spec,
					(__bridge CFDictionaryRef)attrs, nullptr, &session);
			if(st == noErr && session)
			{
				VTSessionSetProperty(session, kVTDecompressionPropertyKey_RealTime, kCFBooleanTrue);
				CFBooleanRef hw = nullptr;
				VTSessionCopyProperty(session, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder,
						kCFAllocatorDefault, &hw);
				std::lock_guard<std::mutex> lk(shared->m);
				shared->c.sessionsCreated++;
				shared->c.width = dim.width;
				shared->c.height = dim.height;
				shared->c.bitDepth = bits;
				shared->c.hardwareDecoder = hw == kCFBooleanTrue;
				shared->c.codec = codec == annexb::Codec::HEVC ? "hevc" : "h264";
				shared->c.pixelFormat = fourcc(pf);
				if(hw)
					CFRelease(hw);
				break;
			}
			session = nullptr;
			if(!compressed)
				break;
		}
		if(!session)
		{
			errors.push_back({ErrorKind::SessionCreateFailed, st, false,
					"VTDecompressionSessionCreate failed (hardware decoder required)"});
			return false;
		}
		// A fresh session can only start at a keyframe.
		awaitingIdr.store(true);
		return true;
	}

	void noteSei(const annexb::Nal &n)
	{
		annexb::HdrSei s = hdrSei;
		if(annexb::parseHdrSei(codec, n, s) && s != hdrSei)
		{
			hdrSei = s;
			auto m = std::make_shared<HdrMeta>();
			m->sei = s;
			hdrMeta = m;
		}
	}

	// Called with decMutex held.
	void applyParams(const annexb::ParameterSets &ps, std::vector<Error> &errors)
	{
		if(session && ps == params)
			return;
		bool change = !params.sps.empty() && ps != params;
		params = ps;
		if(change)
		{
			std::lock_guard<std::mutex> lk(shared->m);
			shared->c.configChanges++;
		}
		createSession(errors);
	}

	bool ensureCodec(const uint8_t *data, size_t size)
	{
		if(codecKnown)
			return true;
		if(cfg.codec != Codec::Auto)
		{
			codec = cfg.codec == Codec::H264 ? annexb::Codec::H264 : annexb::Codec::HEVC;
			codecKnown = true;
			return true;
		}
		annexb::Codec c;
		if(!annexb::detectCodec(data, size, &c))
			return false;
		codec = c;
		codecKnown = true;
		return true;
	}

	bool submitConfig(const uint8_t *data, size_t size)
	{
		std::vector<Error> errors;
		bool ok = false;
		{
			std::lock_guard<std::mutex> lk(decMutex);
			if(stopping.load())
				return false;
			{
				std::lock_guard<std::mutex> sl(shared->m);
				shared->c.configsSubmitted++;
			}
			if(!ensureCodec(data, size))
			{
				errors.push_back({ErrorKind::BadConfig, 0, false, "no recognisable parameter sets in config"});
			}
			else
			{
				nals.clear();
				annexb::splitNals(data, size, nals);
				annexb::ParameterSets ps;
				ps.codec = codec;
				for(const auto &n : nals)
				{
					int t = annexb::nalType(codec, n);
					if(annexb::isParameterSet(codec, t))
						ps.add(n, t);
					else if(annexb::isSei(codec, t))
						noteSei(n);
				}
				if(!ps.complete())
					errors.push_back({ErrorKind::BadConfig, 0, false, "config sample lacks VPS/SPS/PPS"});
				else
				{
					applyParams(ps, errors);
					ok = session != nullptr;
				}
			}
		}
		for(const auto &e : errors)
			deliver(e);
		return ok;
	}

	bool submitFrame(const uint8_t *data, size_t size, uint16_t frameIndex, uint64_t frameReadyUs, bool recovered,
			int32_t framesLost)
	{
		double t0 = nowUs();
		double readyAbs = 0;
		if(frameReadyUs)
			readyAbs = (double)frameReadyUs - (monotonicUs() - nowUs());
		std::vector<Error> errors;
		bool ok = false;
		{
			std::lock_guard<std::mutex> lk(decMutex);
			if(stopping.load())
				return false;
			{
				std::lock_guard<std::mutex> sl(shared->m);
				shared->c.framesSubmitted++;
				if(recovered) shared->c.framesRecovered++;
				if(framesLost > 0) shared->c.framesLostReported += (uint64_t)framesLost;
				if(shared->lastSubmitUs > 0)
					shared->submitInterval.add((t0 - shared->lastSubmitUs) / 1000.0);
				shared->lastSubmitUs = t0;
			}
			ok = decodeLocked(data, size, frameIndex, readyAbs, t0, errors);
		}
		{
			std::lock_guard<std::mutex> sl(shared->m);
			shared->submitCall.add((nowUs() - t0) / 1000.0);
		}
		for(const auto &e : errors)
			deliver(e);
		return ok;
	}

	void skipAwaitingIdr(std::vector<Error> &errors, const char *why)
	{
		{
			std::lock_guard<std::mutex> sl(shared->m);
			shared->c.framesSkippedAwaitingIdr++;
		}
		if(claimIdrRequest())
			errors.push_back({ErrorKind::DecodeFailed, 0, true, why});
	}

	bool decodeLocked(const uint8_t *data, size_t size, uint16_t frameIndex, double readyAbs, double t0,
			std::vector<Error> &errors)
	{
		if(!ensureCodec(data, size))
		{
			skipAwaitingIdr(errors, "frame before any parameter sets");
			return false;
		}
		nals.clear();
		annexb::splitNals(data, size, nals);
		annexb::ParameterSets inband;
		inband.codec = codec;
		bool hasPs = false, key = false;
		size_t payload = 0;
		for(const auto &n : nals)
		{
			int t = annexb::nalType(codec, n);
			if(annexb::isParameterSet(codec, t)) { inband.add(n, t); hasPs = true; continue; }
			if(annexb::isAud(codec, t)) continue;
			if(annexb::isSei(codec, t)) noteSei(n);
			if(annexb::isVcl(codec, t) && annexb::isKeyframe(codec, t)) key = true;
			payload += 4 + n.size;
		}
		if(hasPs && inband.complete())
			applyParams(inband, errors);
		if(compressedUnsupported.load() && useCompressedOutput)
		{
			useCompressedOutput = false;
			if(!params.sps.empty())
				createSession(errors);
		}
		if(sessionInvalid.exchange(false) && !params.sps.empty())
		{
			createSession(errors);
			std::lock_guard<std::mutex> sl(shared->m);
			shared->c.sessionRecoveries++;
		}
		if(!session)
		{
			skipAwaitingIdr(errors, "no decoder session (missing or bad parameter sets)");
			return false;
		}
		if(payload == 0)
			return true;  // parameter sets / SEI only
		if(awaitingIdr.load())
		{
			if(!key)
			{
				skipAwaitingIdr(errors, "waiting for IDR");
				return false;
			}
			awaitingIdr.store(false);
		}

		for(int attempt = 0; attempt < 2; attempt++)
		{
			CMBlockBufferRef bb = nullptr;
			OSStatus st = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, payload,
					CMMemoryPoolGetAllocator(memPool), nullptr, 0, payload, kCMBlockBufferAssureMemoryNowFlag, &bb);
			if(st != noErr)
			{
				errors.push_back({ErrorKind::DecodeFailed, st, false, "CMBlockBuffer allocation failed"});
				return false;
			}
			char *dst = nullptr;
			CMBlockBufferGetDataPointer(bb, 0, nullptr, nullptr, &dst);
			for(const auto &n : nals)
			{
				int t = annexb::nalType(codec, n);
				if(annexb::isParameterSet(codec, t) || annexb::isAud(codec, t))
					continue;
				uint32_t len = (uint32_t)n.size;
				dst[0] = (char)(len >> 24); dst[1] = (char)(len >> 16); dst[2] = (char)(len >> 8); dst[3] = (char)len;
				std::memcpy(dst + 4, n.data, n.size);
				dst += 4 + n.size;
			}
			CMSampleBufferRef sb = nullptr;
			size_t sampleSize = payload;
			st = CMSampleBufferCreateReady(kCFAllocatorDefault, bb, format, 1, 0, nullptr, 1, &sampleSize, &sb);
			CFRelease(bb);
			if(st != noErr)
			{
				errors.push_back({ErrorKind::DecodeFailed, st, false, "CMSampleBufferCreateReady failed"});
				return false;
			}

			auto rec = std::make_shared<FrameRecord>();
			rec->seq = ++submitSeq;
			rec->frameIndex = frameIndex;
			rec->keyframe = key;
			rec->readyUs = readyAbs;
			rec->submitUs = t0;
			rec->hdr = hdrMeta;
			VTDecodeFrameFlags flags = cfg.asyncDecode ? kVTDecodeFrame_EnableAsynchronousDecompression : 0;
			VTDecodeInfoFlags info = 0;
			st = VTDecompressionSessionDecodeFrameWithOutputHandler(session, sb, flags, &info,
					^(OSStatus status, VTDecodeInfoFlags infoFlags, CVImageBufferRef image, CMTime, CMTime) {
						onDecoded(rec, status, infoFlags, image);
					});
			CFRelease(sb);
			if(st == noErr)
				return true;

			if(st == kVTInvalidSessionErr || st == kVTVideoDecoderMalfunctionErr)
			{
				// Sleep/wake, GPU or media server reset: rebuild from the
				// current parameter sets. A keyframe can be retried right away.
				bool rebuilt = createSession(errors);
				{
					std::lock_guard<std::mutex> sl(shared->m);
					shared->c.sessionRecoveries++;
				}
				if(rebuilt && key && attempt == 0)
				{
					awaitingIdr.store(false);
					continue;
				}
				errors.push_back({ErrorKind::SessionInvalidated, st, claimIdrRequest(),
						"decoder session invalidated; rebuilt, waiting for IDR"});
				return false;
			}
			{
				std::lock_guard<std::mutex> sl(shared->m);
				shared->c.decodeErrors++;
			}
			if(cfg.waitForIdrAfterError)
				awaitingIdr.store(true);
			errors.push_back({ErrorKind::DecodeFailed, st, claimIdrRequest(), "VTDecompressionSessionDecodeFrame rejected the frame"});
			return false;
		}
		return false;
	}

	// VideoToolbox thread (or the submitting thread with synchronous decode).
	void onDecoded(const std::shared_ptr<FrameRecord> &rec, OSStatus status, VTDecodeInfoFlags info, CVImageBufferRef image)
	{
		double t = nowUs();
		if(status != noErr || !image)
		{
			if((info & kVTDecodeInfo_FrameDropped) && status == noErr)
				return;
			if(status == kVTInvalidSessionErr || status == kVTVideoDecoderMalfunctionErr)
				sessionInvalid.store(true);
			{
				std::lock_guard<std::mutex> sl(shared->m);
				shared->c.decodeErrors++;
			}
			if(cfg.waitForIdrAfterError)
				awaitingIdr.store(true);
			deliver({ErrorKind::DecodeFailed, status, claimIdrRequest(), "decoder reported an error for a frame"});
			return;
		}
		rec->decodedUs = t;
		{
			std::lock_guard<std::mutex> sl(shared->m);
			shared->c.framesDecoded++;
			shared->decode.add((t - rec->submitUs) / 1000.0);
		}
		DecodedFrame old;
		{
			std::lock_guard<std::mutex> lk(slotMutex);
			old = slot;
			slot.buf = CVPixelBufferRetain(image);
			slot.rec = rec;
		}
		slotCv.notify_all();
		if(old.buf)
		{
			CVPixelBufferRelease(old.buf);
			std::lock_guard<std::mutex> sl(shared->m);
			shared->c.framesSuperseded++;
		}
		if(pacing.load() != (int)PacingMode::DisplayLink)
			wakeRenderThread();
	}

	DecodedFrame takeSlot()
	{
		std::lock_guard<std::mutex> lk(slotMutex);
		DecodedFrame f = slot;
		slot = DecodedFrame();
		return f;
	}

	void putBackIfEmpty(DecodedFrame &f)
	{
		{
			std::lock_guard<std::mutex> lk(slotMutex);
			if(!slot.buf)
			{
				slot = f;
				f = DecodedFrame();
			}
		}
		if(f.buf)
		{
			CVPixelBufferRelease(f.buf);
			std::lock_guard<std::mutex> sl(shared->m);
			shared->c.framesSuperseded++;
		}
	}

	// ======================= render thread ==================================

	void wakeRenderThread()
	{
		if(wakeSource && runLoop)
		{
			CFRunLoopSourceSignal(wakeSource);
			CFRunLoopWakeUp(runLoop);
		}
	}

	void post(std::function<void()> fn)
	{
		{
			std::lock_guard<std::mutex> lk(cmdMutex);
			cmds.push_back(std::move(fn));
		}
		wakeRenderThread();
	}

	static void wakePerform(void *info) { static_cast<Impl *>(info)->onWake(); }

	void onWake()
	{
		std::vector<std::function<void()>> todo;
		{
			std::lock_guard<std::mutex> lk(cmdMutex);
			todo.swap(cmds);
		}
		for(auto &fn : todo)
			fn();
		if(activePacing != PacingMode::DisplayLink)
		{
			for(;;)
			{
				DecodedFrame f = takeSlot();
				if(!f.buf)
					break;
				frameArrived(f);
			}
		}
	}

	void renderThreadMain(std::promise<void> *ready)
	{
		pthread_setname_np("plk.video.render");
		pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
		@autoreleasepool
		{
			runLoop = (CFRunLoopRef)CFRetain(CFRunLoopGetCurrent());
			CFRunLoopSourceContext ctx = {};
			ctx.info = this;
			ctx.perform = &Impl::wakePerform;
			wakeSource = CFRunLoopSourceCreate(kCFAllocatorDefault, 0, &ctx);
			CFRunLoopAddSource(runLoop, wakeSource, kCFRunLoopDefaultMode);
			applyPacing(offscreenMode() ? PacingMode::Immediate : cfg.pacing);
		}
		ready->set_value();
		// Build every pipeline state before the first frame can arrive so
		// that no frame pays for shader specialization (~100+ ms each).
		@autoreleasepool
		{
			double t0 = nowUs();
			for(MTLPixelFormat fmt : {MTLPixelFormatBGRA8Unorm, MTLPixelFormatBGR10A2Unorm, MTLPixelFormatRGBA16Float})
				for(Frag f : {FragBilinear, FragBicubic, FragLanczos, FragCopy})
					pipelineFor(f, fmt);
			NSLog(@"plk-video: pipeline states built in %.1f ms", (nowUs() - t0) / 1000.0);
		}
		while(!renderStop.load())
		{
			@autoreleasepool
			{
				CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.25, false);
			}
		}
		@autoreleasepool
		{
			if(link)
			{
				linkTarget.impl = nullptr;
				[link invalidate];
				link = nil;
			}
			CFRunLoopRemoveSource(runLoop, wakeSource, kCFRunLoopDefaultMode);
		}
	}

	void applyPacing(PacingMode m)
	{
		activePacing = m;
		pacing.store((int)m);
		if(m == PacingMode::DisplayLink)
		{
			[CATransaction begin];
			[CATransaction setDisableActions:YES];
			layer.displaySyncEnabled = YES;
			[CATransaction commit];
			if(!link)
			{
				link = [[CAMetalDisplayLink alloc] initWithMetalLayer:layer];
				linkTarget = [PLKDisplayLinkTarget new];
				linkTarget.impl = this;
				link.delegate = linkTarget;
				// 1 is the minimum: lower values are rejected (read back as 2).
				// Measured on macOS 27 / 120 Hz windowed: targetPresentationTimestamp
				// is ~41.6 ms (5 refreshes) after the callback for both 1 and 2.
				link.preferredFrameLatency = 1.0f;
				float hz = cfg.displayRefreshHz > 0 ? cfg.displayRefreshHz : 120.0f;
				link.preferredFrameRateRange = CAFrameRateRangeMake(hz, hz, hz);
				[link addToRunLoop:[NSRunLoop currentRunLoop] forMode:NSDefaultRunLoopMode];
			}
		}
		else
		{
			if(link)
			{
				linkTarget.impl = nullptr;
				[link invalidate];
				link = nil;
				linkTarget = nil;
			}
			if(!offscreenMode())
			{
				[CATransaction begin];
				[CATransaction setDisableActions:YES];
				layer.displaySyncEnabled = m == PacingMode::ImmediateVsync;
				[CATransaction commit];
			}
		}
		std::lock_guard<std::mutex> sl(shared->m);
		shared->c.pacing = toString(m);
	}

	void onDisplayLink(CAMetalDisplayLinkUpdate *update)
	{
		double now = nowUs();
		double target = update.targetPresentationTimestamp * 1e6;
		double deadline = update.targetTimestamp * 1e6;
		{
			std::lock_guard<std::mutex> sl(shared->m);
			shared->c.displayLinkTicks++;
			shared->linkLead.add((target - now) / 1000.0);
			shared->linkDeadline.add((deadline - now) / 1000.0);
		}
		DecodedFrame f = takeSlot();
		uint32_t margin = latchMarginUs.load();
		if(!f.buf && margin > 0)
		{
			double waitUs = deadline - (double)margin - nowUs();
			if(waitUs > 0)
			{
				{
					std::lock_guard<std::mutex> sl(shared->m);
					shared->c.latchWaits++;
				}
				std::unique_lock<std::mutex> lk(slotMutex);
				slotCv.wait_for(lk, std::chrono::microseconds((int64_t)waitUs), [&] { return slot.buf != nullptr || renderStop.load(); });
				f = slot;
				slot = DecodedFrame();
				lk.unlock();
				if(f.buf)
				{
					std::lock_guard<std::mutex> sl(shared->m);
					shared->c.latchHits++;
				}
			}
		}
		if(!f.buf)
		{
			std::lock_guard<std::mutex> sl(shared->m);
			shared->c.ticksWithoutNewFrame++;
			return;
		}
		f.rec->targetPresentUs = target;
		renderFrame(f, update.drawable, target);
	}

	// Any thread: uses only the device and the library, which never change after create().
	id<MTLRenderPipelineState> makePipeline(Frag frag, MTLPixelFormat fmt) const
	{
		static const char *names[] = {"fs_bilinear", "fs_bicubic", "fs_lanczos", "fs_copy",
			"fs_bilinear_nr", "fs_bicubic_nr", "fs_lanczos_nr", "fs_overlay", "fs_rgb", "fs_grid_build"};
		MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
		d.vertexFunction = [library newFunctionWithName:@"vs_fullscreen"];
		d.fragmentFunction = [library newFunctionWithName:@(names[frag])];
		d.colorAttachments[0].pixelFormat = fmt;
		if(frag == FragOverlay)
		{
			// Premultiplied alpha over what is there; the target's own alpha stays 1.
			MTLRenderPipelineColorAttachmentDescriptor *a = d.colorAttachments[0];
			a.blendingEnabled = YES;
			a.rgbBlendOperation = MTLBlendOperationAdd;
			a.alphaBlendOperation = MTLBlendOperationAdd;
			a.sourceRGBBlendFactor = MTLBlendFactorOne;
			a.sourceAlphaBlendFactor = MTLBlendFactorOne;
			a.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
			a.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
		}
		NSError *err = nil;
		id<MTLRenderPipelineState> p = [device newRenderPipelineStateWithDescriptor:d error:&err];
		if(!p)
			NSLog(@"plk-video: pipeline %s failed: %@", names[frag], err);
		return p;
	}

	id<MTLRenderPipelineState> pipelineFor(Frag frag, MTLPixelFormat fmt)
	{
		auto key = std::make_pair((int)frag, fmt);
		auto it = pipelines.find(key);
		if(it != pipelines.end())
			return it->second;
		id<MTLRenderPipelineState> p = makePipeline(frag, fmt);
		pipelines[key] = p;
		return p;
	}

	// A state that is used only if it is already there: building one takes a frame's
	// worth of time many times over, so the mixed-in path never builds its own (setEnhancer
	// does, off the render thread).
	id<MTLRenderPipelineState> pipelineIfBuilt(Frag frag, MTLPixelFormat fmt)
	{
		auto it = pipelines.find(std::make_pair((int)frag, fmt));
		return it == pipelines.end() ? nil : it->second;
	}

	static Frag mixedIn(Frag frag)
	{
		return frag == FragBicubic ? FragBicubicNr : frag == FragLanczos ? FragLanczosNr : FragBilinearNr;
	}

	void configureLayer(const ColorInfo &c, const std::shared_ptr<const HdrMeta> &hdr)
	{
		CFStringRef csName = colorSpaceName(c, cfg.hdrOutput, cfg.sdrColor);
		CGColorSpaceRef cs = CGColorSpaceCreateWithName(csName);
		[CATransaction begin];
		[CATransaction setDisableActions:YES];
		layer.pixelFormat = outputFormatFor(c, cfg.hdrOutput);
		layer.colorspace = cs;
		layer.wantsExtendedDynamicRangeContent = c.hdr();
		layer.EDRMetadata = edrMetadataFor(c, hdr);
		[CATransaction commit];
		CGColorSpaceRelease(cs);
		layerColor = c;
		layerHdr = hdr;
		layerBrightness = cfg.hdrBrightness;
		layerConfigured = true;
		std::lock_guard<std::mutex> sl(shared->m);
		shared->c.layerColorSpace = ((__bridge NSString *)csName).UTF8String;
	}

	CAEDRMetadata *edrMetadataFor(const ColorInfo &c, const std::shared_ptr<const HdrMeta> &hdr)
	{
		if(!c.hdr())
			return nil;
		if(c.transfer == Transfer::HLG)
			return [CAEDRMetadata HLGMetadata];
		// No metadata: macOS leaves the picture alone (the shader has fitted it to the
		// display) and a fullscreen one can go straight to the display.
		if(cfg.hdrToneMap == HdrToneMap::App && c.transfer == Transfer::PQ)
		{
			std::lock_guard<std::mutex> sl(shared->m);
			shared->c.hdrMetadata = "none on the layer: tone mapped by the app";
			return nil;
		}
		// The picture is scaled in linear light (hdrBrightness), so the range it is said
		// to cover is scaled with it.
		const float gain = cfg.hdrBrightness;
		const bool scaled = brightnessScaled(gain);
		// All-zero values mean "unknown" in both SEI messages; treat as absent.
		bool hasMd = hdr && hdr->sei.hasMasteringDisplay
			&& (hdr->sei.masteringDisplay[16] | hdr->sei.masteringDisplay[17] | hdr->sei.masteringDisplay[18] | hdr->sei.masteringDisplay[19]);
		bool hasCll = hdr && hdr->sei.hasContentLightLevel
			&& (hdr->sei.contentLightLevel[0] | hdr->sei.contentLightLevel[1] | hdr->sei.contentLightLevel[2] | hdr->sei.contentLightLevel[3]);
		if(hasMd || hasCll)
		{
			const auto &md = hdr->sei.masteringDisplay;
			const auto &cl = hdr->sei.contentLightLevel;
			char desc[160];
			// MDCV luminance fields are in 0.0001 cd/m2 units (D.2.28).
			const double mdMin = hasMd ? ((md[20] << 24 | md[21] << 16 | md[22] << 8 | md[23]) / 10000.0) : 0.0;
			const double mdMax = hasMd ? ((uint32_t)(md[16] << 24 | md[17] << 16 | md[18] << 8 | md[19]) / 10000.0) : 0.0;
			const unsigned maxCll = (unsigned)(cl[0] << 8 | cl[1]);
			int n = snprintf(desc, sizeof desc, "SEI: mastering %s%.4f-%.0f nits, CLL %s%u/%u nits",
					hasMd ? "" : "(none) ", mdMin, mdMax,
					hasCll ? "" : "(none) ", maxCll, (unsigned)(cl[2] << 8 | cl[3]));
			if(scaled && n > 0 && (size_t)n < sizeof desc)
				snprintf(desc + n, sizeof desc - (size_t)n, ", brightness %.0f %%", gain * 100.0f);
			{
				std::lock_guard<std::mutex> sl(shared->m);
				shared->c.hdrMetadata = desc;
			}
			if(scaled)
			{
				const float lo = (float)(hasMd ? mdMin : 0.005) * gain;
				const float hi = (float)(hasMd ? mdMax : (maxCll ? maxCll : 1000.0)) * gain;
				return [CAEDRMetadata HDR10MetadataWithMinLuminance:lo maxLuminance:hi opticalOutputScale:100.0f];
			}
			NSData *mdcv = hasMd ? [NSData dataWithBytes:md.data() length:24] : nil;
			NSData *cll = hasCll ? [NSData dataWithBytes:cl.data() length:4] : nil;
			return [CAEDRMetadata HDR10MetadataWithDisplayInfo:mdcv contentInfo:cll opticalOutputScale:100.0f];
		}
		// No SEI: PS5-like defaults (1000-nit mastering display).
		{
			char desc[96];
			if(scaled)
				snprintf(desc, sizeof desc, "defaults: mastering 0.005-1000 nits, brightness %.0f %%", gain * 100.0f);
			else
				snprintf(desc, sizeof desc, "defaults: mastering 0.005-1000 nits");
			std::lock_guard<std::mutex> sl(shared->m);
			shared->c.hdrMetadata = desc;
		}
		return [CAEDRMetadata HDR10MetadataWithMinLuminance:0.005f * gain maxLuminance:1000.0f * gain opticalOutputScale:100.0f];
	}

	static MTLRegion letterbox(double srcW, double srcH, NSUInteger tw, NSUInteger th)
	{
		double sa = srcW / srcH, ta = (double)tw / (double)th;
		NSUInteger w, h;
		if(ta > sa) { h = th; w = (NSUInteger)std::lround(th * sa); }
		else { w = tw; h = (NSUInteger)std::lround(tw / sa); }
		w = std::min(w, tw);
		h = std::min(h, th);
		return MTLRegionMake2D((tw - w) / 2, (th - h) / 2, w, h);
	}

	// The picture's place in a target of tw x th: fitted into what the content insets leave.
	MTLRegion pictureRegion(double srcW, double srcH, NSUInteger tw, NSUInteger th) const
	{
		const long freeW = (long)tw - insetLeft - insetRight, freeH = (long)th - insetTop - insetBottom;
		if(freeW < kMinInsetContent || freeH < kMinInsetContent)
			return letterbox(srcW, srcH, tw, th);
		MTLRegion r = letterbox(srcW, srcH, (NSUInteger)freeW, (NSUInteger)freeH);
		r.origin.x += (NSUInteger)insetLeft;
		r.origin.y += (NSUInteger)insetTop;
		return r;
	}

	id<MTLTexture> makeTexture(MTLPixelFormat fmt, NSUInteger w, NSUInteger h, MTLTextureUsage usage)
	{
		MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:w height:h mipmapped:NO];
		d.usage = usage;
		d.storageMode = MTLStorageModePrivate;
		return [device newTextureWithDescriptor:d];
	}

	// The two maps of what is fixed to the screen (MotionState), as textures: made empty,
	// and filled in whenever the motion queue has new ones.
	id<MTLTexture> fixedTex, fixedCentreTex;
	void fixedMasks()
	{
		auto make = [&](int w, int h, int bytes) {
			MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:bytes == 2 ? MTLPixelFormatRG8Unorm : MTLPixelFormatR8Unorm
																					 width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
			d.usage = MTLTextureUsageShaderRead;
			d.storageMode = MTLStorageModeShared;
			id<MTLTexture> t = [device newTextureWithDescriptor:d];
			std::vector<uint8_t> none((size_t)w * h * bytes, 0);
			[t replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h) mipmapLevel:0 withBytes:none.data() bytesPerRow:(NSUInteger)(w * bytes)];
			return t;
		};
		if(!fixedTex)
		{
			fixedTex = make(kMotionW, kMotionH, 2);
			fixedCentreTex = make(kCentreW, kCentreH, 1);
		}
		std::vector<uint8_t> mask, centre;
		{
			std::lock_guard<std::mutex> lk(motion->m);
			if(!motion->maskNew)
				return;
			motion->maskNew = false;
			mask = motion->mask;
			centre = motion->centreMask;
		}
		if(mask.size() == (size_t)kMotionW * kMotionH * 2)
			[fixedTex replaceRegion:MTLRegionMake2D(0, 0, kMotionW, kMotionH) mipmapLevel:0 withBytes:mask.data() bytesPerRow:kMotionW * 2];
		if(centre.size() == (size_t)kCentreW * kCentreH)
			[fixedCentreTex replaceRegion:MTLRegionMake2D(0, 0, kCentreW, kCentreH) mipmapLevel:0 withBytes:centre.data() bytesPerRow:kCentreW];
	}

	void drawPass(id<MTLCommandBuffer> cb, id<MTLTexture> target, MTLRegion vp, id<MTLRenderPipelineState> pso,
			id<MTLTexture> t0, id<MTLTexture> t1, const ShaderParams *params, const NrDraw *nr = nullptr,
			const OverlayDraw *overlay = nullptr)
	{
		MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
		rp.colorAttachments[0].texture = target;
		rp.colorAttachments[0].loadAction = MTLLoadActionClear;
		rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
		rp.colorAttachments[0].storeAction = MTLStoreActionStore;
		id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
		[enc setRenderPipelineState:pso];
		[enc setViewport:(MTLViewport){(double)vp.origin.x, (double)vp.origin.y, (double)vp.size.width, (double)vp.size.height, 0.0, 1.0}];
		[enc setFragmentTexture:t0 atIndex:0];
		if(t1)
			[enc setFragmentTexture:t1 atIndex:1];
		if(params)
		{
			[enc setFragmentBytes:params length:sizeof(ShaderParams) atIndex:0];
			// What the camera warp leaves where it is. Always there, as the shaders name them.
			fixedMasks();
			[enc setFragmentTexture:fixedTex atIndex:6];
			[enc setFragmentTexture:fixedCentreTex atIndex:7];
		}
		if(nr)
		{
			[enc setFragmentTexture:nr->key atIndex:2];
			[enc setFragmentTexture:nr->out atIndex:3];
			// Something is bound whatever the mix: the shader names the texture either way.
			[enc setFragmentTexture:nr->grid ?: nr->key atIndex:4];
			[enc setFragmentBytes:&nr->params length:sizeof(NrShaderParams) atIndex:1];
		}
		[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		if(overlay)
		{
			const MTLRegion &at = overlay->at;
			[enc setRenderPipelineState:overlay->pso];
			[enc setViewport:(MTLViewport){(double)at.origin.x, (double)at.origin.y, (double)at.size.width, (double)at.size.height, 0.0, 1.0}];
			[enc setFragmentTexture:overlay->tex atIndex:0];
			[enc setFragmentBytes:&overlay->params length:sizeof(OverlayShaderParams) atIndex:0];
			[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		}
		[enc endEncoding];
	}

	// The overlay as this frame's last pass draws it, or false: none, it does not fit, or
	// its render state is not there (setOverlay() builds it, off this thread).
	bool overlayFor(id<MTLTexture> target, MTLRegion dest, const ColorInfo &ci, bool linear, OverlayDraw &out)
	{
		if(!overlayTex)
			return false;
		const long x = (long)dest.origin.x + overlayX, y = (long)dest.origin.y + overlayY;
		if(x < 0 || y < 0 || x + (long)overlayTex.width > (long)target.width || y + (long)overlayTex.height > (long)target.height)
			return false;
		out.pso = pipelineIfBuilt(FragOverlay, target.pixelFormat);
		if(!out.pso)
			return false;
		out.tex = overlayTex;
		out.at = MTLRegionMake2D((NSUInteger)x, (NSUInteger)y, overlayTex.width, overlayTex.height);
		out.params = {};
		if(linear)
			out.params.mode = simd_make_float2(ci.primaries == Primaries::BT2020 ? 2.0f : 3.0f, 1.0f);
		else if(ci.transfer == Transfer::PQ)
			out.params.mode = simd_make_float2(1.0f, kOverlayWhiteNits / 10000.0f);
		return true;
	}

	// ---- enhancer ----------------------------------------------------------

	// Forgets the result in hand and whatever is still on its way: after this, nothing
	// made from an earlier picture, size or enhancer is mixed in.
	void dropEnhanced()
	{
		{
			std::lock_guard<std::mutex> lk(enhance->m);
			enhance->generation++;
			enhance->latest.reset();
		}
		enhance->needReset.store(true);
	}

	// An 8-bit SDR picture, whose code values the enhancer and the mix work on, or a PQ
	// picture, of which they work on an SDR rendition. Not HLG: no stream here sends it.
	bool enhancing(const ColorInfo &ci) const
	{
		return enhancer && enhanceCfg.enabled && (ci.transfer == Transfer::PQ || (!ci.hdr() && ci.bits == 8));
	}

	// The enhancer's pictures: 8 bits for an SDR picture, as the stream has them. Half
	// float for a PQ picture: its rendition packs the highlights into few code values,
	// and a step of 8 bits there would be a visible step of brightness.
	static OSType enhancerFormat(const ColorInfo &ci)
	{
		return ci.transfer == Transfer::PQ ? kCVPixelFormatType_64RGBAHalf : kCVPixelFormatType_32BGRA;
	}

	static MTLPixelFormat enhancerTextureFormat(OSType format)
	{
		return format == kCVPixelFormatType_64RGBAHalf ? MTLPixelFormatRGBA16Float
			: format == kCVPixelFormatType_32BGRA ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatInvalid;
	}

	CVMetalTextureRef wrapEnhancerBuffer(CVPixelBufferRef buf, bool renderTarget)
	{
		MTLPixelFormat format = enhancerTextureFormat(CVPixelBufferGetPixelFormatType(buf));
		if(format == MTLPixelFormatInvalid || !CVPixelBufferGetIOSurface(buf))
			return nullptr;
		NSDictionary *attrs = @{(__bridge NSString *)kCVMetalTextureUsage :
			@(MTLTextureUsageShaderRead | (renderTarget ? MTLTextureUsageRenderTarget : 0))};
		CVMetalTextureRef tex = nullptr;
		CVReturn r = CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, texCache, buf,
				(__bridge CFDictionaryRef)attrs, format, CVPixelBufferGetWidth(buf),
				CVPixelBufferGetHeight(buf), 0, &tex);
		if(r != kCVReturnSuccess || !tex || !CVMetalTextureGetTexture(tex))
		{
			if(tex) CFRelease(tex);
			return nullptr;
		}
		return tex;
	}

	// The enhancer's newest result if it can be mixed into a frame decoded at `decodedUs`:
	// fills `draw` and returns what must stay alive until the GPU is done with the frame.
	std::shared_ptr<EnhancedFrame> enhancedFor(double decodedUs, const ColorInfo &ci, NrDraw &draw, double *ageMs)
	{
		if(enhanceCfg.strength <= 0)
			return nullptr;
		std::shared_ptr<EnhancedFrame> e;
		{
			std::lock_guard<std::mutex> lk(enhance->m);
			e = enhance->latest;
		}
		// A result made of the picture's other kind (the stream changed between SDR and
		// HDR) is not a change to this one.
		if(!e || CVPixelBufferGetPixelFormatType(e->key->buf) != enhancerFormat(ci))
			return nullptr;
		double age = (decodedUs - e->key->decodedUs) / 1000.0;
		double maxAge = (double)enhanceCfg.maxAgeMs;
		if(age < 0 || age >= maxAge)
			return nullptr;
		if(!e->outTex)
			e->outTex = wrapEnhancerBuffer(e->out, false);
		if(!e->outTex)
			return nullptr;
		double t = std::min(1.0, std::max(0.0, (age - maxAge / 2) / (maxAge / 2)));
		double fade = 1.0 - t * t * (3.0 - 2.0 * t);
		// The key frame's primaries: the stream's for an SDR picture, BT.709 for the
		// rendition of a PQ picture.
		double kr = 0.2126, kb = 0.0722;
		if(ci.transfer != Transfer::PQ)
			lumaCoefficients(ci.matrix, kr, kb);
		draw.key = CVMetalTextureGetTexture(e->key->tex);
		draw.out = CVMetalTextureGetTexture(e->outTex);
		draw.params.lumaWeights = simd_make_float4((float)kr, (float)(1.0 - kr - kb), (float)kb, 0.0f);
		draw.params.keySize = simd_make_float2((float)draw.key.width, (float)draw.key.height);
		float low = std::max(0.0f, enhanceCfg.motionLow);
		draw.params.motion = simd_make_float2(low, std::max(low + 1e-4f, enhanceCfg.motionHigh));
		draw.params.strength = (float)(std::min(2.0f, enhanceCfg.strength) * fade);
		draw.params.split = std::min(1.0f, std::max(0.0f, enhanceCfg.compareSplit));
		draw.params.guided = 0.0f;
		draw.params.unused = 0.0f;
		draw.grid = nil;
		*ageMs = age;
		return e;
	}

	// EnhanceConfig::guided: makes sure the table of `e`'s change exists (drawn into `cb`,
	// ahead of the pass that uses it) and names it in `draw`. One small pass per result.
	void useGrid(id<MTLCommandBuffer> cb, const std::shared_ptr<EnhancedFrame> &e, NrDraw &draw)
	{
		if(!enhanceCfg.guided || enhanceCfg.wait || !e)
			return;
		id<MTLRenderPipelineState> pso = pipelineIfBuilt(FragGridBuild, MTLPixelFormatRGBA16Float);
		if(!pso)
			return;
		uint64_t generationNow;
		{
			std::lock_guard<std::mutex> lk(enhance->m);
			generationNow = enhance->generation;
		}
		if(generationNow != gridGeneration)
		{
			gridGeneration = generationNow;
			gridValid = false;
		}
		if(!gridValid || gridSeq != e->key->seq)
		{
			for(auto &t : gridTex)
				if(!t)
					t = makeTexture(MTLPixelFormatRGBA16Float, kGridWidth, kGridHeight,
							MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
			const int next = 1 - gridIndex;
			MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
			rp.colorAttachments[0].texture = gridTex[next];
			rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
			rp.colorAttachments[0].storeAction = MTLStoreActionStore;
			id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
			[enc setRenderPipelineState:pso];
			[enc setViewport:(MTLViewport){0.0, 0.0, (double)kGridWidth, (double)kGridHeight, 0.0, 1.0}];
			[enc setFragmentTexture:draw.key atIndex:0];
			[enc setFragmentTexture:draw.out atIndex:1];
			[enc setFragmentTexture:gridTex[gridIndex] atIndex:2];
			GridShaderParams gp = {};
			gp.lumaWeights = draw.params.lumaWeights;
			// Half of the table before it stays: a table is an average of few samples, and
			// two of them differ by more than the picture did.
			gp.blend = gridValid ? 0.5f : 0.0f;
			[enc setFragmentBytes:&gp length:sizeof(gp) atIndex:0];
			[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
			[enc endEncoding];
			gridIndex = next;
			gridSeq = e->key->seq;
			gridValid = true;
		}
		draw.grid = gridTex[gridIndex];
		draw.params.guided = 1.0f;
		// The detail is held to the same test of stillness as a whole change is without the
		// table. Measured on three PS5 clips against each frame's own result (2026-11-27): a
		// stricter test (0.012 to 0.045, 0.006 to 0.025) was no better and on two clips worse,
		// and no detail at all, the table alone, still made 70 % of the network's change.
	}

	// Draws this frame for the enhancer and hands it over, if the enhancer is idle. Runs
	// after the frame's own command buffer has been committed: on the GPU the copy comes
	// after the presented picture, and nothing here is waited for.
	uint64_t offerToEnhancer(const std::shared_ptr<FrameRecord> &rec, CVPixelBufferRef buf, CVMetalTextureRef ty,
			CVMetalTextureRef tc, const ShaderParams &shown, CGRect clean, const ColorInfo &ci)
	{
		if(enhance->busy.load() || nowUs() < enhance->holdUntilUs.load())
			return 0;
		size_t sw = (size_t)clean.size.width, sh = (size_t)clean.size.height;
		size_t ph = enhanceCfg.processHeight <= 0 || (size_t)enhanceCfg.processHeight >= sh
			? sh : (size_t)std::max(enhanceCfg.processHeight, 320);
		size_t pw = ph == sh ? sw : (size_t)std::llround((double)sw * (double)ph / (double)sh);
		pw += pw & 1;
		if(pw < 64 || ph < 64)
			return 0;
		const OSType format = enhancerFormat(ci);
		if(!keyPool || pw != keyPoolW || ph != keyPoolH || format != keyPoolFormat)
		{
			if(keyPool)
				CVPixelBufferPoolRelease(keyPool);
			keyPool = nullptr;
			NSDictionary *attrs = @{
				(__bridge NSString *)kCVPixelBufferPixelFormatTypeKey : @(format),
				(__bridge NSString *)kCVPixelBufferWidthKey : @(pw),
				(__bridge NSString *)kCVPixelBufferHeightKey : @(ph),
				(__bridge NSString *)kCVPixelBufferIOSurfacePropertiesKey : @{},
				(__bridge NSString *)kCVPixelBufferMetalCompatibilityKey : @YES,
			};
			if(CVPixelBufferPoolCreate(kCFAllocatorDefault, nullptr, (__bridge CFDictionaryRef)attrs, &keyPool) != kCVReturnSuccess)
			{
				keyPool = nullptr;
				return 0;
			}
			keyPoolW = pw;
			keyPoolH = ph;
			keyPoolFormat = format;
			dropEnhanced();
			std::lock_guard<std::mutex> sl(shared->m);
			shared->c.enhanceWidth = (int)pw;
			shared->c.enhanceHeight = (int)ph;
		}
		auto key = std::make_shared<KeyFrame>();
		if(CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, keyPool, &key->buf) != kCVReturnSuccess || !key->buf)
			return 0;
		key->tex = wrapEnhancerBuffer(key->buf, true);
		id<MTLRenderPipelineState> pso = pipelineFor(FragBilinear, enhancerTextureFormat(format));
		if(!key->tex || !pso)
			return 0;
		// The picture as the enhancer works on it, whatever the frame on screen was made
		// into: the code values of an SDR picture, the SDR rendition of a PQ picture.
		ShaderParams params = shown;
		params.linearize = simd_make_float2(ci.transfer == Transfer::PQ ? 3.0f : 0.0f, 0.0f);
		params.toneMap = simd_make_float2(0.0f, 0.0f);
		key->seq = ++keySeq;
		key->decodedUs = rec->decodedUs;
		// A pause in the offers (a stalled stream, the enhancer switched off for a while)
		// breaks the continuity the enhancer's own history relies on.
		if(lastOfferUs > 0 && rec->decodedUs - lastOfferUs > 500000.0)
			enhance->needReset.store(true);
		lastOfferUs = rec->decodedUs;
		uint64_t generation;
		{
			std::lock_guard<std::mutex> lk(enhance->m);
			generation = enhance->generation;
		}

		id<MTLCommandBuffer> cb = [queue commandBuffer];
		drawPass(cb, CVMetalTextureGetTexture(key->tex), MTLRegionMake2D(0, 0, pw, ph), pso,
				CVMetalTextureGetTexture(ty), CVMetalTextureGetTexture(tc), &params);
		CFRetain(ty);
		CFRetain(tc);
		CVPixelBufferRetain(buf);
		enhance->busy.store(true);
		std::shared_ptr<EnhanceState> st = enhance;
		std::shared_ptr<Shared> sh2 = shared;
		std::shared_ptr<FrameEnhancer> target = enhancer;
		[cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
			CFRelease(ty);
			CFRelease(tc);
			CVPixelBufferRelease(buf);
			bool drawn = c.status == MTLCommandBufferStatusCompleted;
			double inputGpuMs = drawn && c.GPUEndTime > c.GPUStartTime ? (c.GPUEndTime - c.GPUStartTime) * 1000.0 : 0.0;
			double handedUs = nowUs();
			bool reset = st->needReset.exchange(false);
			bool taken = drawn && target->submit(key->buf, key->seq, reset,
					[st, sh2, key, generation, handedUs, inputGpuMs](CVPixelBufferRef output, double enhancerMs) {
						std::shared_ptr<EnhancedFrame> e;
						if(output && CVPixelBufferGetPixelFormatType(output) == CVPixelBufferGetPixelFormatType(key->buf)
							&& CVPixelBufferGetIOSurface(output)
							&& CVPixelBufferGetWidth(output) == CVPixelBufferGetWidth(key->buf)
							&& CVPixelBufferGetHeight(output) == CVPixelBufferGetHeight(key->buf))
						{
							e = std::make_shared<EnhancedFrame>();
							e->key = key;
							e->out = CVPixelBufferRetain(output);
						}
						std::shared_ptr<EnhancedFrame> old;
						{
							std::lock_guard<std::mutex> lk(st->m);
							if(e && st->generation == generation)
							{
								old = st->latest;
								st->latest = e;
							}
						}
						old.reset();
						if(!e)
							st->needReset.store(true);
						st->busy.store(false);
						EnhanceTiming timing;
						timing.seq = key->seq;
						timing.ok = e != nullptr;
						timing.width = (int)CVPixelBufferGetWidth(key->buf);
						timing.height = (int)CVPixelBufferGetHeight(key->buf);
						timing.inputGpuMs = inputGpuMs;
						timing.enhancerMs = enhancerMs;
						timing.totalMs = (nowUs() - handedUs) / 1000.0;
						if(e)
						{
							double smooth = st->roundTripMs.load();
							st->roundTripMs.store(smooth <= 0 ? timing.totalMs : smooth * 0.8 + timing.totalMs * 0.2);
						}
						// Idle, and with a result or without: frames that wait hear of it.
						st->signal();
						{
							std::lock_guard<std::mutex> sl(sh2->m);
							if(e)
							{
								sh2->c.enhanceCompleted++;
								sh2->enhance.add(timing.totalMs);
							}
							else
								sh2->c.enhanceFailed++;
						}
						sh2->report(timing);
					});
			if(!taken)
			{
				// Refused: do not ask again with every frame.
				st->needReset.store(true);
				st->holdUntilUs.store(nowUs() + 250000.0);
				st->busy.store(false);
				st->signal();
			}
			std::lock_guard<std::mutex> sl(sh2->m);
			if(taken)
				sh2->c.enhanceOffered++;
			else
				sh2->c.enhanceFailed++;
			if(inputGpuMs > 0)
				sh2->enhanceInputGpu.add(inputGpuMs);
		}];
		[cb commit];
		lastKeyCommandBuffer = cb;
		rec->enhanceOffered = true;
		return key->seq;
	}

	// ---- which frames the enhancer gets, and which frames wait for it -------

	// The plane textures of a decoded frame and the shader parameters that depend only on
	// the frame: what offerToEnhancer() needs of a frame that is not being rendered now.
	bool keyPlanes(CVPixelBufferRef buf, const ColorInfo &ci, CVMetalTextureRef *ty, CVMetalTextureRef *tc,
			ShaderParams &params, CGRect &clean)
	{
		bool ten = ci.bits > 8;
		size_t w0 = CVPixelBufferGetWidthOfPlane(buf, 0), h0 = CVPixelBufferGetHeightOfPlane(buf, 0);
		size_t w1 = CVPixelBufferGetWidthOfPlane(buf, 1), h1 = CVPixelBufferGetHeightOfPlane(buf, 1);
		*ty = *tc = nullptr;
		CVReturn r0 = CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, texCache, buf, nullptr,
				ten ? MTLPixelFormatR16Unorm : MTLPixelFormatR8Unorm, w0, h0, 0, ty);
		CVReturn r1 = CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, texCache, buf, nullptr,
				ten ? MTLPixelFormatRG16Unorm : MTLPixelFormatRG8Unorm, w1, h1, 1, tc);
		if(r0 != kCVReturnSuccess || r1 != kCVReturnSuccess || !*ty || !*tc)
		{
			if(*ty) CFRelease(*ty);
			if(*tc) CFRelease(*tc);
			*ty = *tc = nullptr;
			return false;
		}
		clean = CVImageBufferGetCleanRect(buf);
		if(clean.size.width <= 0 || clean.size.height <= 0)
			clean = CGRectMake(0, 0, w0, h0);
		params = {};
		colorRows(ci, params);
		params.lumaSize = simd_make_float2((float)w0, (float)h0);
		params.chromaSize = simd_make_float2((float)w1, (float)h1);
		params.uvScale = simd_make_float2((float)(clean.size.width / w0), (float)(clean.size.height / h0));
		params.uvOffset = simd_make_float2((float)(clean.origin.x / w0), (float)(clean.origin.y / h0));
		params.chromaShift = simd_make_float2(ci.chromaLeft ? 0.25f : 0.0f, ci.chromaTop ? 0.25f : 0.0f);
		params.sdrMap = simd_make_float2(ci.transfer == Transfer::PQ ? 10000.0f / kEnhancerSdrWhiteNits : 0.0f, 0.0f);
		return true;
	}

	// Offers a frame that is not being rendered now. 0 when it could not be offered.
	uint64_t offerFrame(const DecodedFrame &f)
	{
		ColorInfo ci = colorInfoFor(f.buf);
		CVMetalTextureRef ty = nullptr, tc = nullptr;
		ShaderParams params;
		CGRect clean;
		if(!keyPlanes(f.buf, ci, &ty, &tc, params, clean))
			return 0;
		uint64_t seq = offerToEnhancer(f.rec, f.buf, ty, tc, params, clean, ci);
		CFRelease(ty);
		CFRelease(tc);
		return seq;
	}

	// EnhanceConfig::wait is honoured for this frame: there is an enhancer for its kind of
	// picture, it takes frames, and frames are shown as they come.
	bool waitsForEnhancer(const DecodedFrame &f)
	{
		return enhanceCfg.wait && activePacing != PacingMode::DisplayLink && enhancing(colorInfoFor(f.buf))
			&& enhanceCfg.strength > 0 && nowUs() >= enhance->holdUntilUs.load();
	}

	void clearPending()
	{
		if(nrPending.buf)
			CVPixelBufferRelease(nrPending.buf);
		nrPending = DecodedFrame();
	}

	// ---- camera warp: how far the whole picture moved ------------------------

	static constexpr int kMotionW = 192, kMotionH = 108;

	// The luma of a decoded frame as kMotionW x kMotionH means of 4 x 4 points each.
	static bool smallLuma(CVPixelBufferRef buf, bool wide, std::vector<uint8_t> &out)
	{
		const OSType format = CVPixelBufferGetPixelFormatType(buf);
		if((format >> 24) == '&' || !CVPixelBufferIsPlanar(buf)
			|| CVPixelBufferLockBaseAddress(buf, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess)
			return false;
		const uint8_t *base = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(buf, 0);
		const size_t stride = CVPixelBufferGetBytesPerRowOfPlane(buf, 0);
		const size_t w = CVPixelBufferGetWidthOfPlane(buf, 0), h = CVPixelBufferGetHeightOfPlane(buf, 0);
		const bool ok = base && w >= (size_t)kMotionW * 4 && h >= (size_t)kMotionH * 4;
		if(ok)
		{
			out.resize((size_t)kMotionW * kMotionH);
			const size_t step = wide ? 2 : 1, high = wide ? 1 : 0;
			for(int j = 0; j < kMotionH; j++)
			{
				for(int i = 0; i < kMotionW; i++)
				{
					unsigned sum = 0;
					for(int b = 0; b < 4; b++)
					{
						const uint8_t *row = base + ((size_t)(8 * j + 2 * b + 1) * h / (size_t)(8 * kMotionH)) * stride;
						for(int a = 0; a < 4; a++)
							sum += row[((size_t)(8 * i + 2 * a + 1) * w / (size_t)(8 * kMotionW)) * step + high];
					}
					out[(size_t)j * kMotionW + i] = (uint8_t)(sum >> 4);
				}
			}
		}
		CVPixelBufferUnlockBaseAddress(buf, kCVPixelBufferLock_ReadOnly);
		return ok;
	}

	// A new picture of the game: how far it moved against the last one is measured and
	// told: see measureMotionOf() for how (piece by piece, the far pieces first).
	//
	// It takes 3 to 5 ms a picture, which no frame can wait for on the render thread: the
	// work is done on a queue of its own, one picture at a time, with the frame's buffer
	// kept until then, and the sink is told from there.
	struct MotionState {
		std::mutex m;
		std::function<void(const PictureMotion &)> sink;  // m
		std::function<void(const uint8_t *, int, int, double)> tap;  // m
		std::vector<uint8_t> last;                        // the queue only
		double lastUs = 0;
		// What stays where it is on screen while the world slides (a character the camera
		// follows, a crosshair, a HUD): see learnFixed(). The queue only, but for the two
		// masks and their flag (m).
		std::vector<uint8_t> centreLast;
		std::vector<float> stay, slide, centreStay, centreSlide;
		std::atomic<bool> busy{false};                    // a picture is being measured
		std::atomic<uint64_t> skipped{0};                 // pictures not measured because of that
		std::atomic<double> costUs{0};                    // what measuring one takes, lately
		std::atomic<double> turnSinceUs{0};               // since when the camera is being turned hard; 0: it is not
		std::vector<uint8_t> hard;                        // the queue only: what is fixed, yes or no
		std::vector<uint8_t> mask, centreMask;            // m
		bool maskNew = false;                             // m
		dispatch_queue_t queue = dispatch_queue_create("plk.video.motion", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0));
	};
	std::shared_ptr<MotionState> motion = std::make_shared<MotionState>();

	// When a picture would have arrived had the network not held it up: the stream's frames
	// are numbered and the console makes them at an even pace, so the earliest that any
	// frame has come, for its number, says when each is due. A frame's lateness against
	// that is the network's doing (over mobile data they come 27 to 40 ms apart where they
	// are made 33 apart), and the camera warp, which sets the stick against the pictures,
	// needs the time the picture was made by, not the time it got here. Render thread only.
	struct PictureClock {
		bool have = false;
		uint16_t lastIndex = 0;
		double index = 0, firstUs = 0, lastUs = 0;
		std::deque<std::pair<double, double>> recent;  // (number, arrival) of the last two seconds
		double at(uint16_t frameIndex, double arrivalUs)
		{
			const int step = (int16_t)(uint16_t)(frameIndex - lastIndex);
			if(!have || step <= 0 || step > 600 || arrivalUs - lastUs > 2e6)
			{
				have = true;
				index = 0;
				firstUs = arrivalUs;
				recent.clear();
			}
			else
				index += step;
			lastIndex = frameIndex;
			lastUs = arrivalUs;
			recent.push_back({ index, arrivalUs });
			while(recent.size() > 120)
				recent.pop_front();
			// The pace from the whole of the stream so far; until there are ten seconds of
			// it, it is not known well enough to say anything, and the arrival stands.
			if(index < 600)
				return arrivalUs;
			const double periodUs = (arrivalUs - firstUs) / index;
			double made = arrivalUs;
			for(const auto &r : recent)
				made = std::min(made, r.second + (index - r.first) * periodUs);
			return std::max(arrivalUs - 100000.0, made);
		}
	};
	// Not on unless asked for (PLK_PICTURE_CLOCK=1): written on the home network, where there
	// is nothing for it to do, and not yet seen at work over mobile data.
	const bool pictureClockOn = getenv("PLK_PICTURE_CLOCK") && atoi(getenv("PLK_PICTURE_CLOCK")) != 0;
	PictureClock pictureClock;

	void measureMotion(const DecodedFrame &f)
	{
		const double madeUs = pictureClockOn ? pictureClock.at(f.rec->frameIndex, f.rec->decodedUs) : f.rec->decodedUs;
		lastPictureArrivalUs = madeUs;
		f.rec->pictureUs = madeUs;
		if(!motionSink)
			return;
		// One at a time, and never a queue of them: a picture that comes while the one before
		// is still being measured is not measured. (They were all queued once: measuring
		// fell seconds behind the stream, what it told came too late to be set against the
		// stick, the warp learned nothing, and the frames kept waiting ran the decoder's
		// buffers short. 2026-11-27.)
		std::shared_ptr<MotionState> st = motion;
		if(st->busy.exchange(true))
		{
			st->skipped.fetch_add(1);
			return;
		}
		CVPixelBufferRef buf = CVPixelBufferRetain(f.buf);
		const bool wide = colorInfoFor(f.buf).bits > 8;
		const double pictureUs = madeUs;
		dispatch_async(st->queue, ^{
			const double began = hostTimeUs();
			measureMotionOf(*st, buf, wide, pictureUs);
			CVPixelBufferRelease(buf);
			st->costUs.store(st->costUs.load() * 0.95 + (hostTimeUs() - began) * 0.05);
			st->busy.store(false);
		});
	}

	// The middle of the picture point for point: kCentreW x kCentreH of a 1080p frame, a
	// 7.5th of the width and of the height of any. A crosshair is a few points thin, and
	// in the small picture it is lost among what is behind it.
	static constexpr int kCentreW = 256, kCentreH = 144;

	static bool centreLuma(CVPixelBufferRef buf, bool wide, std::vector<uint8_t> &out)
	{
		if(CVPixelBufferLockBaseAddress(buf, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess)
			return false;
		const uint8_t *base = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(buf, 0);
		const size_t stride = CVPixelBufferGetBytesPerRowOfPlane(buf, 0);
		const size_t w = CVPixelBufferGetWidthOfPlane(buf, 0), h = CVPixelBufferGetHeightOfPlane(buf, 0);
		const bool ok = base && w >= 640 && h >= 360;
		if(ok)
		{
			out.resize((size_t)kCentreW * kCentreH);
			const size_t step = wide ? 2 : 1, high = wide ? 1 : 0;
			const double x0 = w * (0.5 - 1.0 / 15.0), y0 = h * (0.5 - 1.0 / 15.0), sx = w / 7.5 / kCentreW, sy = h / 7.5 / kCentreH;
			for(int j = 0; j < kCentreH; j++)
			{
				const uint8_t *row = base + (size_t)(y0 + (j + 0.5) * sy) * stride;
				for(int i = 0; i < kCentreW; i++)
					out[(size_t)j * kCentreW + i] = row[(size_t)(x0 + (i + 0.5) * sx) * step + high];
			}
		}
		CVPixelBufferUnlockBaseAddress(buf, kCVPixelBufferLock_ReadOnly);
		return ok;
	}

	// What is fixed to the screen, from one pair of pictures in which the world slid by
	// (sx, sy) points of this map: a point of the world is what it was (sx, sy) away and
	// has changed where it is; a point fixed to the screen is the other way round. Both
	// differences are kept as means over the last such pairs, and a point is fixed by so
	// much as the first is small against the second (none where neither says anything: a
	// flat sky is as well moved as not).
	//
	// `whole`: the map of the whole picture, which is then made of whole things only. A point
	// here and there called fixed among points that are not tears the picture into a mosaic
	// when it is moved (seen 2026-11-27, with a rule that asked for no measured shift and
	// called much of the sand and the sea fixed): single points and thin bits are dropped,
	// what is left is grown a little, and its edge is made soft, so that the picture is
	// stretched there, not torn. The map of the middle keeps its thin lines: a crosshair is one.
	static void learnFixed(const std::vector<uint8_t> &last, const std::vector<uint8_t> &now, int w, int h, int sx, int sy,
			std::vector<float> &stay, std::vector<float> &slide, float floor, bool whole, std::vector<uint8_t> &mask,
			std::vector<uint8_t> &hard)
	{
		if(stay.size() != now.size())
		{
			stay.assign(now.size(), 0.0f);
			slide.assign(now.size(), 0.0f);
		}
		const float rate = 0.06f;
		for(int y = 0; y < h; y++)
		{
			const int fromY = y - sy;
			if(fromY < 0 || fromY >= h)
				continue;
			for(int x = 0; x < w; x++)
			{
				const int fromX = x - sx;
				if(fromX < 0 || fromX >= w)
					continue;
				const size_t at = (size_t)y * w + x;
				const float a = (float)std::abs((int)now[at] - (int)last[at]);
				float c = (float)std::abs((int)now[at] - (int)last[(size_t)fromY * w + fromX]);
				// Both ways: what was here must also not have gone where the world went. Beside
				// a fixed thing the first test alone fails for the world too (what "came from"
				// there is the fixed thing), and a border as wide as the shift, each side of
				// the character, was called fixed with it.
				const int toX = x + sx, toY = y + sy;
				if(toX >= 0 && toX < w && toY >= 0 && toY < h)
					c = std::min(c, (float)std::abs((int)now[(size_t)toY * w + toX] - (int)last[at]));
				stay[at] += (a - stay[at]) * rate;
				slide[at] += (c - slide[at]) * rate;
			}
		}
		const size_t n = now.size();
		std::vector<uint8_t> raw(n), tmp(n);
		for(size_t i = 0; i < n; i++)
		{
			const float c = slide[i], a = stay[i];
			const float f = c > floor ? std::max(0.0f, std::min(1.0f, (0.67f - a / c) * 3.0f)) : 0.0f;
			raw[i] = (uint8_t)(f * 255.0f);
		}
		// The least (`grow` false) or the most of each point and those within `r` of it.
		auto spread = [&](std::vector<uint8_t> &from, std::vector<uint8_t> &to, int r, bool grow) {
			for(int pass = 0; pass < 2; pass++)
			{
				std::vector<uint8_t> &src = pass ? to : from, &dst = pass ? from : to;
				for(int y = 0; y < h; y++)
					for(int x = 0; x < w; x++)
					{
						uint8_t v = grow ? 0 : 255;
						for(int k = -r; k <= r; k++)
						{
							const int xx = pass ? x : std::max(0, std::min(w - 1, x + k)), yy = pass ? std::max(0, std::min(h - 1, y + k)) : y;
							const uint8_t u = src[(size_t)yy * w + xx];
							v = grow ? std::max(v, u) : std::min(v, u);
						}
						dst[(size_t)y * w + x] = v;
					}
			}
		};
		if(!whole)
		{
			// A point by itself is nothing: it stays only with seven others among the 24
			// round it (a line two points thick has nine).
			for(int y = 0; y < h; y++)
				for(int x = 0; x < w; x++)
				{
					int with = 0;
					for(int j = std::max(0, y - 2); j <= std::min(h - 1, y + 2); j++)
						for(int i = std::max(0, x - 2); i <= std::min(w - 1, x + 2); i++)
							with += raw[(size_t)j * w + i] >= 128 ? 1 : 0;
					tmp[(size_t)y * w + x] = raw[(size_t)y * w + x] >= 128 && with >= 8 ? 255 : 0;
				}
			raw = tmp;
			// Three points beyond: a crosshair half covered is drawn twice, once where it
			// is and once where the picture beside it was fetched from.
			spread(raw, tmp, 3, true);
			mask = raw;
			hard = raw;
			return;
		}
		for(size_t i = 0; i < n; i++)
			raw[i] = raw[i] >= 128 ? 255 : 0;
		spread(raw, tmp, 1, false);   // what is not three points thick each way goes
		// What is left, back to its size and four points beyond. Generously: a fixed thing not
		// wholly covered is drawn twice, once where it is and once where the world beside it
		// was fetched from (an arm beside the character, a slice of the minimap beside it),
		// which is worse than a little too much of the picture standing still.
		spread(raw, tmp, 4, true);
		// A HUD is a box, and what is inside it may well move (a minimap turns with the
		// camera): a fixed thing of some size near the picture's edge is taken with the whole
		// box round it. Then the map is left as it is, all or nothing: a point half fixed is
		// moved half as far as the one beside it, and the picture ripples there (seen
		// 2026-11-27 with a soft edge: the minimap bent like a flag and the sand round the
		// character's legs waved).
		// Twice: the first round's boxes, each a point larger all round, join what belongs to
		// one HUD (a minimap came out as two boxes with a strip of world between them).
		for(int round = 0; round < 2; round++)
		{
		const int grow = round == 0 ? 2 : 1;
		std::vector<int> label(n, 0);
		std::vector<int> stack;
		int next = 0;
		for(size_t start = 0; start < n; start++)
		{
			if(raw[start] < 128 || label[start])
				continue;
			next++;
			int x0 = w, x1 = 0, y0 = h, y1 = 0, area = 0;
			stack.assign(1, (int)start);
			label[start] = next;
			while(!stack.empty())
			{
				const int at = stack.back();
				stack.pop_back();
				const int x = at % w, y = at / w;
				x0 = std::min(x0, x);
				x1 = std::max(x1, x);
				y0 = std::min(y0, y);
				y1 = std::max(y1, y);
				area++;
				const int around[4] = { x > 0 ? at - 1 : -1, x < w - 1 ? at + 1 : -1, y > 0 ? at - w : -1, y < h - 1 ? at + w : -1 };
				for(int o : around)
					if(o >= 0 && raw[(size_t)o] >= 128 && !label[(size_t)o])
					{
						label[(size_t)o] = next;
						stack.push_back(o);
					}
			}
			const double cx = (x0 + x1) / 2.0 / w, cy = (y0 + y1) / 2.0 / h;
			const bool atEdge = cx < 0.25 || cx > 0.75 || cy < 0.2 || cy > 0.8;
			const int box = (x1 - x0 + 1) * (y1 - y0 + 1);
			if(atEdge && area >= 24 && box <= w * h / 12)
				for(int y = std::max(0, y0 - grow); y <= std::min(h - 1, y1 + grow); y++)
					for(int x = std::max(0, x0 - grow); x <= std::min(w - 1, x1 + grow); x++)
						raw[(size_t)y * w + x] = 255;
		}
		}
		// Not "fixed or not" but how far from the nearest fixed point, in points of this map
		// (a byte: 255 at a fixed point, 8 less for each point away). The picture is then
		// moved the less the nearer to something fixed, over a distance that grows with the
		// move itself: stretched round the character, not cut along a box (the cut showed as
		// lines through the surf and the sand each side of him).
		// That is for what stands in the picture's middle (the character). What is fixed at
		// its edge (a HUD) is small, and the picture is cut round it instead: stretching
		// reaches twice the move's length, and bent a vapour trail a fifth of the picture
		// away from an ammunition count.
		std::vector<uint8_t> edge(n, 0);
		{
			std::vector<int> label(n, 0);
			std::vector<int> stack, members;
			for(size_t start = 0; start < n; start++)
			{
				if(raw[start] < 128 || label[start])
					continue;
				double sx2 = 0, sy2 = 0;
				members.clear();
				stack.assign(1, (int)start);
				label[start] = 1;
				while(!stack.empty())
				{
					const int at = stack.back();
					stack.pop_back();
					members.push_back(at);
					const int x = at % w, y = at / w;
					sx2 += x;
					sy2 += y;
					const int around[4] = { x > 0 ? at - 1 : -1, x < w - 1 ? at + 1 : -1, y > 0 ? at - w : -1, y < h - 1 ? at + w : -1 };
					for(int o : around)
						if(o >= 0 && raw[(size_t)o] >= 128 && !label[(size_t)o])
						{
							label[(size_t)o] = 1;
							stack.push_back(o);
						}
				}
				const double cx = sx2 / members.size() / w, cy = sy2 / members.size() / h;
				if((cx < 0.25 || cx > 0.75 || cy < 0.2 || cy > 0.8) && (int)members.size() <= w * h / 12)
					for(int at : members)
						edge[(size_t)at] = 255;
			}
		}
		std::vector<float> far(n);
		for(size_t i = 0; i < n; i++)
			far[i] = raw[i] >= 128 && !edge[i] ? 0.0f : 1e6f;
		for(int y = 0; y < h; y++)
			for(int x = 0; x < w; x++)
			{
				float &d = far[(size_t)y * w + x];
				if(x > 0) d = std::min(d, far[(size_t)y * w + x - 1] + 1.0f);
				if(y > 0) d = std::min(d, far[(size_t)(y - 1) * w + x] + 1.0f);
				if(x > 0 && y > 0) d = std::min(d, far[(size_t)(y - 1) * w + x - 1] + 1.414f);
				if(x < w - 1 && y > 0) d = std::min(d, far[(size_t)(y - 1) * w + x + 1] + 1.414f);
			}
		for(int y = h - 1; y >= 0; y--)
			for(int x = w - 1; x >= 0; x--)
			{
				float &d = far[(size_t)y * w + x];
				if(x < w - 1) d = std::min(d, far[(size_t)y * w + x + 1] + 1.0f);
				if(y < h - 1) d = std::min(d, far[(size_t)(y + 1) * w + x] + 1.0f);
				if(x < w - 1 && y < h - 1) d = std::min(d, far[(size_t)(y + 1) * w + x + 1] + 1.414f);
				if(x > 0 && y < h - 1) d = std::min(d, far[(size_t)(y + 1) * w + x - 1] + 1.414f);
			}
		hard = raw;
		// Two bytes a point: how near the middle's fixed things are, and whether this is one of the edge's.
		mask.resize(n * 2);
		for(size_t i = 0; i < n; i++)
		{
			mask[i * 2] = (uint8_t)std::max(0.0f, 255.0f - far[i] * 8.0f);
			mask[i * 2 + 1] = edge[i];
		}
	}

	static void measureMotionOf(MotionState &st, CVPixelBufferRef buf, bool wide, double pictureUs)
	{
		{
			// The small grey picture for whoever looks on.
			std::function<void(const uint8_t *, int, int, double)> tap;
			{
				std::lock_guard<std::mutex> lk(st.m);
				tap = st.tap;
			}
			if(tap && CVPixelBufferIsPlanar(buf) && CVPixelBufferLockBaseAddress(buf, kCVPixelBufferLock_ReadOnly) == kCVReturnSuccess)
			{
				const int tw = 480, th = 270;
				const uint8_t *base = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(buf, 0);
				const size_t stride = CVPixelBufferGetBytesPerRowOfPlane(buf, 0);
				const size_t w = CVPixelBufferGetWidthOfPlane(buf, 0), h = CVPixelBufferGetHeightOfPlane(buf, 0);
				std::vector<uint8_t> grey((size_t)tw * th);
				const size_t step = wide ? 2 : 1, high = wide ? 1 : 0;
				if(base)
					for(int j = 0; j < th; j++)
					{
						const uint8_t *row = base + ((size_t)(2 * j + 1) * h / (size_t)(2 * th)) * stride;
						for(int i = 0; i < tw; i++)
							grey[(size_t)j * tw + i] = row[((size_t)(2 * i + 1) * w / (size_t)(2 * tw)) * step + high];
					}
				CVPixelBufferUnlockBaseAddress(buf, kCVPixelBufferLock_ReadOnly);
				if(base)
					tap(grey.data(), tw, th, pictureUs);
			}
		}
		std::vector<uint8_t> &motionLast = st.last;
		double &motionLastUs = st.lastUs;
		std::vector<uint8_t> now;
		if(!smallLuma(buf, wide, now))
		{
			motionLast.clear();
			return;
		}
		PictureMotion m;
		m.previousUs = motionLastUs;
		m.pictureUs = pictureUs;
		if(motionLast.size() == now.size() && m.pictureUs - m.previousUs < 150000.0)
		{
			// A picture is not one sheet. The character the camera follows and the HUD stand
			// still; sand, sea and sky have nothing in them to tell by, or move in their own
			// way (waves; ground near an orbiting camera); what tells how far the camera
			// turned is whatever has shape and went the same way: far islands, trees, houses.
			// So the picture is asked piece by piece: each of 24 pieces finds its own best
			// shift, the pieces whose shift fits them much better than no shift are believed,
			// and the camera's turn is what most of those agree on. (One shift for the whole
			// picture, as this was, measured a beach in daylight turning at 65 % of its width
			// a second as standing still: only the strip along the horizon said otherwise.
			// 2026-11-27.)
			const int rangeX = 24, rangeY = 14, marginX = rangeX + 4, marginY = rangeY + 4;
			const int tilesX = 6, tilesY = 4, tileW = (kMotionW - 2 * marginX) / tilesX, tileH = (kMotionH - 2 * marginY) / tilesY;
			const std::vector<uint8_t> &fixedNow = st.hard;
			const bool haveFixed = fixedNow.size() == now.size();
			struct Piece { double x, y; bool upper; };
			std::vector<Piece> pieces;
			for(int ty = 0; ty < tilesY; ty++)
				for(int tx = 0; tx < tilesX; tx++)
				{
					const int x0 = marginX + tx * tileW, y0 = marginY + ty * tileH;
					// What has shape in this piece, and is not known to be fixed to the screen.
					int points = 0, rough = 0;
					for(int y = y0; y < y0 + tileH; y++)
						for(int x = x0; x < x0 + tileW; x++)
						{
							const size_t at = (size_t)y * kMotionW + x;
							if(haveFixed && fixedNow[at] >= 128)
								continue;
							points++;
							rough += std::abs((int)motionLast[at + 1] - (int)motionLast[at - 1]) >= 5 ? 1 : 0;
						}
					if(points < tileW * tileH / 2 || rough < 10)
						continue;
					auto cost = [&](int dx, int dy) {
						uint64_t c = 0;
						for(int y = y0; y < y0 + tileH; y++)
						{
							const uint8_t *pa = motionLast.data() + (size_t)y * kMotionW, *pb = now.data() + (size_t)(y + dy) * kMotionW + dx;
							const uint8_t *pf = haveFixed ? fixedNow.data() + (size_t)y * kMotionW : nullptr;
							for(int x = x0; x < x0 + tileW; x++)
								if(!pf || pf[x] < 128)
									c += (uint64_t)std::abs((int)pa[x] - (int)pb[x]);
						}
						return (double)c / points;
					};
					int bx = 0, by = 0;
					double best = 1e30;
					for(int dy = -rangeY; dy <= rangeY; dy += 2)
						for(int dx = -rangeX; dx <= rangeX; dx += 2)
						{
							const double c = cost(dx, dy);
							if(c < best)
							{
								best = c;
								bx = dx;
								by = dy;
							}
						}
					int fx = bx, fy = by;
					for(int dy = -1; dy <= 1; dy++)
						for(int dx = -1; dx <= 1; dx++)
						{
							const int x = std::max(-rangeX, std::min(rangeX, bx + dx)), y = std::max(-rangeY, std::min(rangeY, by + dy));
							const double c = cost(x, y);
							if(c < best)
							{
								best = c;
								fx = x;
								fy = y;
							}
						}
					// Believed only if it is a place of its own: two points to either side the
					// piece must fit clearly worse. Sand and open water fit as well, or as badly,
					// wherever they are put, and say nothing; and a piece that fits best unmoved
					// is as good a witness as one that fits best moved (a slow turn, under a
					// point of this map a picture, is told by them: counted as "standing still"
					// they had every turn slower than 8 % of the width a second measured as
					// none, and the table learned that the stick does nothing there. 2026-11-27).
					if(std::abs(fx) >= rangeX || std::abs(fy) >= rangeY || best > 8.0)
						continue;
					const double beside = std::min(cost(std::max(-rangeX, fx - 2), fy), cost(std::min(rangeX, fx + 2), fy));
					if(beside < best * 1.4 + 0.3)
						continue;
					// Between the points: the lowest place of a parabola through the best cost
					// and the two beside it, each way.
					auto between = [&](int ax, int ay) {
						const double lo = cost(fx - ax, fy - ay), hi = cost(fx + ax, fy + ay), bend = lo - 2 * best + hi;
						return bend > 1e-6 ? std::max(-0.5, std::min(0.5, 0.5 * (lo - hi) / bend)) : 0.0;
					};
					pieces.push_back({ fx + between(1, 0), fy + between(0, 1), ty < tilesY / 2 });
				}
			double shiftX = 0, shiftY = 0;
			// While the player is turning the camera, a piece that moved is believed before
			// pieces that did not: under an open sky the only things with shape are the ones
			// fixed to the screen (the character, a crosshair, an ammunition count), and
			// until those have been learned as fixed they outvote the one cloud that shows
			// the turn (2026-11-27: a turn measured as none, looking up). If nothing moved,
			// the camera is at the end of its way, and that is what is told.
			const double turnedSince = st.turnSinceUs.load();
			if(turnedSince > 0 && pictureUs - turnedSince > 250000.0)
			{
				std::vector<Piece> moving;
				for(const Piece &p : pieces)
					if(std::hypot(p.x, p.y) >= 0.3)
						moving.push_back(p);
				if(!moving.empty() && moving.size() < pieces.size())
				{
					if(moving.size() == 1)
						moving.push_back(moving[0]);
					pieces.swap(moving);
				}
				else if(moving.empty())
				{
					// Nothing that could be followed moved. If the picture as a whole changed
					// all the same, the camera is turning too fast to follow (it blurs) and
					// only what is fixed to the screen was left to match: that is "cannot
					// say", not "stands still". (Taken for standing still, a fast turn made
					// the warp believe the stick had stopped moving the camera.)
					uint64_t changed = 0, counted = 0;
					for(int y = marginY; y < kMotionH - marginY; y += 2)
						for(int x = marginX; x < kMotionW - marginX; x += 2)
						{
							const size_t at = (size_t)y * kMotionW + x;
							if(haveFixed && fixedNow[at] >= 128)
								continue;
							changed += (uint64_t)std::abs((int)now[at] - (int)motionLast[at]);
							counted++;
						}
					if(counted && (double)changed / counted > 2.5)
						pieces.clear();
				}
			}
			// What is far away tells the camera's turn; what is near moves by more or less than
			// that when the camera goes round the character (bushes at his feet, a car beside
			// him: with them voting, a third of the pictures of a turn had no shift that most
			// agreed on, and the rest were told now by the far and now by the near). Far is,
			// nearly always, the upper half of the picture: if three pieces there have
			// something to say, they are the ones asked.
			bool turnsRound = false;
			{
				std::vector<Piece> upper, lower;
				for(const Piece &p : pieces)
					(p.upper ? upper : lower).push_back(p);
				// Unless the lower half goes the other way: then the camera looks down on the
				// scene and a turn takes the picture round its middle. No one shift is that,
				// and none is told (told the upper half's, the whole picture was pushed
				// sideways while its lower half went the other way).
				if(upper.size() >= 3 && lower.size() >= 3)
				{
					auto middle = [](std::vector<Piece> v) {
						std::sort(v.begin(), v.end(), [](const Piece &a, const Piece &b) { return a.x < b.x; });
						return v[v.size() / 2].x;
					};
					const double up = middle(upper), down = middle(lower);
					turnsRound = std::abs(up) >= 0.5 && up * down < 0 && std::abs(down) >= 0.3 * std::abs(up);
				}
				if(upper.size() >= 3)
					pieces.swap(upper);
			}
			if(turnsRound)
				pieces.clear();
			if(pieces.size() >= 2)
			{
				// What most agree on: the middle one each way, and how many are near it.
				std::vector<double> xs, ys;
				for(const Piece &p : pieces)
				{
					xs.push_back(p.x);
					ys.push_back(p.y);
				}
				std::sort(xs.begin(), xs.end());
				std::sort(ys.begin(), ys.end());
				const double mx = xs[xs.size() / 2], my = ys[ys.size() / 2];
				const double within = 0.6 + 0.08 * std::max(std::abs(mx), std::abs(my));
				double sumX = 0, sumY = 0;
				int near = 0;
				for(const Piece &p : pieces)
					if(std::abs(p.x - mx) <= within && std::abs(p.y - my) <= within)
					{
						sumX += p.x;
						sumY += p.y;
						near++;
					}
				m.valid = near >= 2 && near * 2 >= (int)pieces.size();
				if(m.valid)
				{
					shiftX = sumX / near;
					shiftY = sumY / near;
				}
			}
			else
				m.valid = false;
			const int fineX = (int)std::lround(shiftX), fineY = (int)std::lround(shiftY);
			m.dx = (float)(shiftX / kMotionW);
			m.dy = (float)(shiftY / kMotionH);
			// What does not slide with the rest, from a pair in which the rest clearly did.
			std::vector<uint8_t> centre;
			const bool haveCentre = centreLuma(buf, wide, centre);
			// What does not slide with the rest, from a pair in which the rest clearly did.
			if(m.valid)
			{
				std::vector<uint8_t> mask, centreMask;
				if(std::abs(fineX) + std::abs(fineY) >= 2)
					learnFixed(motionLast, now, kMotionW, kMotionH, fineX, fineY, st.stay, st.slide, 2.0f, true, mask, st.hard);
				// The middle point for point: kCentreW of them are a 7.5th of the width.
				const int px = (int)std::lround(m.dx * kCentreW * 7.5), py = (int)std::lround(m.dy * kCentreH * 7.5);
				if(haveCentre && st.centreLast.size() == centre.size() && std::abs(px) + std::abs(py) >= 4)
				{
					std::vector<uint8_t> unused;
					learnFixed(st.centreLast, centre, kCentreW, kCentreH, px, py, st.centreStay, st.centreSlide, 3.5f, false, centreMask, unused);
				}
				if(!mask.empty() || !centreMask.empty())
				{
					std::lock_guard<std::mutex> lk(st.m);
					if(!mask.empty())
						st.mask.swap(mask);
					if(!centreMask.empty())
						st.centreMask.swap(centreMask);
					st.maskNew = true;
				}
			}
			if(haveCentre)
				st.centreLast.swap(centre);
			else
				st.centreLast.clear();
		}
		else
			st.centreLast.clear();
		motionLast.swap(now);
		motionLastUs = pictureUs;
		std::function<void(const PictureMotion &)> sink;
		{
			std::lock_guard<std::mutex> lk(st.m);
			sink = st.sink;
		}
		if(m.previousUs > 0 && sink)
			sink(m);
	}

	// Whether this frame shows the picture the frame before it showed: a game that renders
	// 30 pictures a second in a 60 fps stream sends every one twice. The two differ by what
	// the encoder made of them (a mean of 0.2 eight-bit levels, measured on four clips of a
	// PS5 game, against 5 to 6 between two pictures), so the test is against what
	// a picture has changed by lately and not against zero. A still scene, where a new picture is no more
	// different than a repeat, counts as new pictures throughout: the enhancer then does
	// every frame, as without this. Luma only, at 48 x 27 points, read from the decoder's
	// own buffer.
	bool repeatsLastFrame(CVPixelBufferRef buf)
	{
		const OSType format = CVPixelBufferGetPixelFormatType(buf);
		// A compressed surface has no pixels to read here.
		if((format >> 24) == '&' || !CVPixelBufferIsPlanar(buf)
			|| CVPixelBufferLockBaseAddress(buf, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess)
		{
			lastSignature.clear();
			return false;
		}
		const uint8_t *base = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(buf, 0);
		const size_t stride = CVPixelBufferGetBytesPerRowOfPlane(buf, 0);
		const size_t w = CVPixelBufferGetWidthOfPlane(buf, 0), h = CVPixelBufferGetHeightOfPlane(buf, 0);
		const bool wide = w > 0 && stride >= 2 * w && colorInfoFor(buf).bits > 8;
		const size_t kCols = 48, kRows = 27;
		std::vector<uint16_t> signature;
		if(base && w >= kCols && h >= kRows)
		{
			signature.reserve(kCols * kRows);
			for(size_t j = 0; j < kRows; j++)
			{
				const uint8_t *row = base + ((2 * j + 1) * h / (2 * kRows)) * stride;
				for(size_t i = 0; i < kCols; i++)
				{
					const size_t x = (2 * i + 1) * w / (2 * kCols);
					// Sixteen bits either way: an 8-bit sample in the high byte.
					signature.push_back(wide ? (uint16_t)(row[2 * x] | (row[2 * x + 1] << 8)) : (uint16_t)(row[x] << 8));
				}
			}
		}
		CVPixelBufferUnlockBaseAddress(buf, kCVPixelBufferLock_ReadOnly);
		bool repeat = false;
		if(!signature.empty() && signature.size() == lastSignature.size())
		{
			double sum = 0;
			for(size_t i = 0; i < signature.size(); i++)
				sum += std::abs((int)signature[i] - (int)lastSignature[i]);
			const double change = sum / (double)signature.size() / 65535.0;
			// Against what a new picture has changed by lately: the median of the larger half
			// of the last eight changes, which one cut or one still moment does not move. A
			// repeat is under half of that (a quarter, measured), and never follows a repeat:
			// a frame after one is taken for a picture, whatever it measures.
			if(recentChanges.size() >= 8 && !lastWasRepeat)
			{
				std::vector<double> sorted(recentChanges.begin(), recentChanges.end());
				std::sort(sorted.begin(), sorted.end());
				const double typical = (sorted[5] + sorted[6]) / 2;
				repeat = change < typical * 0.5;
			}
			recentChanges.push_back(change);
			if(recentChanges.size() > 8)
				recentChanges.pop_front();
		}
		else
			recentChanges.clear();
		lastWasRepeat = repeat;
		// A repeat is compared with nothing: the next frame is measured against the picture.
		if(!repeat)
			lastSignature.swap(signature);
		return repeat;
	}

	// A decoded frame, in an immediate pacing mode: rendered now, or held for its result.
	void frameArrived(DecodedFrame &f)
	{
		double t = f.rec->decodedUs;
		if(lastArrivalUs > 0 && t > lastArrivalUs && t - lastArrivalUs < 200000.0)
			arrivalIntervalUs = arrivalIntervalUs * 0.95 + (t - lastArrivalUs) * 0.05;
		lastArrivalUs = t;
		if(generatesFor(f))
		{
			pictureArrived(f);
			return;
		}
		if(lastPicture || !due.empty() || !awaiting.empty() || !order.empty())
			endGeneration();
		bool repeat = false;
		const bool enhances = enhanceCfg.skipRepeats && enhancing(colorInfoFor(f.buf));
		if(enhances || motionSink || warpSource)
		{
			const double t0 = nowUs();
			repeat = repeatsLastFrame(f.buf);
			const double took = nowUs() - t0;
			std::lock_guard<std::mutex> sl(shared->m);
			shared->c.repeatChecks++;
			shared->c.repeatCheckUs += took;
		}
		// The camera warp: a new picture is measured, and a repeat is the picture that came
		// with the frame before it.
		if(repeat)
			f.rec->pictureUs = lastPictureArrivalUs;
		else if(motionSink || warpSource)
			measureMotion(f);
		if(repeat && enhances)
		{
			// The picture has been given to the enhancer already, as the frame before this.
			f.rec->repeat = true;
			f.rec->enhanceOffered = true;
			{
				std::lock_guard<std::mutex> sl(shared->m);
				shared->c.framesRepeated++;
			}
			if(!held.empty())
			{
				// That frame still waits for its result and will be shown with it; this one
				// would be the same picture again, right behind it.
				CVPixelBufferRelease(f.buf);
				f = DecodedFrame();
				return;
			}
			renderFrame(f, nil, 0);
			return;
		}
		if(!waitsForEnhancer(f))
		{
			flushHeld();
			renderFrame(f, nil, 0);
			return;
		}
		clearPending();
		// One frame is with the enhancer and one is next. A frame that arrives while both
		// places are taken replaces the one that is next: an enhancer slower than the stream
		// shows the frames it can do, each as fresh as it can be, and the others not at all.
		while(held.size() >= 2)
		{
			if(held.back().tried)
			{
				renderHeldFront(true);
				continue;
			}
			CVPixelBufferRelease(held.back().f.buf);
			held.pop_back();
			std::lock_guard<std::mutex> sl(shared->m);
			shared->c.enhanceSkipped++;
		}
		Held h;
		h.f = f;
		f = DecodedFrame();
		double waitUs = std::max(1.0, (double)enhanceCfg.maxWaitMs) * 1000.0;
		h.deadlineUs = nowUs() + waitUs;
		held.push_back(h);
		submitHeld();
		// The wait ends at the deadline whatever the enhancer does.
		std::shared_ptr<EnhanceState> st = enhance;
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(waitUs * 1000.0) + 300000), dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
			st->signal();
		});
	}

	// The oldest frame that has not been offered goes to the enhancer, if it is idle.
	void submitHeld()
	{
		if(enhance->busy.load())
			return;
		for(Held &h : held)
		{
			if(h.tried)
				continue;
			h.tried = true;
			h.keySeq = offerFrame(h.f);
			return;
		}
	}

	void renderHeldFront(bool late)
	{
		DecodedFrame f = held.front().f;
		held.pop_front();
		if(late)
		{
			std::lock_guard<std::mutex> sl(shared->m);
			shared->c.enhanceLate++;
		}
		renderFrame(f, nil, 0);
	}

	void flushHeld()
	{
		while(!held.empty())
			renderHeldFront(true);
	}

	// ---- frame generation ---------------------------------------------------

	bool generatesFor(const DecodedFrame &f)
	{
		if(!generating || !interpolator || activePacing == PacingMode::DisplayLink)
			return false;
		ColorInfo ci = colorInfoFor(f.buf);
		return ci.transfer == Transfer::PQ || (!ci.hdr() && ci.bits == 8);
	}

	// Leaves frame generation: what still waits is shown, and nothing is kept.
	void endGeneration()
	{
		generation++;
		showDue(true);
		// What is still on its way, in order and without what was to come before it. A
		// generated picture that arrives after this finds its stage shown and is dropped.
		for(auto &stage : order)
		{
			if(stage->shown)
				continue;
			stage->shown = true;
			renderFrame(stage->f, nil, 0, stage->picture);
		}
		order.clear();
		while(!awaiting.empty())
		{
			DecodedFrame f = takeAwaiting();
			renderFrame(f, nil, 0);
		}
		lastPicture.reset();
	}

	// The pictures whose turn has come (all of them with `all`), oldest first.
	void showDue(bool all)
	{
		while(!due.empty() && (all || nowUs() >= due.front().dueUs))
		{
			DuePicture d = due.front();
			due.pop_front();
			renderFrame(d.f, nil, 0, d.picture);
			if(d.generated)
			{
				std::lock_guard<std::mutex> sl(shared->m);
				shared->c.framesGenerated++;
			}
		}
		// The next one in its time.
		if(!due.empty() && !all)
		{
			std::shared_ptr<EnhanceState> st = enhance;
			const double waitUs = std::max(0.0, due.front().dueUs - nowUs());
			dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(waitUs * 1000.0) + 100000), dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
				st->signal();
			});
		}
	}

	// A record for a picture that no frame of the stream carried: the generated one. It
	// has the times of the frame it leads up to, so what it took is counted from there.
	static std::shared_ptr<FrameRecord> recordLike(const FrameRecord &r)
	{
		auto rec = std::make_shared<FrameRecord>();
		rec->seq = r.seq;
		rec->frameIndex = r.frameIndex;
		rec->readyUs = r.readyUs;
		rec->submitUs = r.submitUs;
		rec->decodedUs = r.decodedUs;
		rec->hdr = r.hdr;
		return rec;
	}

	// The frame as one R'G'B' picture in the stream's code values, with the enhancer's
	// newest result mixed in: what frame generation is given and what is shown of it.
	// `offer`: the enhancer is given the picture too, if it is free and has not had it.
	std::shared_ptr<RgbPicture> rgbPictureOf(const DecodedFrame &f, id<MTLCommandBuffer> __strong *commandBuffer, bool offer)
	{
		ColorInfo ci = colorInfoFor(f.buf);
		CVMetalTextureRef ty = nullptr, tc = nullptr;
		ShaderParams params;
		CGRect clean;
		if(!keyPlanes(f.buf, ci, &ty, &tc, params, clean))
			return nullptr;
		// Released when the GPU is done with them. Built in place: a copy would release twice.
		struct Planes {
			CVMetalTextureRef ty, tc;
			Planes(CVMetalTextureRef y, CVMetalTextureRef c) : ty(y), tc(c) {}
			Planes(const Planes &) = delete;
			~Planes()
			{
				CFRelease(ty);
				CFRelease(tc);
			}
		};
		auto planes = std::make_shared<Planes>(ty, tc);
		const size_t w = (size_t)clean.size.width, h = (size_t)clean.size.height;
		const OSType format = enhancerFormat(ci);
		if(!rgbPool || w != rgbPoolW || h != rgbPoolH || format != rgbPoolFormat)
		{
			if(rgbPool)
				CVPixelBufferPoolRelease(rgbPool);
			rgbPool = nullptr;
			NSDictionary *attrs = @{
				(__bridge NSString *)kCVPixelBufferPixelFormatTypeKey : @(format),
				(__bridge NSString *)kCVPixelBufferWidthKey : @(w),
				(__bridge NSString *)kCVPixelBufferHeightKey : @(h),
				(__bridge NSString *)kCVPixelBufferIOSurfacePropertiesKey : @{},
				(__bridge NSString *)kCVPixelBufferMetalCompatibilityKey : @YES,
			};
			if(CVPixelBufferPoolCreate(kCFAllocatorDefault, nullptr, (__bridge CFDictionaryRef)attrs, &rgbPool) != kCVReturnSuccess)
			{
				rgbPool = nullptr;
				return nullptr;
			}
			rgbPoolW = w;
			rgbPoolH = h;
			rgbPoolFormat = format;
			lastPicture.reset();
		}
		auto picture = std::make_shared<RgbPicture>();
		if(CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, rgbPool, &picture->buf) != kCVReturnSuccess || !picture->buf)
			return nullptr;
		picture->tex = wrapEnhancerBuffer(picture->buf, true);
		if(!picture->tex)
			return nullptr;
		MTLPixelFormat textureFormat = enhancerTextureFormat(format);
		// The enhancer's result, as a frame shown at once gets it.
		NrDraw nrDraw;
		double nrAgeMs = 0;
		std::shared_ptr<EnhancedFrame> nr;
		id<MTLRenderPipelineState> mix = nil;
		if(enhancing(ci))
		{
			nr = enhancedFor(f.rec->decodedUs, ci, nrDraw, &nrAgeMs);
			mix = nr ? pipelineIfBuilt(FragBilinearNr, textureFormat) : nil;
		}
		id<MTLCommandBuffer> cb = [queue commandBuffer];
		if(mix)
			useGrid(cb, nr, nrDraw);
		drawPass(cb, CVMetalTextureGetTexture(picture->tex), MTLRegionMake2D(0, 0, w, h),
				mix ?: pipelineFor(FragBilinear, textureFormat), CVMetalTextureGetTexture(ty), CVMetalTextureGetTexture(tc),
				&params, mix ? &nrDraw : nullptr);
		if(mix)
		{
			f.rec->enhanceAgeMs = nrAgeMs;
			std::lock_guard<std::mutex> sl(shared->m);
			shared->c.framesEnhanced++;
			if(nrAgeMs <= 0)
				shared->c.framesEnhancedExact++;
			shared->enhanceAge.add(nrAgeMs);
		}
		CVPixelBufferRef keepBuf = CVPixelBufferRetain(f.buf);
		[cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
			(void)planes;
			(void)nr;
			CVPixelBufferRelease(keepBuf);
		}];
		if(offer && enhancing(ci) && !f.rec->enhanceOffered)
			offerToEnhancer(f.rec, f.buf, ty, tc, params, clean, ci);
		*commandBuffer = cb;
		return picture;
	}

	// A decoded frame while frame generation is on.
	void pictureArrived(DecodedFrame &f)
	{
		showDue(false);
		{
			const double t0 = nowUs();
			const bool repeat = repeatsLastFrame(f.buf);
			const double took = nowUs() - t0;
			std::lock_guard<std::mutex> sl(shared->m);
			shared->c.repeatChecks++;
			shared->c.repeatCheckUs += took;
			if(repeat)
				shared->c.framesRepeated++;
			if(repeat)
			{
				// Its picture is on its way to the screen already; in its place comes the
				// picture between this one and the next.
				CVPixelBufferRelease(f.buf);
				f = DecodedFrame();
				return;
			}
		}
		if(motionSink || warpSource)
			measureMotion(f);
		// EnhanceConfig::wait: the picture first gets the enhancer's result of itself, so
		// that what is generated lies between two enhanced pictures and nothing in them is
		// a change made to another picture. Only while the enhancer is free when a picture
		// comes (one that is slower than the game's pictures works beside them, as without).
		while(!awaiting.empty())
			pictureReady(takeAwaiting());
		if(lastNewPictureUs > 0 && f.rec->decodedUs > lastNewPictureUs && f.rec->decodedUs - lastNewPictureUs < 200000.0)
			pictureIntervalUs = pictureIntervalUs * 0.9 + (f.rec->decodedUs - lastNewPictureUs) * 0.1;
		lastNewPictureUs = f.rec->decodedUs;
		// And only while a result takes less than the game takes for a picture: one that takes
		// longer would hold every picture back for a result that the next picture cuts short.
		const double takes = enhance->roundTripMs.load() * 1000.0;
		if(enhanceCfg.wait && enhanceCfg.strength > 0 && enhancing(colorInfoFor(f.buf)) && !enhance->busy.load()
			&& nowUs() >= enhance->holdUntilUs.load() && (takes <= 0 || takes < pictureIntervalUs * 0.9))
		{
			const uint64_t offered = offerFrame(f);
			if(offered)
			{
				Held h;
				h.f = f;
				f = DecodedFrame();
				h.tried = true;
				h.keySeq = offered;
				const double waitUs = std::max(1.0, (double)enhanceCfg.maxWaitMs) * 1000.0;
				h.deadlineUs = nowUs() + waitUs;
				awaiting.push_back(h);
				std::shared_ptr<EnhanceState> st = enhance;
				dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(waitUs * 1000.0) + 300000), dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
					st->signal();
				});
				return;
			}
		}
		pictureReady(f);
	}

	DecodedFrame takeAwaiting()
	{
		DecodedFrame f = awaiting.front().f;
		awaiting.pop_front();
		return f;
	}

	// The pictures that waited for the enhancer and have its result, or need wait no longer.
	void serveAwaiting()
	{
		uint64_t latestSeq = 0;
		{
			std::lock_guard<std::mutex> lk(enhance->m);
			if(enhance->latest)
				latestSeq = enhance->latest->key->seq;
		}
		const bool idle = !enhance->busy.load();
		while(!awaiting.empty())
		{
			const Held &h = awaiting.front();
			if(h.keySeq > latestSeq && !idle && nowUs() < h.deadlineUs)
				break;
			pictureReady(takeAwaiting());
		}
	}

	// A new picture of the stream, with whatever the enhancer is to add to it at hand: it
	// becomes one R'G'B' picture, and the picture between it and the last one is asked for.
	void pictureReady(DecodedFrame f)
	{
		// The generation is small and what the screen waits for; the enhancer's work is long
		// and shares the GPU with it. When a picture is to be generated, the enhancer gets
		// this one after that is made (pictureGenerated()), not before.
		const bool generates = lastPicture && !generationBusy && f.rec->decodedUs - lastPictureUs <= 150000.0;
		id<MTLCommandBuffer> cb = nil;
		std::shared_ptr<RgbPicture> picture = rgbPictureOf(f, &cb, !generates);
		if(!picture)
		{
			endGeneration();
			renderFrame(f, nil, 0);
			return;
		}
		std::shared_ptr<RgbPicture> previous = lastPicture;
		// Across a pause the two pictures have nothing between them.
		if(previous && f.rec->decodedUs - lastPictureUs > 150000.0)
			previous.reset();
		lastPicture = picture;
		lastPictureUs = f.rec->decodedUs;
		// The pictures reach the screen in the order they came in, whatever is generated
		// between them and however long that takes: each has a place in `order`.
		auto stage = std::make_shared<Stage>();
		stage->picture = picture;
		stage->f = f;
		order.push_back(stage);
		// One pair at a time. A picture that comes while the last pair is still in the works
		// (the GPU is behind) gets nothing generated before it.
		if(!previous || generationBusy)
		{
			[cb commit];
			lastRgbCommandBuffer = cb;
			stage->done = true;
			serveOrder();
			return;
		}
		generationBusy = true;
		const uint64_t seq = ++rgbSeq;
		std::shared_ptr<FrameInterpolator> target = interpolator;
		std::shared_ptr<EnhanceState> st = enhance;
		std::shared_ptr<Shared> sh = shared;
		Impl *self = this;
		// Posts to the render thread through the wake the enhancer's results use: it is
		// cleared before this object goes, and what was posted is then dropped with it.
		auto arrived = [self, st, stage](std::vector<std::shared_ptr<RgbPicture>> made) {
			std::lock_guard<std::mutex> lk(st->m);
			if(st->wake)
				self->post([self, stage, made] { self->pictureGenerated(stage, made); });
		};
		const int wanted = generating;
		// The generated frames of this pair, as they come in; given on with the last.
		auto collected = std::make_shared<std::vector<std::shared_ptr<RgbPicture>>>();
		// After the picture is drawn: the interpolator reads what this command buffer wrote.
		[cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
			const double handedUs = nowUs();
			bool taken = c.status == MTLCommandBufferStatusCompleted
				&& target->interpolate(previous->buf, picture->buf, wanted, seq,
						[sh, arrived, handedUs, collected](CVPixelBufferRef output, int index, int count, double) {
					if(!output || !CVPixelBufferGetIOSurface(output) || count <= 0)
					{
						{
							std::lock_guard<std::mutex> sl(sh->m);
							sh->c.generateFailed++;
						}
						arrived({});
						return;
					}
					auto made = std::make_shared<RgbPicture>();
					made->buf = CVPixelBufferRetain(output);
					collected->push_back(made);
					if(index + 1 < count)
						return;
					{
						std::lock_guard<std::mutex> sl(sh->m);
						sh->c.generateMsTotal += (nowUs() - handedUs) / 1000.0;
						sh->c.generatePairs++;
					}
					arrived(*collected);
				});
			if(!taken)
			{
				uint64_t failed;
				{
					std::lock_guard<std::mutex> sl(sh->m);
					failed = ++sh->c.generateFailed;
				}
				// The first and then every three hundredth: one that refuses does so every time.
				if(failed % 300 == 1)
					NSLog(@"plk-video: frame generation did not take a pair (%s %zux%zu, command buffer status %lu%s%s)",
							fourcc(CVPixelBufferGetPixelFormatType(picture->buf)).c_str(), CVPixelBufferGetWidth(picture->buf),
							CVPixelBufferGetHeight(picture->buf), (unsigned long)c.status, c.error ? ": " : "",
							c.error ? c.error.localizedDescription.UTF8String : "");
				arrived({});
			}
		}];
		[cb commit];
		lastRgbCommandBuffer = cb;
	}

	// The picture before `stage`'s is made, or was not (render thread).
	void pictureGenerated(std::shared_ptr<Stage> stage, std::vector<std::shared_ptr<RgbPicture>> made)
	{
		generationBusy = false;
		if(!stage->shown && stage->f.buf && !stage->f.rec->enhanceOffered && enhancing(colorInfoFor(stage->f.buf)))
			offerFrame(stage->f);
		bool usable = !made.empty();
		for(auto &m : made)
		{
			m->tex = wrapEnhancerBuffer(m->buf, false);
			usable = usable && m->tex;
		}
		if(usable)
			stage->made = made;
		stage->done = true;
		serveOrder();
	}

	// Gives the pictures that are ready, from the oldest on, their times on screen: a
	// generated one as soon as may be, the picture it leads up to one frame of the stream
	// after it, and never one before the one that came before it.
	void serveOrder()
	{
		while(!order.empty() && order.front()->done)
		{
			std::shared_ptr<Stage> stage = order.front();
			order.pop_front();
			if(stage->shown)
				continue;
			stage->shown = true;
			// The time from one picture of the game to the next, shared evenly among the
			// generated ones and the picture itself; never under the panel's own shortest.
			const double step = std::min(40000.0, std::max(8333.0, pictureIntervalUs / (double)(stage->made.size() + 1)));
			const double now = nowUs();
			// Not closer to the last one than the panel's shortest refresh (two pictures
			// inside one refresh are one picture), nor than half a step, so that a picture
			// that is late does not flash by; when all is in time this never holds one back.
			double at = std::max(now, lastDueUs + std::max(8333.0, step * 0.5));
			if(!offscreenMode())
			{
				// On screen the display keeps the times: the first generated picture at once
				// (never before one that is still to come), and the others and the picture
				// they lead up to rendered right behind it, each with its time on screen one
				// step after the one before.
				for(size_t i = 0; i < stage->made.size(); i++)
				{
					DecodedFrame between;
					between.buf = CVPixelBufferRetain(stage->f.buf);
					between.rec = recordLike(*stage->f.rec);
					// Its place between the two pictures, as a time of arrival.
					between.rec->pictureUs = stage->f.rec->decodedUs
							- pictureIntervalUs * (double)(stage->made.size() - i) / (double)(stage->made.size() + 1);
					renderFrame(between, nil, 0, stage->made[i], at > now + 1000.0 ? at : 0);
					{
						std::lock_guard<std::mutex> sl(shared->m);
						shared->c.framesGenerated++;
					}
					// From when it can be on screen at the earliest: the GPU has it in a few ms.
					at = std::max(at, now + 3000.0) + step;
				}
				renderFrame(stage->f, nil, 0, stage->picture, at > now + 1000.0 ? at : 0);
				lastDueUs = std::max(at, now);
				continue;
			}
			for(size_t i = 0; i < stage->made.size(); i++)
			{
				DuePicture between;
				between.picture = stage->made[i];
				between.f.buf = CVPixelBufferRetain(stage->f.buf);
				between.f.rec = recordLike(*stage->f.rec);
				between.dueUs = at;
				between.generated = true;
				due.push_back(between);
				at += step;
			}
			DuePicture real;
			real.picture = stage->picture;
			real.f = stage->f;
			real.dueUs = at;
			due.push_back(real);
			lastDueUs = at;
		}
		showDue(false);
		if(!due.empty())
		{
			std::shared_ptr<EnhanceState> st = enhance;
			const double waitUs = std::max(0.0, due.front().dueUs - nowUs());
			dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(waitUs * 1000.0) + 100000), dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
				st->signal();
			});
		}
	}

	// The enhancer became idle, or a held frame's wait may be over (render thread).
	void onEnhancerEvent()
	{
		showDue(false);
		serveAwaiting();
		uint64_t latestSeq = 0;
		{
			std::lock_guard<std::mutex> lk(enhance->m);
			if(enhance->latest)
				latestSeq = enhance->latest->key->seq;
		}
		bool idle = !enhance->busy.load();
		while(!held.empty())
		{
			const Held &h = held.front();
			// Its own result is in; or it has none to wait for (it could not be offered, or
			// the enhancer came back without it); or it has waited long enough.
			bool own = h.tried && h.keySeq != 0 && h.keySeq <= latestSeq;
			bool none = h.tried && (h.keySeq == 0 || (idle && h.keySeq > latestSeq));
			bool late = nowUs() >= h.deadlineUs;
			if(!own && !none && !late)
				break;
			renderHeldFront(!own);
		}
		submitHeld();
		if(held.empty() && nrPending.buf && !enhance->busy.load())
		{
			// Fresh enough to be worth the enhancer's time: the next frame is not here yet.
			if(nowUs() - nrPending.rec->decodedUs < arrivalIntervalUs && enhancing(colorInfoFor(nrPending.buf)))
				offerFrame(nrPending);
			clearPending();
		}
	}

	// `picture`: the frame's picture as R'G'B' code values in one texture (frame generation),
	// shown in place of the frame's own planes; whatever an enhancer does is in it already.
	// `presentAtUs`: when the frame is to be on screen, on nowUs()'s clock; 0 for at once.
	// The display itself keeps that time (it has a 4.17 ms grid of its own on a 120 Hz
	// panel), which no timer of ours could.
	void renderFrame(DecodedFrame &f, id<CAMetalDrawable> drawable, double targetPresentUs,
			std::shared_ptr<RgbPicture> picture = nullptr, double presentAtUs = 0)
	{
		auto rec = f.rec;
		rec->renderStartUs = nowUs();
		CVPixelBufferRef buf = f.buf;
		ColorInfo ci = colorInfoFor(buf);
		MTLPixelFormat outFmt = outputFormatFor(ci, cfg.hdrOutput);
		bool linear = linearOutput(ci, cfg.hdrOutput);
		bool off = offscreenMode();

		if(!off)
		{
			if(!layerConfigured || !ci.sameOutput(layerColor) || outFmt != layer.pixelFormat)
				configureLayer(ci, rec->hdr);
			else if(ci.hdr() && (rec->hdr != layerHdr || layerBrightness != cfg.hdrBrightness))
			{
				[CATransaction begin];
				[CATransaction setDisableActions:YES];
				layer.EDRMetadata = edrMetadataFor(ci, rec->hdr);
				[CATransaction commit];
				layerHdr = rec->hdr;
				layerBrightness = cfg.hdrBrightness;
			}
			if(drawable && drawable.texture.pixelFormat != outFmt)
			{
				// Display link handed out a drawable in the previous format;
				// skip this refresh, the next drawable has the new one.
				putBackIfEmpty(f);
				return;
			}
		}
		{
			std::lock_guard<std::mutex> sl(shared->m);
			shared->c.colorDescription = ci.describe();
			shared->c.hdr = ci.hdr();
		}

		// Zero-copy plane textures.
		bool ten = ci.bits > 8;
		size_t w0 = CVPixelBufferGetWidthOfPlane(buf, 0), h0 = CVPixelBufferGetHeightOfPlane(buf, 0);
		size_t w1 = CVPixelBufferGetWidthOfPlane(buf, 1), h1 = CVPixelBufferGetHeightOfPlane(buf, 1);
		CVMetalTextureRef ty = nullptr, tc = nullptr;
		CVReturn r0 = CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, texCache, buf, nullptr,
				ten ? MTLPixelFormatR16Unorm : MTLPixelFormatR8Unorm, w0, h0, 0, &ty);
		CVReturn r1 = CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, texCache, buf, nullptr,
				ten ? MTLPixelFormatRG16Unorm : MTLPixelFormatRG8Unorm, w1, h1, 1, &tc);
		if(r0 != kCVReturnSuccess || r1 != kCVReturnSuccess || !ty || !tc)
		{
			NSLog(@"plk-video: CVMetalTextureCache failed for %s (%d/%d)", fourcc(ci.pixelFormat).c_str(), r0, r1);
			if(ty) CFRelease(ty);
			if(tc) CFRelease(tc);
			CVPixelBufferRelease(buf);
			f.buf = nullptr;
			if(ci.pixelFormat >> 24 == '&')
				compressedUnsupported.store(true);
			return;
		}
		id<MTLTexture> texY = CVMetalTextureGetTexture(ty);
		id<MTLTexture> texC = CVMetalTextureGetTexture(tc);

		CGRect clean = CVImageBufferGetCleanRect(buf);
		if(clean.size.width <= 0 || clean.size.height <= 0)
			clean = CGRectMake(0, 0, w0, h0);
		ShaderParams params = {};
		colorRows(ci, params);
		params.lumaSize = simd_make_float2((float)w0, (float)h0);
		params.chromaSize = simd_make_float2((float)w1, (float)h1);
		params.uvScale = simd_make_float2((float)(clean.size.width / w0), (float)(clean.size.height / h0));
		params.uvOffset = simd_make_float2((float)(clean.origin.x / w0), (float)(clean.origin.y / h0));
		// Left-sited chroma sits 1/4 chroma texel left of where a centred
		// sampler assumes it; shift the lookup right by that much.
		params.chromaShift = simd_make_float2(ci.chromaLeft ? 0.25f : 0.0f, ci.chromaTop ? 0.25f : 0.0f);
		// The brightness is a factor on linear light.
		const float gain = cfg.hdrBrightness;
		params.linearize = simd_make_float2(0.0f, 0.0f);
		params.toneMap = simd_make_float2(0.0f, 0.0f);
		if(ci.transfer == Transfer::PQ && cfg.hdrToneMap == HdrToneMap::App)
		{
			const AppToneMap tm = appToneMap(gain, masteringPeakNits(rec->hdr), displayHeadroom.load());
			if(linear)
			{
				// Extended linear without metadata: 1.0 is SDR white.
				params.linearize = simd_make_float2(1.0f, 10000.0f * tm.nitsToEdr);
				params.toneMap = simd_make_float2(tm.knee, tm.headroom);
			}
			else
			{
				// A PQ layer without metadata: macOS shows 100 nits as SDR white (measured).
				// In the shader's linear light 1.0 is 10000 nits, which is 100 of those.
				params.linearize = simd_make_float2(2.0f, 100.0f * tm.nitsToEdr);
				params.toneMap = simd_make_float2(tm.knee / 100.0f, tm.headroom / 100.0f);
			}
		}
		else if(linear)
			// ExtendedLinear with metadata: 1.0 = 100 nits, matching opticalOutputScale 100.
			params.linearize = simd_make_float2(1.0f, 100.0f * gain);
		else if(ci.transfer == Transfer::PQ && brightnessScaled(gain))
			// At 100 % a PQ drawable gets the decoder's code values untouched.
			params.linearize = simd_make_float2(2.0f, gain);
		params.sdrMap = simd_make_float2(ci.transfer == Transfer::PQ ? 10000.0f / kEnhancerSdrWhiteNits : 0.0f, 0.0f);
		if(warpSource)
		{
			// For when this frame is on screen: its time if it has one, else a refresh from now.
			float wx = 0, wy = 0;
			warpSource(rec->pictureUs > 0 ? rec->pictureUs : rec->decodedUs, presentAtUs > 0 ? presentAtUs : nowUs() + shared->renderToGlassUs.load(), &wx, &wy);
			params.warp = simd_make_float2(wx, wy);
		}

		// Target.
		id<MTLTexture> target;
		if(off)
		{
			if(!offscreen || offscreen.width != (NSUInteger)cfg.offscreenWidth || offscreen.height != (NSUInteger)cfg.offscreenHeight
				|| offscreen.pixelFormat != outFmt)
				offscreen = makeTexture(outFmt, cfg.offscreenWidth, cfg.offscreenHeight,
						MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
			target = offscreen;
		}
		else
		{
			if(!drawable)
				drawable = [layer nextDrawable];
			if(!drawable)
			{
				CFRelease(ty);
				CFRelease(tc);
				CVPixelBufferRelease(buf);
				f.buf = nullptr;
				std::lock_guard<std::mutex> sl(shared->m);
				shared->c.framesNotPresented++;
				return;
			}
			target = drawable.texture;
		}

		MTLRegion dest = pictureRegion(clean.size.width, clean.size.height, target.width, target.height);
		OverlayDraw overlayDraw;
		const OverlayDraw *overlay = overlayFor(target, dest, ci, linear, overlayDraw) ? &overlayDraw : nullptr;
		Scaler eff = scaler;
		if(eff == Scaler::MetalFX && (dest.size.width < (NSUInteger)clean.size.width || dest.size.height < (NSUInteger)clean.size.height))
			eff = Scaler::Bilinear;  // MetalFX spatial upscales only
		if(picture)
			eff = Scaler::Bilinear;  // one texture, one tap: every picture of the sequence alike

		// The enhancer's newest result, if this frame can carry it (see offerToEnhancer()
		// for where the results come from).
		bool enhanceOn = !picture && enhancing(ci);
		NrDraw nrDraw;
		double nrAgeMs = 0;
		std::shared_ptr<EnhancedFrame> nr;
		if(enhanceOn)
			nr = enhancedFor(rec->decodedUs, ci, nrDraw, &nrAgeMs);
		// A repeat carries the result of the picture it repeats: of its own picture.
		if(nr && rec->repeat && nrAgeMs <= arrivalIntervalUs / 1000.0 * 1.6)
			nrAgeMs = 0;
		bool mixed = false;

		id<MTLCommandBuffer> cb = [queue commandBuffer];
		if(nr)
			useGrid(cb, nr, nrDraw);
		bool encoded = false;
		if(eff == Scaler::MetalFX)
		{
			id<MTLRenderPipelineState> mix = nr ? pipelineIfBuilt(FragBilinearNr, MTLPixelFormatRGBA16Float) : nil;
			encoded = encodeMetalFX(cb, target, dest, texY, texC, params, (NSUInteger)clean.size.width,
					(NSUInteger)clean.size.height, linear, mix, mix ? &nrDraw : nullptr, overlay);
			if(!encoded)
				eff = Scaler::Bilinear;
			else
				mixed = mix != nil;
		}
		if(!encoded)
		{
			Frag frag = eff == Scaler::Bicubic ? FragBicubic : eff == Scaler::Lanczos ? FragLanczos : FragBilinear;
			id<MTLRenderPipelineState> mix = nr ? pipelineIfBuilt(mixedIn(frag), target.pixelFormat) : nil;
			if(picture)
				drawPass(cb, target, dest, pipelineFor(FragRgb, target.pixelFormat), CVMetalTextureGetTexture(picture->tex), texC,
						&params, nullptr, overlay);
			else if(mix)
				drawPass(cb, target, dest, mix, texY, texC, &params, &nrDraw, overlay);
			else
				drawPass(cb, target, dest, pipelineFor(frag, target.pixelFormat), texY, texC, &params, nullptr, overlay);
			mixed = mix != nil;
		}
		if(!mixed)
			nr.reset();
		else
			rec->enhanceAgeMs = nrAgeMs;

		std::shared_ptr<Shared> sh = shared;
		CVPixelBufferRef keepBuf = buf;  // ownership moves to the completion handler
		f.buf = nullptr;
		rec->presentExpected = !off;
		rec->pending.store(off ? 1 : 2);
		if(enhanceOn)
		{
			// For offerToEnhancer() below: the handler may release these first.
			CFRetain(ty);
			CFRetain(tc);
			CVPixelBufferRetain(buf);
		}
		[cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
			rec->gpuStartUs = c.GPUStartTime * 1e6;
			rec->gpuEndUs = c.GPUEndTime * 1e6;
			CFRelease(ty);
			CFRelease(tc);
			CVPixelBufferRelease(keepBuf);
			(void)nr;  // the result's textures live until the GPU is done with them
			(void)picture;
			if(rec->pending.fetch_sub(1) == 1)
				sh->finish(*rec);
		}];
		if(!off)
		{
			[drawable addPresentedHandler:^(id<MTLDrawable> d) {
				rec->presentedUs = d.presentedTime * 1e6;
				const double took = rec->presentedUs - rec->renderStartUs;
				if(took > 0 && took < 100000.0)
					sh->renderToGlassUs.store(sh->renderToGlassUs.load() * 0.9 + took * 0.1);
				if(rec->pending.fetch_sub(1) == 1)
					sh->finish(*rec);
			}];
			if(presentAtUs > 0)
				[cb presentDrawable:drawable atTime:presentAtUs / 1e6];
			else
				[cb presentDrawable:drawable];
		}
		rec->committedUs = nowUs();
		{
			std::lock_guard<std::mutex> sl(shared->m);
			shared->c.framesRendered++;
			shared->queue.add((rec->renderStartUs - rec->decodedUs) / 1000.0);
			shared->renderCpu.add((rec->committedUs - rec->renderStartUs) / 1000.0);
			shared->c.scaler = toString(eff);
			shared->c.targetWidth = (int)target.width;
			shared->c.targetHeight = (int)target.height;
			shared->c.pictureWidth = (int)dest.size.width;
			shared->c.pictureHeight = (int)dest.size.height;
			if(mixed)
			{
				shared->c.framesEnhanced++;
				if(nrAgeMs <= 0)
					shared->c.framesEnhancedExact++;
				shared->enhanceAge.add(nrAgeMs);
			}
		}
		if(off && cfg.captureCount > 0 && !cfg.captureDir.empty())
			captureFrame(cb, target, *rec, mixed ? nr.get() : nullptr, nrAgeMs);
		[cb commit];
		lastCommandBuffer = cb;
		lastTarget = target;
		(void)targetPresentUs;
		if(enhanceOn)
		{
			// After the frame's own command buffer: on the GPU the copy comes after the
			// presented picture. A frame that was held has been offered already.
			if(!rec->enhanceOffered && !offerToEnhancer(rec, buf, ty, tc, params, clean, ci) && enhance->busy.load())
			{
				clearPending();
				nrPending.buf = CVPixelBufferRetain(buf);
				nrPending.rec = rec;
			}
			CFRelease(ty);
			CFRelease(tc);
			CVPixelBufferRelease(buf);
		}
		if(++framesSinceFlush >= 120)
		{
			framesSinceFlush = 0;
			CVMetalTextureCacheFlush(texCache, 0);
		}
	}

	// `mix` and `nr`: the enhancer's result goes into the first pass, at the stream's size,
	// so that the scaler works on the enhanced picture.
	bool encodeMetalFX(id<MTLCommandBuffer> cb, id<MTLTexture> target, MTLRegion dest, id<MTLTexture> texY,
			id<MTLTexture> texC, const ShaderParams &params, NSUInteger srcW, NSUInteger srcH, bool linear,
			id<MTLRenderPipelineState> mix, const NrDraw *nr, const OverlayDraw *overlay)
	{
		NSUInteger dw = dest.size.width, dh = dest.size.height;
		MTLFXSpatialScalerColorProcessingMode mode =
			linear ? MTLFXSpatialScalerColorProcessingModeHDR : MTLFXSpatialScalerColorProcessingModePerceptual;
		if(!fx || fx.inputWidth != srcW || fx.inputHeight != srcH || fx.outputWidth != dw || fx.outputHeight != dh
			|| fx.colorProcessingMode != mode)
		{
			fx = nil;
			fxIn = nil;
			fxOut = nil;
			if(![MTLFXSpatialScalerDescriptor supportsDevice:device])
				return false;
			MTLFXSpatialScalerDescriptor *d = [MTLFXSpatialScalerDescriptor new];
			d.inputWidth = srcW;
			d.inputHeight = srcH;
			d.outputWidth = dw;
			d.outputHeight = dh;
			d.colorTextureFormat = MTLPixelFormatRGBA16Float;
			d.outputTextureFormat = MTLPixelFormatRGBA16Float;
			// Non-linear (BT.709 gamma or PQ) code values, or linear HDR light.
			d.colorProcessingMode = mode;
			fx = [d newSpatialScalerWithDevice:device];
			if(!fx)
				return false;
			fxIn = makeTexture(MTLPixelFormatRGBA16Float, srcW, srcH, fx.colorTextureUsage | MTLTextureUsageRenderTarget);
			fxOut = makeTexture(MTLPixelFormatRGBA16Float, dw, dh, fx.outputTextureUsage | MTLTextureUsageShaderRead);
		}
		drawPass(cb, fxIn, MTLRegionMake2D(0, 0, srcW, srcH),
				mix ? mix : pipelineFor(FragBilinear, MTLPixelFormatRGBA16Float), texY, texC, &params, mix ? nr : nullptr);
		fx.colorTexture = fxIn;
		fx.outputTexture = fxOut;
		fx.inputContentWidth = srcW;
		fx.inputContentHeight = srcH;
		[fx encodeToCommandBuffer:cb];
		drawPass(cb, target, dest, pipelineFor(FragCopy, target.pixelFormat), fxOut, nil, nullptr, nullptr, overlay);
		return true;
	}

	// PipelineConfig::captureDir: a copy of the frame just drawn into `target`, made by `cb`.
	void captureFrame(id<MTLCommandBuffer> cb, id<MTLTexture> target, const FrameRecord &rec, const EnhancedFrame *mixedIn, double ageMs)
	{
		const uint64_t n = captureSeen++;
		const uint32_t every = cfg.captureEvery ? cfg.captureEvery : 1;
		if(n < cfg.captureSkip || (n - cfg.captureSkip) % every != 0 || captured.size() >= cfg.captureCount)
			return;
		CapturedFrame c;
		c.width = target.width;
		c.height = target.height;
		c.format = target.pixelFormat;
		c.bytesPerPixel = c.format == MTLPixelFormatRGBA16Float ? 8 : 4;
		c.bytes = [device newBufferWithLength:c.width * c.height * c.bytesPerPixel options:MTLResourceStorageModeShared];
		if(!c.bytes)
			return;
		c.seq = rec.seq;
		c.frameIndex = rec.frameIndex;
		c.decodedUs = rec.decodedUs;
		c.enhanceAgeMs = mixedIn ? ageMs : -1.0;
		c.keySeq = mixedIn && mixedIn->key ? mixedIn->key->seq : 0;
		id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
		[blit copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
				   sourceSize:MTLSizeMake(c.width, c.height, 1) toBuffer:c.bytes destinationOffset:0
		destinationBytesPerRow:c.width * c.bytesPerPixel destinationBytesPerImage:c.width * c.height * c.bytesPerPixel];
		[blit endEncoding];
		captured.push_back(c);
	}

	// After the last command buffer has completed.
	void writeCaptured()
	{
		if(captured.empty() || cfg.captureDir.empty())
			return;
		FILE *csv = fopen((cfg.captureDir + "/frames.csv").c_str(), "w");
		if(!csv)
		{
			NSLog(@"plk-video: cannot write to %s", cfg.captureDir.c_str());
			captured.clear();
			return;
		}
		fprintf(csv, "file,width,height,format,seq,frame_index,decoded_us,enhance_age_ms,enhance_key_seq\n");
		for(size_t i = 0; i < captured.size(); i++)
		{
			const CapturedFrame &c = captured[i];
			char name[32];
			snprintf(name, sizeof(name), "%04zu.raw", i);
			if(FILE *f = fopen((cfg.captureDir + "/" + name).c_str(), "wb"))
			{
				fwrite(c.bytes.contents, 1, c.width * c.height * c.bytesPerPixel, f);
				fclose(f);
			}
			const char *format = c.format == MTLPixelFormatBGRA8Unorm ? "bgra8" : c.format == MTLPixelFormatBGR10A2Unorm ? "bgr10a2"
				: c.format == MTLPixelFormatRGBA16Float ? "rgba16f" : "other";
			fprintf(csv, "%s,%lu,%lu,%s,%llu,%u,%.0f,%.2f,%llu\n", name, (unsigned long)c.width, (unsigned long)c.height, format,
					(unsigned long long)c.seq, (unsigned)c.frameIndex, c.decodedUs, c.enhanceAgeMs, (unsigned long long)c.keySeq);
		}
		fclose(csv);
		captured.clear();
	}

	bool readPixel(int x, int y, float rgba[4])
	{
		if(!lastTarget || x < 0 || y < 0 || (NSUInteger)x >= lastTarget.width || (NSUInteger)y >= lastTarget.height)
			return false;
		id<MTLBuffer> b = [device newBufferWithLength:16 options:MTLResourceStorageModeShared];
		id<MTLCommandBuffer> cb = [queue commandBuffer];
		id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
		NSUInteger bpp = lastTarget.pixelFormat == MTLPixelFormatRGBA16Float ? 8 : 4;
		[blit copyFromTexture:lastTarget sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x, y, 0)
				   sourceSize:MTLSizeMake(1, 1, 1) toBuffer:b destinationOffset:0 destinationBytesPerRow:bpp
		 destinationBytesPerImage:bpp];
		[blit endEncoding];
		[cb commit];
		[cb waitUntilCompleted];
		const uint8_t *p = (const uint8_t *)b.contents;
		switch(lastTarget.pixelFormat)
		{
			case MTLPixelFormatBGRA8Unorm:
				rgba[0] = p[2] / 255.0f; rgba[1] = p[1] / 255.0f; rgba[2] = p[0] / 255.0f; rgba[3] = p[3] / 255.0f;
				return true;
			case MTLPixelFormatBGR10A2Unorm:
			{
				uint32_t v;
				std::memcpy(&v, p, 4);
				rgba[2] = (v & 1023) / 1023.0f;
				rgba[1] = ((v >> 10) & 1023) / 1023.0f;
				rgba[0] = ((v >> 20) & 1023) / 1023.0f;
				rgba[3] = (v >> 30) / 3.0f;
				return true;
			}
			case MTLPixelFormatRGBA16Float:
			{
				__fp16 h[4];
				std::memcpy(h, p, 8);
				for(int i = 0; i < 4; i++)
					rgba[i] = (float)h[i];
				return true;
			}
			default:
				return false;
		}
	}
};

}  // namespace plk::video

@implementation PLKDisplayLinkTarget
- (void)metalDisplayLink:(CAMetalDisplayLink *)link needsUpdate:(CAMetalDisplayLinkUpdate *)update
{
	(void)link;
	if(_impl)
		_impl->onDisplayLink(update);
}
@end

namespace plk::video {

std::unique_ptr<VideoPipeline> VideoPipeline::create(void *metalLayer, const PipelineConfig &config, ErrorCallback onError,
		std::string *error)
{
	auto fail = [&](const std::string &m) -> std::unique_ptr<VideoPipeline> {
		if(error)
			*error = m;
		return nullptr;
	};
	CAMetalLayer *layer = (__bridge CAMetalLayer *)metalLayer;
	if(!layer || ![layer isKindOfClass:[CAMetalLayer class]])
		return fail("metalLayer is not a CAMetalLayer");

	auto impl = std::make_unique<Impl>();
	impl->cfg = config;
	impl->onError = std::move(onError);
	impl->shared = std::make_shared<Shared>(config.statsWindow);
	impl->shared->onFrame = config.onFrame;
	impl->shared->onEnhance = config.onEnhance;
	impl->scaler = config.scaler;
	impl->useCompressedOutput = config.compressedDecoderOutput;
	impl->latchMarginUs.store(config.latchMarginUs);
	impl->displayHeadroom.store(config.displayHeadroom);
	impl->layer = layer;
	impl->device = layer.device ?: layer.preferredDevice ?: MTLCreateSystemDefaultDevice();
	if(!impl->device)
		return fail("no Metal device");
	impl->queue = [impl->device newCommandQueue];
	NSError *err = nil;
	double t0 = nowUs();
	impl->library = [impl->device newLibraryWithSource:@(kPlkShaderSource) options:[MTLCompileOptions new] error:&err];
	if(!impl->library)
		return fail(std::string("shader compile failed: ") + (err ? err.localizedDescription.UTF8String : "?"));
	NSLog(@"plk-video: shaders compiled in %.1f ms", (nowUs() - t0) / 1000.0);
	if(CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, impl->device, nullptr, &impl->texCache) != kCVReturnSuccess)
		return fail("CVMetalTextureCacheCreate failed");
	impl->memPool = CMMemoryPoolCreate(nullptr);

	[CATransaction begin];
	[CATransaction setDisableActions:YES];
	layer.device = impl->device;
	layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	layer.framebufferOnly = YES;
	layer.opaque = YES;
	layer.maximumDrawableCount = config.maxDrawables == 2 ? 2 : 3;
	layer.allowsNextDrawableTimeout = YES;
	layer.presentsWithTransaction = NO;
	CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB); // see colorSpaceName()
	layer.colorspace = cs;
	CGColorSpaceRelease(cs);
	[CATransaction commit];
	{
		std::lock_guard<std::mutex> sl(impl->shared->m);
		impl->shared->c.scaler = toString(config.scaler);
	}

	std::promise<void> ready;
	auto readyFuture = ready.get_future();
	Impl *raw = impl.get();
	raw->enhance->wake = [raw] { raw->post([raw] { raw->onEnhancerEvent(); }); };
	impl->renderThread = std::thread([raw, &ready] { raw->renderThreadMain(&ready); });
	readyFuture.wait();
	return std::unique_ptr<VideoPipeline>(new VideoPipeline(std::move(impl)));
}

VideoPipeline::VideoPipeline(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

VideoPipeline::~VideoPipeline()
{
	Impl *d = impl_.get();
	d->stopping.store(true);
	{
		std::lock_guard<std::mutex> lk(d->decMutex);
		d->destroySession();
	}
	{
		std::lock_guard<std::mutex> cl(d->shared->cbMutex);
		d->shared->onFrame = nullptr;
		d->shared->onEnhance = nullptr;
	}
	d->renderStop.store(true);
	d->slotCv.notify_all();
	d->wakeRenderThread();
	if(d->renderThread.joinable())
		d->renderThread.join();
	@autoreleasepool
	{
		[d->lastCommandBuffer waitUntilCompleted];
		[d->lastKeyCommandBuffer waitUntilCompleted];
		d->writeCaptured();
		// A result still on its way lands in the state its callback holds and is dropped there.
		d->dropEnhanced();
		{
			std::lock_guard<std::mutex> lk(d->enhance->m);
			d->enhance->wake = nullptr;
		}
		{
			// Nothing is told any more; a picture still being measured ends by itself.
			std::lock_guard<std::mutex> lk(d->motion->m);
			d->motion->sink = nullptr;
		}
		// Whoever was being told has been told when this returns: what the sink reaches may go.
		dispatch_sync(d->motion->queue, ^{});
		[d->lastRgbCommandBuffer waitUntilCompleted];
		for(auto &p : d->due)
			if(p.f.buf)
				CVPixelBufferRelease(p.f.buf);
		for(auto &stage : d->order)
			if(!stage->shown && stage->f.buf)
				CVPixelBufferRelease(stage->f.buf);
		d->order.clear();
		for(auto &h : d->awaiting)
			if(h.f.buf)
				CVPixelBufferRelease(h.f.buf);
		d->awaiting.clear();
		d->due.clear();
		d->lastPicture.reset();
		if(d->rgbPool)
			CVPixelBufferPoolRelease(d->rgbPool);
		for(auto &h : d->held)
			if(h.f.buf)
				CVPixelBufferRelease(h.f.buf);
		d->held.clear();
		d->clearPending();
		if(d->keyPool)
			CVPixelBufferPoolRelease(d->keyPool);
		DecodedFrame f = d->takeSlot();
		if(f.buf)
			CVPixelBufferRelease(f.buf);
		if(d->wakeSource) CFRelease(d->wakeSource);
		if(d->runLoop) CFRelease(d->runLoop);
		if(d->texCache) CFRelease(d->texCache);
		if(d->memPool)
		{
			CMMemoryPoolInvalidate(d->memPool);
			CFRelease(d->memPool);
		}
		impl_.reset();
	}
}

bool VideoPipeline::submitConfig(const uint8_t *data, size_t size) { return impl_->submitConfig(data, size); }

bool VideoPipeline::submitFrame(const uint8_t *data, size_t size, uint16_t frameIndex, uint64_t frameReadyUs,
		bool recovered, int32_t framesLost)
{
	return impl_->submitFrame(data, size, frameIndex, frameReadyUs, recovered, framesLost);
}

void VideoPipeline::setPacingMode(PacingMode mode)
{
	if(impl_->offscreenMode())
		return;
	Impl *d = impl_.get();
	d->post([d, mode] { d->applyPacing(mode); });
}

void VideoPipeline::setScaler(Scaler scaler)
{
	Impl *d = impl_.get();
	d->post([d, scaler] { d->scaler = scaler; });
}

// The layer's colour space and pixel format follow from these two: the next frame sets the
// layer up again (renderFrame()), on the render thread like every other change to it.
void VideoPipeline::setSdrColor(SdrColor color)
{
	Impl *d = impl_.get();
	d->post([d, color] {
		if(d->cfg.sdrColor == color)
			return;
		d->cfg.sdrColor = color;
		d->layerConfigured = false;
	});
}

void VideoPipeline::setHdrOutput(HdrOutput output)
{
	Impl *d = impl_.get();
	d->post([d, output] {
		if(d->cfg.hdrOutput == output)
			return;
		d->cfg.hdrOutput = output;
		d->layerConfigured = false;
	});
}

// The layer keeps its colour space and format: the next frame is scaled in the shader and
// the layer's HDR metadata follows (renderFrame()).
void VideoPipeline::setHdrBrightness(float gain)
{
	Impl *d = impl_.get();
	gain = std::min(std::max(gain, 0.05f), 4.0f);
	d->post([d, gain] { d->cfg.hdrBrightness = gain; });
}

// The layer's HDR metadata comes or goes with this: the next frame sets the layer up again.
void VideoPipeline::setHdrToneMap(HdrToneMap toneMap)
{
	Impl *d = impl_.get();
	d->post([d, toneMap] {
		if(d->cfg.hdrToneMap == toneMap)
			return;
		d->cfg.hdrToneMap = toneMap;
		d->layerConfigured = false;
	});
}

void VideoPipeline::setDisplayHeadroom(float headroom) { impl_->displayHeadroom.store(headroom > 0 ? headroom : 0.0f); }

void VideoPipeline::setLatchMarginUs(uint32_t marginUs) { impl_->latchMarginUs.store(marginUs); }

void VideoPipeline::setDrawableSize(int w, int h)
{
	Impl *d = impl_.get();
	d->post([d, w, h] {
		[CATransaction begin];
		[CATransaction setDisableActions:YES];
		d->layer.drawableSize = CGSizeMake(w, h);
		[CATransaction commit];
	});
}

void VideoPipeline::setContentInsets(int left, int top, int right, int bottom)
{
	Impl *d = impl_.get();
	left = std::max(left, 0);
	top = std::max(top, 0);
	right = std::max(right, 0);
	bottom = std::max(bottom, 0);
	d->post([d, left, top, right, bottom] {
		d->insetLeft = left;
		d->insetTop = top;
		d->insetRight = right;
		d->insetBottom = bottom;
	});
}

void VideoPipeline::setOverlay(const OverlayImage &image)
{
	Impl *d = impl_.get();
	if(!image.bgra || image.width <= 0 || image.height <= 0)
	{
		d->post([d] { d->overlayTex = nil; });
		return;
	}
	// A texture of its own for every picture: the one before may still be read by a frame
	// the GPU has not finished. The device makes textures from any thread.
	MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
																				  width:(NSUInteger)image.width
																				 height:(NSUInteger)image.height
																			  mipmapped:NO];
	td.usage = MTLTextureUsageShaderRead;
	td.storageMode = MTLStorageModeShared;
	id<MTLTexture> tex = [d->device newTextureWithDescriptor:td];
	if(!tex)
		return;
	[tex replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)image.width, (NSUInteger)image.height)
		   mipmapLevel:0
			 withBytes:image.bgra
		   bytesPerRow:image.bytesPerRow ? image.bytesPerRow : (size_t)image.width * 4];
	// As setEnhancer() does with its states: built here, not on the render thread.
	std::vector<std::pair<std::pair<int, MTLPixelFormat>, id<MTLRenderPipelineState>>> built;
	if(!d->overlayStatesBuilt.exchange(true))
		for(MTLPixelFormat fmt : {MTLPixelFormatBGRA8Unorm, MTLPixelFormatBGR10A2Unorm, MTLPixelFormatRGBA16Float})
			built.push_back({{(int)Impl::FragOverlay, fmt}, d->makePipeline(Impl::FragOverlay, fmt)});
	const int x = image.x, y = image.y;
	d->post([d, tex, built, x, y] {
		for(const auto &b : built)
			if(b.second)
				d->pipelines[b.first] = b.second;
		d->overlayTex = tex;
		d->overlayX = x;
		d->overlayY = y;
	});
}

void VideoPipeline::setEnhancer(std::shared_ptr<FrameEnhancer> enhancer)
{
	Impl *d = impl_.get();
	// The states of the mixed-in path are built here, on the caller's thread: each takes a
	// frame's time many times over, and the render thread has frames to show meanwhile.
	std::vector<std::pair<std::pair<int, MTLPixelFormat>, id<MTLRenderPipelineState>>> built;
	std::string description;
	if(enhancer)
	{
		description = enhancer->describe();
		if(!d->mixStatesBuilt.exchange(true))
		{
			const std::pair<Impl::Frag, MTLPixelFormat> wanted[] = {
				// The table of EnhanceConfig::guided.
				{Impl::FragGridBuild, MTLPixelFormatRGBA16Float},
				{Impl::FragBilinearNr, MTLPixelFormatBGRA8Unorm},
				{Impl::FragBicubicNr, MTLPixelFormatBGRA8Unorm},
				{Impl::FragLanczosNr, MTLPixelFormatBGRA8Unorm},
				// MetalFX's first pass, and an HDR picture on a linear drawable.
				{Impl::FragBilinearNr, MTLPixelFormatRGBA16Float},
				{Impl::FragBicubicNr, MTLPixelFormatRGBA16Float},
				{Impl::FragLanczosNr, MTLPixelFormatRGBA16Float},
				// An HDR picture on a PQ drawable.
				{Impl::FragBilinearNr, MTLPixelFormatBGR10A2Unorm},
				{Impl::FragBicubicNr, MTLPixelFormatBGR10A2Unorm},
				{Impl::FragLanczosNr, MTLPixelFormatBGR10A2Unorm},
			};
			double t0 = nowUs();
			for(const auto &w : wanted)
				built.push_back({{(int)w.first, w.second}, d->makePipeline(w.first, w.second)});
			NSLog(@"plk-video: enhancer pipeline states built in %.1f ms", (nowUs() - t0) / 1000.0);
		}
	}
	d->post([d, enhancer, built, description] {
		for(const auto &b : built)
			if(b.second)
				d->pipelines[b.first] = b.second;
		d->enhancer = enhancer;
		d->dropEnhanced();
		std::lock_guard<std::mutex> sl(d->shared->m);
		d->shared->c.enhancer = description;
		if(!enhancer)
		{
			d->shared->c.enhanceWidth = 0;
			d->shared->c.enhanceHeight = 0;
		}
	});
}

void VideoPipeline::setInterpolator(std::shared_ptr<FrameInterpolator> interpolator)
{
	Impl *d = impl_.get();
	// The states a whole R'G'B' picture is drawn with, built here and not on the render
	// thread, which has frames to show meanwhile.
	std::vector<std::pair<std::pair<int, MTLPixelFormat>, id<MTLRenderPipelineState>>> built;
	if(interpolator)
		for(MTLPixelFormat fmt : {MTLPixelFormatBGRA8Unorm, MTLPixelFormatBGR10A2Unorm, MTLPixelFormatRGBA16Float})
			built.push_back({{(int)Impl::FragRgb, fmt}, d->makePipeline(Impl::FragRgb, fmt)});
	d->post([d, interpolator, built] {
		for(const auto &b : built)
			if(b.second && !d->pipelines.count(b.first))
				d->pipelines[b.first] = b.second;
		d->endGeneration();
		d->interpolator = interpolator;
	});
}

void VideoPipeline::motionCost(double *costMs, uint64_t *skipped) const
{
	*costMs = impl_->motion->costUs.load() / 1000.0;
	*skipped = impl_->motion->skipped.load();
}

void VideoPipeline::setTurning(bool turning)
{
	std::atomic<double> &since = impl_->motion->turnSinceUs;
	if(!turning)
		since.store(0);
	else if(since.load() == 0)
		since.store(hostTimeUs());
}

void VideoPipeline::setFrameTap(std::function<void(const uint8_t *grey, int width, int height, double pictureUs)> tap)
{
	std::shared_ptr<Impl::MotionState> st = impl_->motion;
	std::lock_guard<std::mutex> lk(st->m);
	st->tap = tap;
}

void VideoPipeline::setWarp(std::function<void(const PictureMotion &)> motion,
		std::function<void(double pictureUs, double displayUs, float *dx, float *dy)> warp)
{
	Impl *d = impl_.get();
	d->post([d, motion, warp] {
		d->motionSink = motion;
		d->warpSource = warp;
		std::shared_ptr<Impl::MotionState> st = d->motion;
		{
			std::lock_guard<std::mutex> lk(st->m);
			st->sink = motion;
		}
		dispatch_async(st->queue, ^{
			st->last.clear();
			st->lastUs = 0;
		});
	});
}

void VideoPipeline::setFrameGeneration(int generated)
{
	Impl *d = impl_.get();
	generated = generated < 0 ? 0 : generated > 15 ? 15 : generated;
	d->post([d, generated] {
		if(d->generating && !generated)
			d->endGeneration();
		d->generating = generated;
	});
}

void VideoPipeline::setEnhanceConfig(const EnhanceConfig &config)
{
	Impl *d = impl_.get();
	d->post([d, config] {
		bool was = d->enhanceCfg.enabled;
		d->enhanceCfg = config;
		if(was != config.enabled)
			d->dropEnhanced();
		// Frames held under the old choice are not kept waiting under the new one.
		if(!config.wait || !config.enabled)
			d->flushHeld();
	});
}

Stats VideoPipeline::counters() const
{
	Shared &s = *impl_->shared;
	std::lock_guard<std::mutex> lk(s.m);
	return s.c;
}

Stats VideoPipeline::stats() const
{
	Shared &s = *impl_->shared;
	std::lock_guard<std::mutex> lk(s.m);
	Stats out = s.c;
	out.enhanceMs = s.enhance.summarize();
	out.enhanceInputGpuMs = s.enhanceInputGpu.summarize();
	out.enhanceAgeMs = s.enhanceAge.summarize();
	out.submitCallMs = s.submitCall.summarize();
	out.decodeMs = s.decode.summarize();
	out.queueMs = s.queue.summarize();
	out.renderCpuMs = s.renderCpu.summarize();
	out.renderGpuMs = s.renderGpu.summarize();
	out.submitToPresentedMs = s.submitToPresented.summarize();
	out.readyToPresentedMs = s.readyToPresented.summarize();
	out.decodedToPresentedMs = s.decodedToPresented.summarize();
	out.submitIntervalMs = s.submitInterval.summarize();
	out.presentIntervalMs = s.presentInterval.summarize();
	out.linkLeadMs = s.linkLead.summarize();
	out.linkDeadlineMs = s.linkDeadline.summarize();
	out.presentVsTargetMs = s.presentVsTarget.summarize();
	return out;
}

void VideoPipeline::resetStats() { impl_->shared->reset(); }

void VideoPipeline::debugInvalidateSession()
{
	std::lock_guard<std::mutex> lk(impl_->decMutex);
	if(impl_->session)
		VTDecompressionSessionInvalidate(impl_->session);
}

bool VideoPipeline::debugReadPixel(int x, int y, float rgba[4])
{
	Impl *d = impl_.get();
	auto p = std::make_shared<std::promise<bool>>();
	auto fut = p->get_future();
	float tmp[4] = {0, 0, 0, 0};
	float *out = tmp;
	d->post([d, x, y, out, p] { p->set_value(d->readPixel(x, y, out)); });
	bool ok = fut.get();
	std::memcpy(rgba, tmp, sizeof(tmp));
	return ok;
}

}  // namespace plk::video
