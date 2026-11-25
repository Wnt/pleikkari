// The enhancer path for an HDR picture, end to end and without a console: an HEVC Main 10
// stream in BT.2020 and PQ encoded here, decoded and rendered offscreen, and a fake
// enhancer that changes the SDR rendition it is given by a known amount.
//
// Checks what the enhancer is given (the rendition of sdr_rendition() in Shaders.metal,
// worked out here a second time, in half float), what reaches the rendered picture (the
// change made to the rendition, with the highlight keeping its ratio to it), and that a
// pixel without a change keeps its code values.
//
// Needs a GPU and the hardware HEVC encoder; exits 77 (skipped) without them.
#import "plk/FrameEnhancer.h"
#import "plk/VideoPipeline.h"

#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <unistd.h>
#include <vector>

using namespace plk::video;

static int failures = 0;
#define CHECK(c) do { if(!(c)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); failures++; } } while(0)
#define NEAR(a, b, tol) do { double va = (a), vb = (b); if(std::fabs(va - vb) > (tol)) { \
	fprintf(stderr, "%s:%d: NEAR(%s, %s): %.4f vs %.4f\n", __FILE__, __LINE__, #a, #b, va, vb); failures++; } } while(0)

namespace {

constexpr int kW = 640, kH = 360;
constexpr int kLeftX = kW / 4, kRightX = kW * 3 / 4;
// What the fake enhancer does to the rendition: less red, more green.
constexpr double kRed = -0.05, kGreen = 0.08;

// ---- the rendition, a second time ---------------------------------------------

struct Rgb {
	double r, g, b;
};

double clamp01(double v) { return std::min(1.0, std::max(0.0, v)); }

double pqEotf(double e)  // code value to linear light, 1.0 = 10000 nits
{
	const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
	const double c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;
	double p = std::pow(e, 1.0 / m2);
	return std::pow(std::max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
}

double pqInverse(double l)
{
	const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
	const double c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;
	double p = std::pow(std::min(1.0, std::max(0.0, l)), m1);
	return std::pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}

const double kWhite = 203.0 / 10000.0;  // SDR white in the rendition, in PQ's linear light
const double kKnee = 0.6, kShoulder = 9.3;

Rgb to709(Rgb v)
{
	return {1.660491 * v.r - 0.587641 * v.g - 0.072850 * v.b, -0.124550 * v.r + 1.132900 * v.g - 0.008349 * v.b,
		-0.018151 * v.r - 0.100579 * v.g + 1.118730 * v.b};
}

Rgb to2020(Rgb v)
{
	return {0.627404 * v.r + 0.329283 * v.g + 0.043313 * v.b, 0.069097 * v.r + 0.919540 * v.g + 0.011362 * v.b,
		0.016391 * v.r + 0.088013 * v.g + 0.895595 * v.b};
}

double shoulder(double x) { return std::min(x, kKnee) + std::log(1.0 + kShoulder * std::max(x - kKnee, 0.0)) / kShoulder; }
double encode(double t) { return std::copysign(std::pow(std::fabs(t), 1.0 / 2.2), t); }
double decode(double c) { return std::copysign(std::pow(std::fabs(c), 2.2), c); }

// Linear light of PQ code values in BT.709 primaries, SDR white at 1.0.
Rgb linear709(Rgb pq) { return to709({pqEotf(pq.r) / kWhite, pqEotf(pq.g) / kWhite, pqEotf(pq.b) / kWhite}); }

// What the enhancer is given of PQ code values, before it is saturated.
Rgb rendition(Rgb pq)
{
	Rgb l = linear709(pq);
	return {encode(shoulder(l.r)), encode(shoulder(l.g)), encode(shoulder(l.b))};
}

// What the fake enhancer returns for a rendition, as a change to it.
Rgb fakeChange(Rgb given)
{
	return {clamp01(given.r + kRed) - given.r, clamp01(given.g + kGreen) - given.g, 0.0};
}

double applyOne(double lin, double d)
{
	double t = shoulder(lin);
	double gain = lin > kKnee ? lin / t : 1.0;
	double c = encode(t);
	return decode(std::max(c + d, std::min(c, 0.0))) * gain;
}

// PQ code values with the change `d` to their rendition made to them.
Rgb applied(Rgb pq, Rgb d)
{
	Rgb l = linear709(pq);
	Rgb o = to2020({applyOne(l.r, d.r), applyOne(l.g, d.g), applyOne(l.b, d.b)});
	return {pqInverse(o.r * kWhite), pqInverse(o.g * kWhite), pqInverse(o.b * kWhite)};
}

// 10-bit limited-range luma of a grey of `nits`.
int lumaFor(double nits) { return (int)std::lround(64.0 + 876.0 * pqInverse(nits / 10000.0)); }

// The left half is not grey: its colour has to come through both conversions between
// BT.2020 and BT.709, and its red is on the shoulder while green and blue are under the knee.
constexpr int kLeftCb = 450, kLeftCr = 600;

// R'G'B' code values of 10-bit limited-range Y'CbCr, BT.2020 non-constant luminance.
Rgb codeOf(int luma, int cb = 512, int cr = 512)
{
	const double y = (luma - 64) / 876.0, pb = (cb - 512) / 896.0, pr = (cr - 512) / 896.0;
	return {clamp01(y + 1.4746 * pr), clamp01(y - 0.16455 * pb - 0.57135 * pr), clamp01(y + 1.8814 * pb)};
}

// ---- the stream ---------------------------------------------------------------

struct Encoder {
	VTCompressionSessionRef session = nullptr;
	std::vector<uint8_t> params;  // VPS, SPS and PPS, Annex-B
	std::vector<uint8_t> frame;   // the access unit of the last encode(), Annex-B
	bool failed = false;
	int64_t count = 0;

	static void append(std::vector<uint8_t> &to, const uint8_t *data, size_t size)
	{
		static const uint8_t start[4] = {0, 0, 0, 1};
		to.insert(to.end(), start, start + 4);
		to.insert(to.end(), data, data + size);
	}

	static void output(void *refcon, void *, OSStatus status, VTEncodeInfoFlags, CMSampleBufferRef sample)
	{
		Encoder *e = static_cast<Encoder *>(refcon);
		if(status != noErr || !sample)
		{
			e->failed = true;
			return;
		}
		CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample);
		size_t sets = 0;
		int lengthSize = 0;
		if(CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, 0, nullptr, nullptr, &sets, &lengthSize) != noErr
			|| lengthSize != 4)
		{
			e->failed = true;
			return;
		}
		if(e->params.empty())
		{
			for(size_t i = 0; i < sets; i++)
			{
				const uint8_t *data = nullptr;
				size_t size = 0;
				CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, i, &data, &size, nullptr, nullptr);
				append(e->params, data, size);
			}
		}
		CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
		size_t total = CMBlockBufferGetDataLength(block);
		std::vector<uint8_t> hvcc(total);
		CMBlockBufferCopyDataBytes(block, 0, total, hvcc.data());
		for(size_t at = 0; at + 4 <= total;)
		{
			size_t size = ((size_t)hvcc[at] << 24) | ((size_t)hvcc[at + 1] << 16) | ((size_t)hvcc[at + 2] << 8) | hvcc[at + 3];
			at += 4;
			if(at + size > total)
				break;
			append(e->frame, hvcc.data() + at, size);
			at += size;
		}
	}

	bool open()
	{
		if(VTCompressionSessionCreate(kCFAllocatorDefault, kW, kH, kCMVideoCodecType_HEVC, nullptr, nullptr, nullptr,
				&Encoder::output, this, &session) != noErr)
			return false;
		VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
		VTSessionSetProperty(session, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
		if(VTSessionSetProperty(session, kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_HEVC_Main10_AutoLevel) != noErr)
			return false;
		VTSessionSetProperty(session, kVTCompressionPropertyKey_AverageBitRate, (__bridge CFNumberRef) @(8000000));
		VTSessionSetProperty(session, kVTCompressionPropertyKey_MaxKeyFrameInterval, (__bridge CFNumberRef) @(100000));
		VTSessionSetProperty(session, kVTCompressionPropertyKey_ColorPrimaries, kCVImageBufferColorPrimaries_ITU_R_2020);
		VTSessionSetProperty(session, kVTCompressionPropertyKey_TransferFunction, kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ);
		VTSessionSetProperty(session, kVTCompressionPropertyKey_YCbCrMatrix, kCVImageBufferYCbCrMatrix_ITU_R_2020);
		return VTCompressionSessionPrepareToEncodeFrames(session) == noErr;
	}

	// One more frame of the stream: two flat halves, 10-bit limited-range luma, the left
	// one in colour and the right one grey.
	bool encode(int leftLuma, int rightLuma)
	{
		CVPixelBufferRef buffer = nullptr;
		NSDictionary *attrs = @{(__bridge NSString *)kCVPixelBufferIOSurfacePropertiesKey : @{}};
		if(CVPixelBufferCreate(kCFAllocatorDefault, kW, kH, kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange,
				(__bridge CFDictionaryRef)attrs, &buffer) != kCVReturnSuccess)
			return false;
		CVPixelBufferLockBaseAddress(buffer, 0);
		// 10 bits in the high bits of 16.
		uint8_t *luma = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(buffer, 0);
		size_t lumaStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 0);
		for(int y = 0; y < kH; y++)
		{
			uint16_t *row = (uint16_t *)(luma + y * lumaStride);
			for(int x = 0; x < kW; x++)
				row[x] = (uint16_t)((x < kW / 2 ? leftLuma : rightLuma) << 6);
		}
		uint8_t *chroma = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(buffer, 1);
		size_t chromaStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 1);
		for(int y = 0; y < kH / 2; y++)
		{
			uint16_t *row = (uint16_t *)(chroma + y * chromaStride);
			for(int x = 0; x < kW; x += 2)
			{
				row[x] = (uint16_t)((x < kW / 2 ? kLeftCb : 512) << 6);
				row[x + 1] = (uint16_t)((x < kW / 2 ? kLeftCr : 512) << 6);
			}
		}
		CVPixelBufferUnlockBaseAddress(buffer, 0);

		frame.clear();
		OSStatus status = VTCompressionSessionEncodeFrame(session, buffer, CMTimeMake(count++, 60), kCMTimeInvalid,
				nullptr, nullptr, nullptr);
		CVPixelBufferRelease(buffer);
		if(status != noErr || VTCompressionSessionCompleteFrames(session, kCMTimeInvalid) != noErr)
			return false;
		return !failed && !frame.empty() && !params.empty();
	}

	~Encoder()
	{
		if(session)
		{
			VTCompressionSessionInvalidate(session);
			CFRelease(session);
		}
	}
};

