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

#import "AnnexB.h"
#import "RollingStats.h"

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
	simd_float2 lumaSize, chromaSize, uvScale, uvOffset, chromaShift, linearize;
};
static_assert(sizeof(ShaderParams) == 96, "must match Params in Shaders.metal");

// Folds bit-depth container, range and YCbCr->R'G'B' matrix into three rows
// applied to raw normalized texture values.
void colorRows(const ColorInfo &c, ShaderParams &p)
{
	double kr, kb;
	switch(c.matrix)
	{
		case Matrix::BT601: kr = 0.299; kb = 0.114; break;
		case Matrix::BT2020: kr = 0.2627; kb = 0.0593; break;
		default: kr = 0.2126; kb = 0.0722; break;
	}
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

CGColorSpaceRef copyColorSpace(const ColorInfo &c, HdrOutput mode)
{
	CFStringRef name;
	if(linearOutput(c, mode)) name = c.primaries == Primaries::P3 ? kCGColorSpaceExtendedLinearDisplayP3 : kCGColorSpaceExtendedLinearITUR_2020;
	else if(c.transfer == Transfer::PQ) name = kCGColorSpaceITUR_2100_PQ;
	else if(c.transfer == Transfer::HLG) name = kCGColorSpaceITUR_2100_HLG;
	else if(c.primaries == Primaries::BT2020) name = kCGColorSpaceITUR_2020;
	else if(c.primaries == Primaries::P3) name = kCGColorSpaceDisplayP3;
	else if(c.transfer == Transfer::SRGB) name = kCGColorSpaceSRGB;
	else name = kCGColorSpaceITUR_709;
	return CGColorSpaceCreateWithName(name);
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

// ---- per-frame record -----------------------------------------------------

struct FrameRecord {
	uint64_t seq = 0;
	uint16_t frameIndex = 0;
	bool keyframe = false;
	double readyUs = 0, submitUs = 0, decodedUs = 0, renderStartUs = 0, committedUs = 0;
	double gpuStartUs = 0, gpuEndUs = 0, presentedUs = 0, targetPresentUs = 0;
	bool presentExpected = false;
	std::atomic<int> pending{0};
	std::shared_ptr<const HdrMeta> hdr;
};

struct DecodedFrame {
	CVPixelBufferRef buf = nullptr;  // retained
	std::shared_ptr<FrameRecord> rec;
};

// Statistics shared with Metal/VT handlers that may outlive the Impl.
struct Shared {
	explicit Shared(size_t window)
		: submitCall(window), decode(window), queue(window), renderCpu(window), renderGpu(window), submitToPresented(window),
		  readyToPresented(window), decodedToPresented(window), submitInterval(window), presentInterval(window),
		  linkLead(window), linkDeadline(window), presentVsTarget(window)
	{
	}
	mutable std::mutex m;
	Stats c;  // counters and stream info only
	detail::RollingWindow submitCall, decode, queue, renderCpu, renderGpu, submitToPresented, readyToPresented,
		decodedToPresented, submitInterval, presentInterval, linkLead, linkDeadline, presentVsTarget;
	double lastSubmitUs = 0, lastPresentedUs = 0;

	void finish(const FrameRecord &r)
	{
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
		fresh.targetWidth = c.targetWidth; fresh.targetHeight = c.targetHeight;
		c = fresh;
		for(auto *w : {&submitCall, &decode, &queue, &renderCpu, &renderGpu, &submitToPresented, &readyToPresented,
				&decodedToPresented, &submitInterval, &presentInterval, &linkLead, &linkDeadline, &presentVsTarget})
			w->clear();
		lastSubmitUs = 0;
		lastPresentedUs = 0;
	}
};

constexpr double kIdrRequestMinIntervalUs = 100000.0;

}  // namespace

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
bool parsePacingMode(const std::string &s, PacingMode *out)
{
	if(s == "immediate" || s == "arrival") { *out = PacingMode::Immediate; return true; }
	if(s == "immediate-vsync") { *out = PacingMode::ImmediateVsync; return true; }
	if(s == "displaylink" || s == "vsync") { *out = PacingMode::DisplayLink; return true; }
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
	PacingMode activePacing = PacingMode::DisplayLink;
	Scaler scaler = Scaler::Bilinear;
	id<MTLFXSpatialScaler> fx;
	id<MTLTexture> fxIn, fxOut;
	id<MTLTexture> offscreen;
	id<MTLTexture> lastTarget;
	id<MTLCommandBuffer> lastCommandBuffer;
	uint64_t framesSinceFlush = 0;

	enum Frag { FragBilinear = 0, FragBicubic, FragLanczos, FragCopy };

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

	id<MTLRenderPipelineState> pipelineFor(Frag frag, MTLPixelFormat fmt)
	{
		auto key = std::make_pair((int)frag, fmt);
		auto it = pipelines.find(key);
		if(it != pipelines.end())
			return it->second;
		static const char *names[] = {"fs_bilinear", "fs_bicubic", "fs_lanczos", "fs_copy"};
		MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
		d.vertexFunction = [library newFunctionWithName:@"vs_fullscreen"];
		d.fragmentFunction = [library newFunctionWithName:@(names[frag])];
		d.colorAttachments[0].pixelFormat = fmt;
		NSError *err = nil;
		id<MTLRenderPipelineState> p = [device newRenderPipelineStateWithDescriptor:d error:&err];
		if(!p)
			NSLog(@"plk-video: pipeline %s failed: %@", names[frag], err);
		pipelines[key] = p;
		return p;
	}

	void configureLayer(const ColorInfo &c, const std::shared_ptr<const HdrMeta> &hdr)
	{
		CGColorSpaceRef cs = copyColorSpace(c, cfg.hdrOutput);
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
		layerConfigured = true;
	}

	CAEDRMetadata *edrMetadataFor(const ColorInfo &c, const std::shared_ptr<const HdrMeta> &hdr)
	{
		if(!c.hdr())
			return nil;
		if(c.transfer == Transfer::HLG)
			return [CAEDRMetadata HLGMetadata];
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
			snprintf(desc, sizeof desc, "SEI: mastering %s%.4f-%.0f nits, CLL %s%u/%u nits",
					hasMd ? "" : "(none) ",
					hasMd ? ((md[20] << 24 | md[21] << 16 | md[22] << 8 | md[23]) / 10000.0) : 0.0,
					hasMd ? ((uint32_t)(md[16] << 24 | md[17] << 16 | md[18] << 8 | md[19]) / 10000.0) : 0.0,
					hasCll ? "" : "(none) ", (unsigned)(cl[0] << 8 | cl[1]), (unsigned)(cl[2] << 8 | cl[3]));
			{
				std::lock_guard<std::mutex> sl(shared->m);
				shared->c.hdrMetadata = desc;
			}
			NSData *mdcv = hasMd ? [NSData dataWithBytes:md.data() length:24] : nil;
			NSData *cll = hasCll ? [NSData dataWithBytes:cl.data() length:4] : nil;
			return [CAEDRMetadata HDR10MetadataWithDisplayInfo:mdcv contentInfo:cll opticalOutputScale:100.0f];
		}
		// No SEI: PS5-like defaults (1000-nit mastering display).
		{
			std::lock_guard<std::mutex> sl(shared->m);
			shared->c.hdrMetadata = "defaults: mastering 0.005-1000 nits";
		}
		return [CAEDRMetadata HDR10MetadataWithMinLuminance:0.005f maxLuminance:1000.0f opticalOutputScale:100.0f];
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
			id<MTLTexture> t0, id<MTLTexture> t1, const ShaderParams *params)
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
		[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		[enc endEncoding];
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
			else if(ci.hdr() && rec->hdr != layerHdr)
			{
				[CATransaction begin];
				[CATransaction setDisableActions:YES];
				layer.EDRMetadata = edrMetadataFor(ci, rec->hdr);
				[CATransaction commit];
				layerHdr = rec->hdr;
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
		// ExtendedLinear: 1.0 = 100 nits, matching opticalOutputScale 100.
		params.linearize = simd_make_float2(linear ? 1.0f : 0.0f, 100.0f);

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

		id<MTLCommandBuffer> cb = [queue commandBuffer];
		bool encoded = false;
		if(eff == Scaler::MetalFX)
		{
			encoded = encodeMetalFX(cb, target, dest, texY, texC, params, (NSUInteger)clean.size.width,
					(NSUInteger)clean.size.height, linear);
			if(!encoded)
				eff = Scaler::Bilinear;
		}
		if(!encoded)
		{
			Frag frag = eff == Scaler::Bicubic ? FragBicubic : eff == Scaler::Lanczos ? FragLanczos : FragBilinear;
			drawPass(cb, target, dest, pipelineFor(frag, target.pixelFormat), texY, texC, &params);
		}

		std::shared_ptr<Shared> sh = shared;
		CVPixelBufferRef keepBuf = buf;  // ownership moves to the completion handler
		f.buf = nullptr;
		rec->presentExpected = !off;
		rec->pending.store(off ? 1 : 2);
		[cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
			rec->gpuStartUs = c.GPUStartTime * 1e6;
			rec->gpuEndUs = c.GPUEndTime * 1e6;
			CFRelease(ty);
			CFRelease(tc);
			CVPixelBufferRelease(keepBuf);
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
		}
		[cb commit];
		lastCommandBuffer = cb;
		lastTarget = target;
		(void)targetPresentUs;
		if(++framesSinceFlush >= 120)
		{
			framesSinceFlush = 0;
			CVMetalTextureCacheFlush(texCache, 0);
		}
	}

	bool encodeMetalFX(id<MTLCommandBuffer> cb, id<MTLTexture> target, MTLRegion dest, id<MTLTexture> texY,
			id<MTLTexture> texC, const ShaderParams &params, NSUInteger srcW, NSUInteger srcH, bool linear)
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
		drawPass(cb, fxIn, MTLRegionMake2D(0, 0, srcW, srcH), pipelineFor(FragBilinear, MTLPixelFormatRGBA16Float),
				texY, texC, &params);
		fx.colorTexture = fxIn;
		fx.outputTexture = fxOut;
		fx.inputContentWidth = srcW;
		fx.inputContentHeight = srcH;
		[fx encodeToCommandBuffer:cb];
		drawPass(cb, target, dest, pipelineFor(FragCopy, target.pixelFormat), fxOut, nil, nullptr);
		return true;
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
	impl->scaler = config.scaler;
	impl->useCompressedOutput = config.compressedDecoderOutput;
	impl->latchMarginUs.store(config.latchMarginUs);
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
	CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceITUR_709);
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
	d->renderStop.store(true);
	d->slotCv.notify_all();
	d->wakeRenderThread();
	if(d->renderThread.joinable())
		d->renderThread.join();
	@autoreleasepool
	{
		[d->lastCommandBuffer waitUntilCompleted];
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

Stats VideoPipeline::stats() const
{
	Shared &s = *impl_->shared;
	std::lock_guard<std::mutex> lk(s.m);
	Stats out = s.c;
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
