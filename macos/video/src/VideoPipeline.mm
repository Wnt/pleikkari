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
	simd_float2 lumaSize, chromaSize, uvScale, uvOffset, chromaShift, linearize, toneMap, sdrMap;
};
static_assert(sizeof(ShaderParams) == 112, "must match Params in Shaders.metal");

// Where SDR white is in the SDR rendition of a PQ picture that an enhancer is given
// (sdr_rendition() in Shaders.metal): HDR reference white, ITU-R BT.2408.
const float kEnhancerSdrWhiteNits = 203.0f;

struct alignas(16) NrShaderParams {
	simd_float4 lumaWeights;
	simd_float2 keySize, motion;
	float strength;
	float split;
};
static_assert(sizeof(NrShaderParams) == 48, "must match NrParams in Shaders.metal");

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
	std::atomic<bool> mixStatesBuilt{false};  // any thread: setEnhancer() builds them once

	enum Frag { FragBilinear = 0, FragBicubic, FragLanczos, FragCopy, FragBilinearNr, FragBicubicNr, FragLanczosNr };
	// What a pass needs on top of the plane textures to mix an enhancer's result in.
	struct NrDraw {
		id<MTLTexture> key, out;
		NrShaderParams params;
	};

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
				renderFrame(f, nil, 0);
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
			"fs_bilinear_nr", "fs_bicubic_nr", "fs_lanczos_nr"};
		MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
		d.vertexFunction = [library newFunctionWithName:@"vs_fullscreen"];
		d.fragmentFunction = [library newFunctionWithName:@(names[frag])];
		d.colorAttachments[0].pixelFormat = fmt;
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

	id<MTLTexture> makeTexture(MTLPixelFormat fmt, NSUInteger w, NSUInteger h, MTLTextureUsage usage)
	{
		MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:w height:h mipmapped:NO];
		d.usage = usage;
		d.storageMode = MTLStorageModePrivate;
		return [device newTextureWithDescriptor:d];
	}

	void drawPass(id<MTLCommandBuffer> cb, id<MTLTexture> target, MTLRegion vp, id<MTLRenderPipelineState> pso,
			id<MTLTexture> t0, id<MTLTexture> t1, const ShaderParams *params, const NrDraw *nr = nullptr)
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
			[enc setFragmentBytes:params length:sizeof(ShaderParams) atIndex:0];
		if(nr)
		{
			[enc setFragmentTexture:nr->key atIndex:2];
			[enc setFragmentTexture:nr->out atIndex:3];
			[enc setFragmentBytes:&nr->params length:sizeof(NrShaderParams) atIndex:1];
		}
		[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		[enc endEncoding];
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
		*ageMs = age;
		return e;
	}

	// Draws this frame for the enhancer and hands it over, if the enhancer is idle. Runs
	// after the frame's own command buffer has been committed: on the GPU the copy comes
	// after the presented picture, and nothing here is waited for.
	void offerToEnhancer(const std::shared_ptr<FrameRecord> &rec, CVPixelBufferRef buf, CVMetalTextureRef ty,
			CVMetalTextureRef tc, const ShaderParams &shown, CGRect clean, const ColorInfo &ci)
	{
		if(enhance->busy.load() || nowUs() < enhance->holdUntilUs.load())
			return;
		size_t sw = (size_t)clean.size.width, sh = (size_t)clean.size.height;
		size_t ph = enhanceCfg.processHeight <= 0 || (size_t)enhanceCfg.processHeight >= sh
			? sh : (size_t)std::max(enhanceCfg.processHeight, 320);
		size_t pw = ph == sh ? sw : (size_t)std::llround((double)sw * (double)ph / (double)sh);
		pw += pw & 1;
		if(pw < 64 || ph < 64)
			return;
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
				return;
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
			return;
		key->tex = wrapEnhancerBuffer(key->buf, true);
		id<MTLRenderPipelineState> pso = pipelineFor(FragBilinear, enhancerTextureFormat(format));
		if(!key->tex || !pso)
			return;
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
	}

	void renderFrame(DecodedFrame &f, id<CAMetalDrawable> drawable, double targetPresentUs)
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

		MTLRegion dest = letterbox(clean.size.width, clean.size.height, target.width, target.height);
		Scaler eff = scaler;
		if(eff == Scaler::MetalFX && (dest.size.width < (NSUInteger)clean.size.width || dest.size.height < (NSUInteger)clean.size.height))
			eff = Scaler::Bilinear;  // MetalFX spatial upscales only

		// The enhancer's newest result, if this frame can carry it (see offerToEnhancer()
		// for where the results come from).
		bool enhanceOn = enhancing(ci);
		NrDraw nrDraw;
		double nrAgeMs = 0;
		std::shared_ptr<EnhancedFrame> nr;
		if(enhanceOn)
			nr = enhancedFor(rec->decodedUs, ci, nrDraw, &nrAgeMs);
		bool mixed = false;

		id<MTLCommandBuffer> cb = [queue commandBuffer];
		bool encoded = false;
		if(eff == Scaler::MetalFX)
		{
			id<MTLRenderPipelineState> mix = nr ? pipelineIfBuilt(FragBilinearNr, MTLPixelFormatRGBA16Float) : nil;
			encoded = encodeMetalFX(cb, target, dest, texY, texC, params, (NSUInteger)clean.size.width,
					(NSUInteger)clean.size.height, linear, mix, mix ? &nrDraw : nullptr);
			if(!encoded)
				eff = Scaler::Bilinear;
			else
				mixed = mix != nil;
		}
		if(!encoded)
		{
			Frag frag = eff == Scaler::Bicubic ? FragBicubic : eff == Scaler::Lanczos ? FragLanczos : FragBilinear;
			id<MTLRenderPipelineState> mix = nr ? pipelineIfBuilt(mixedIn(frag), target.pixelFormat) : nil;
			if(mix)
				drawPass(cb, target, dest, mix, texY, texC, &params, &nrDraw);
			else
				drawPass(cb, target, dest, pipelineFor(frag, target.pixelFormat), texY, texC, &params);
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
			if(rec->pending.fetch_sub(1) == 1)
				sh->finish(*rec);
		}];
		if(!off)
		{
			[drawable addPresentedHandler:^(id<MTLDrawable> d) {
				rec->presentedUs = d.presentedTime * 1e6;
				if(rec->pending.fetch_sub(1) == 1)
					sh->finish(*rec);
			}];
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
			if(mixed)
			{
				shared->c.framesEnhanced++;
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
			offerToEnhancer(rec, buf, ty, tc, params, clean, ci);
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
			id<MTLRenderPipelineState> mix, const NrDraw *nr)
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
		drawPass(cb, target, dest, pipelineFor(FragCopy, target.pixelFormat), fxOut, nil, nullptr);
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

void VideoPipeline::setEnhanceConfig(const EnhanceConfig &config)
{
	Impl *d = impl_.get();
	d->post([d, config] {
		bool was = d->enhanceCfg.enabled;
		d->enhanceCfg = config;
		if(was != config.enabled)
			d->dropEnhanced();
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