// ---- the enhancer ---------------------------------------------------------------

// Takes one frame per permission (`accept`), remembers what the middle of each half
// held, and returns the frame with kRed and kGreen added, clamped to 0...1 as a network's
// picture is.
class FakeEnhancer : public FrameEnhancer {
public:
	std::atomic<bool> accept{true};
	std::atomic<int> taken{0}, width{0}, height{0};
	std::atomic<OSType> format{0};
	std::mutex m;
	Rgb left = {}, right = {};  // guarded by m

	bool submit(CVPixelBufferRef frame, uint64_t, bool, Done done) override
	{
		if(!accept.exchange(false))
			return false;
		taken++;
		width = (int)CVPixelBufferGetWidth(frame);
		height = (int)CVPixelBufferGetHeight(frame);
		format = CVPixelBufferGetPixelFormatType(frame);
		if(format != kCVPixelFormatType_64RGBAHalf)
			return false;
		CVPixelBufferRetain(frame);
		dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
			CVPixelBufferRef out = changed(frame);
			done(out, 1.0);
			if(out)
				CVPixelBufferRelease(out);
			CVPixelBufferRelease(frame);
		});
		return true;
	}

	std::string describe() const override { return "fake: red -0.05, green +0.08"; }

private:
	CVPixelBufferRef changed(CVPixelBufferRef in)
	{
		size_t w = CVPixelBufferGetWidth(in), h = CVPixelBufferGetHeight(in);
		CVPixelBufferRef out = nullptr;
		NSDictionary *attrs = @{
			(__bridge NSString *)kCVPixelBufferIOSurfacePropertiesKey : @{},
			(__bridge NSString *)kCVPixelBufferMetalCompatibilityKey : @YES,
		};
		if(CVPixelBufferCreate(kCFAllocatorDefault, w, h, kCVPixelFormatType_64RGBAHalf, (__bridge CFDictionaryRef)attrs, &out)
			!= kCVReturnSuccess)
			return nullptr;
		CVPixelBufferLockBaseAddress(in, kCVPixelBufferLock_ReadOnly);
		CVPixelBufferLockBaseAddress(out, 0);
		const uint8_t *src = (const uint8_t *)CVPixelBufferGetBaseAddress(in);
		uint8_t *dst = (uint8_t *)CVPixelBufferGetBaseAddress(out);
		size_t srcStride = CVPixelBufferGetBytesPerRow(in), dstStride = CVPixelBufferGetBytesPerRow(out);
		for(size_t y = 0; y < h; y++)
		{
			const __fp16 *s = (const __fp16 *)(src + y * srcStride);
			__fp16 *d = (__fp16 *)(dst + y * dstStride);
			for(size_t x = 0; x < w; x++)
			{
				d[x * 4] = (__fp16)clamp01((double)s[x * 4] + kRed);
				d[x * 4 + 1] = (__fp16)clamp01((double)s[x * 4 + 1] + kGreen);
				d[x * 4 + 2] = s[x * 4 + 2];
				d[x * 4 + 3] = (__fp16)1.0f;
			}
		}
		auto at = [&](size_t x) {
			const __fp16 *s = (const __fp16 *)(src + (h / 2) * srcStride) + x * 4;
			return Rgb{(double)s[0], (double)s[1], (double)s[2]};
		};
		{
			std::lock_guard<std::mutex> lk(m);
			left = at(w / 4);
			right = at(w * 3 / 4);
		}
		CVPixelBufferUnlockBaseAddress(out, 0);
		CVPixelBufferUnlockBaseAddress(in, kCVPixelBufferLock_ReadOnly);
		return out;
	}
};

bool waitFor(const std::function<bool()> &done, double seconds = 10.0)
{
	double until = hostTimeUs() + seconds * 1e6;
	while(hostTimeUs() < until)
	{
		if(done())
			return true;
		usleep(1000);
	}
	return done();
}

}  // namespace

int main()
{
	@autoreleasepool
	{
		// Below the knee of the rendition (0.6 of SDR white is 122 nits), a highlight above
		// it, and a picture far from both for the half that changes.
		const int lumaLow = lumaFor(50), lumaHigh = lumaFor(300), lumaOther = lumaFor(8);
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		Encoder encoder;
		if(!device || !encoder.open() || !encoder.encode(lumaLow, lumaHigh))
		{
			printf("skipped: needs a Metal device and the hardware HEVC Main 10 encoder\n");
			return 77;
		}

		CAMetalLayer *layer = [CAMetalLayer layer];
		layer.device = device;
		PipelineConfig config;
		config.codec = Codec::HEVC;
		config.offscreenWidth = kW;
		config.offscreenHeight = kH;
		// A PQ target that holds the decoder's code values as they are: what is read back
		// is what the mix made of them.
		config.hdrOutput = HdrOutput::PQ;
		config.hdrToneMap = HdrToneMap::System;
		std::string error;
		auto pipeline = VideoPipeline::create((__bridge void *)layer, config,
				[](const Error &e) { fprintf(stderr, "pipeline error: %s\n", e.message.c_str()); }, &error);
		if(!pipeline)
		{
			fprintf(stderr, "VideoPipeline::create: %s\n", error.c_str());
			return 1;
		}
		auto fake = std::make_shared<FakeEnhancer>();
		pipeline->setEnhancer(fake);
		EnhanceConfig enhance;
		enhance.maxAgeMs = 600000;  // the test is about what is mixed in, not how fast the machine is
		pipeline->setEnhanceConfig(enhance);

		uint16_t index = 0;
		auto show = [&](int rightLuma) {
			if(index > 0 && !encoder.encode(lumaLow, rightLuma))
				return false;
			if(index == 0)
				pipeline->submitConfig(encoder.params.data(), encoder.params.size());
			uint64_t before = pipeline->stats().framesRendered;
			pipeline->submitFrame(encoder.frame.data(), encoder.frame.size(), index++, 0, false);
			return waitFor([&] { return pipeline->stats().framesRendered > before; });
		};
		auto pixel = [&](int x) {
			float rgba[4] = {-1, -1, -1, -1};
			pipeline->debugReadPixel(x, kH / 2, rgba);
			return Rgb{rgba[0], rgba[1], rgba[2]};
		};
		// Unchanged: the shader returns such a pixel's code values as they are, but two
		// frames of the same picture may come out of the encoder one 10-bit step apart.
		auto same = [](Rgb a, Rgb b) {
			const double step = 1.5 / 1023.0;
			return std::fabs(a.r - b.r) < step && std::fabs(a.g - b.g) < step && std::fabs(a.b - b.b) < step;
		};
		auto results = [&](uint64_t n) { return waitFor([&] { return pipeline->stats().enhanceCompleted >= n; }); };
		const Rgb low = codeOf(lumaLow, kLeftCb, kLeftCr), high = codeOf(lumaHigh), other = codeOf(lumaOther);
		// The conversions between the primaries are the standard ones, and each other's inverse.
		NEAR(to2020({1, 0, 0}).r, 0.6274, 0.0001);
		NEAR(to2020({1, 0, 0}).g, 0.0691, 0.0001);
		NEAR(to709(to2020({0.2, 0.5, 0.9})).b, 0.9, 0.0001);
		NEAR(to709({1, 1, 1}).r, 1.0, 0.0001);
		// The left half's red is on the shoulder, its green and blue under the knee.
		CHECK(linear709(low).r > 1.0 && linear709(low).g < kKnee && linear709(low).g > 0 && linear709(low).b > 0);
		// A 10-bit target and a stream that went through an encoder.
		const double tol = 0.006;
#define NEAR3(got_, want_, tol_) do { Rgb pg = (got_), pw = (want_); NEAR(pg.r, pw.r, tol_); NEAR(pg.g, pw.g, tol_); NEAR(pg.b, pw.b, tol_); } while(0)

		// Nothing has come back yet: the picture as decoded, an HDR one.
		CHECK(show(lumaHigh));
		Stats first = pipeline->stats();
		CHECK(first.hdr);
		const Rgb plainLow = pixel(kLeftX), plainHigh = pixel(kRightX);
		NEAR3(plainLow, low, tol);
		NEAR3(plainHigh, high, tol);

		// What the enhancer was given: the SDR rendition, in half float.
		CHECK(results(1));
		CHECK(fake->taken == 1 && fake->format == kCVPixelFormatType_64RGBAHalf);
		CHECK(fake->width == kW && fake->height == kH);
		Rgb givenLow, givenHigh;
		{
			std::lock_guard<std::mutex> lk(fake->m);
			givenLow = fake->left;
			givenHigh = fake->right;
		}
		NEAR3(givenLow, rendition(low), 0.01);
		NEAR3(givenHigh, rendition(high), 0.01);
		// A grey of 300 nits is on the shoulder, under 1.0; one of 40 nits, under the knee,
		// would be the plain gamma of its share of SDR white.
		CHECK(givenHigh.g > 0.85 && givenHigh.g < 0.97);
		NEAR(rendition(codeOf(lumaFor(40))).g, std::pow(40.0 / 203.0, 1.0 / 2.2), 0.005);

		// The same picture again: the change, made to the rendition and taken back to PQ.
		const Rgb changeLow = fakeChange(givenLow), changeHigh = fakeChange(givenHigh);
		CHECK(show(lumaHigh));
		const Rgb mixedLow = pixel(kLeftX), mixedHigh = pixel(kRightX);
		NEAR3(mixedLow, applied(low, changeLow), tol);
		NEAR3(mixedHigh, applied(high, changeHigh), tol);
		// Green went up, in the highlight too, and the highlight stayed one: brighter than
		// it was, by a proportion and not to the top of the scale.
		CHECK(mixedLow.g > plainLow.g + 0.01);
		CHECK(mixedHigh.g > plainHigh.g + 0.005);
		CHECK(pqEotf(mixedHigh.g) * 10000.0 > 300.0 && pqEotf(mixedHigh.g) * 10000.0 < 600.0);

		// The comparison: left of the line the picture keeps its code values.
		enhance.compareSplit = 0.5f;
		pipeline->setEnhanceConfig(enhance);
		CHECK(show(lumaHigh));
		CHECK(same(pixel(kLeftX), plainLow));
		NEAR3(pixel(kRightX), applied(high, changeHigh), tol);
		enhance.compareSplit = 0.0f;
		pipeline->setEnhanceConfig(enhance);

		// The right half is a different picture now: the change stays out of it.
		CHECK(show(lumaOther));
		NEAR3(pixel(kLeftX), applied(low, changeLow), tol);
		const Rgb movedOther = pixel(kRightX);
		NEAR3(movedOther, other, tol);
		enhance.enabled = false;
		pipeline->setEnhanceConfig(enhance);
		CHECK(show(lumaOther));
		CHECK(same(pixel(kRightX), movedOther));
		CHECK(same(pixel(kLeftX), plainLow));
		enhance.enabled = true;
		pipeline->setEnhanceConfig(enhance);
		usleep(300000);  // past the pause that follows a refusal
		fake->accept = true;
		CHECK(show(lumaHigh));
		CHECK(results(2));
		CHECK(show(lumaHigh));
		NEAR3(pixel(kLeftX), applied(low, changeLow), tol);
		NEAR3(pixel(kRightX), applied(high, changeHigh), tol);

		// Half the strength: half the change to the rendition.
		enhance.strength = 0.5f;
		pipeline->setEnhanceConfig(enhance);
		CHECK(show(lumaHigh));
		NEAR3(pixel(kRightX), applied(high, {changeHigh.r / 2, changeHigh.g / 2, 0.0}), tol);
		enhance.strength = 1.0f;
		pipeline->setEnhanceConfig(enhance);

		// Every scaler has its own mixed-in variant; MetalFX mixes in its first pass.
		for(Scaler scaler : {Scaler::Bicubic, Scaler::Lanczos, Scaler::MetalFX, Scaler::Bilinear})
		{
			pipeline->setScaler(scaler);
			CHECK(show(lumaHigh));
			double t = scaler == Scaler::MetalFX ? 0.02 : tol;
			NEAR3(pixel(kLeftX), applied(low, changeLow), t);
			NEAR3(pixel(kRightX), applied(high, changeHigh), t);
		}

		// A linear target: the same picture as light, 1.0 at 100 nits.
		pipeline->setHdrOutput(HdrOutput::ExtendedLinear);
		CHECK(show(lumaHigh));
		{
			const Rgb want = applied(high, changeHigh), got = pixel(kRightX);
			// A step of the PQ code is about 1 % of light up here.
			NEAR(got.r, pqEotf(want.r) * 100.0, 0.04 * got.r);
			NEAR(got.g, pqEotf(want.g) * 100.0, 0.04 * got.g);
			NEAR(got.b, pqEotf(want.b) * 100.0, 0.04 * got.b);
			CHECK(got.g > 3.0);  // above 300 nits: not clipped at SDR white
		}
		// The app's own tone mapping and brightness come after the mix: where nothing is
		// mixed in, the picture is the one without an enhancer.
		pipeline->setHdrOutput(HdrOutput::PQ);
		pipeline->setHdrToneMap(HdrToneMap::App);
		pipeline->setHdrBrightness(1.5f);
		CHECK(show(lumaOther));
		const Rgb appLow = pixel(kLeftX), appOther = pixel(kRightX);
		enhance.enabled = false;
		pipeline->setEnhanceConfig(enhance);
		CHECK(show(lumaOther));
		CHECK(same(pixel(kRightX), appOther));
		CHECK(appLow.g > pixel(kLeftX).g + 0.01);

		// An overlay's white on an HDR target is the display's SDR white: 100 nits in a PQ
		// layer, 1.0 in a linear one.
		{
			const uint8_t white[4 * 4 * 4] = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
				255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
				255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
				255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
			OverlayImage overlay;
			overlay.bgra = white;
			overlay.width = 4;
			overlay.height = 4;
			overlay.bytesPerRow = 16;
			overlay.x = 8;
			overlay.y = 8;
			pipeline->setOverlay(overlay);
			CHECK(show(lumaOther));
			float rgba[4] = {-1, -1, -1, -1};
			pipeline->debugReadPixel(10, 10, rgba);
			NEAR(pqEotf(rgba[0]) * 10000.0, 100.0, 4.0);
			NEAR(pqEotf(rgba[1]) * 10000.0, 100.0, 4.0);
			NEAR(pqEotf(rgba[2]) * 10000.0, 100.0, 4.0);
			pipeline->setHdrOutput(HdrOutput::ExtendedLinear);
			CHECK(show(lumaOther));
			pipeline->debugReadPixel(10, 10, rgba);
			NEAR(rgba[0], 1.0, 0.02);
			NEAR(rgba[1], 1.0, 0.02);
			NEAR(rgba[2], 1.0, 0.02);
			pipeline->setOverlay(OverlayImage());
			pipeline->setHdrOutput(HdrOutput::PQ);
		}

		Stats stats = pipeline->stats();
		// (enhanceFailed also counts the frames the fake refused while it had no permission.)
		CHECK(stats.enhanceOffered == 2 && stats.enhanceCompleted == 2);
		CHECK(stats.framesEnhanced >= 8);
		pipeline.reset();
	}
	if(failures)
	{
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("all tests passed\n");
	return 0;
}
