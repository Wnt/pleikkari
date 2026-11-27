// The enhancer path of the pipeline, end to end and without a console: an H.264 stream
// encoded here, decoded and rendered offscreen, and a fake enhancer that adds a known
// amount of green to the frame it is given. Checks what reaches the rendered picture:
// all of the change where the picture has not changed since, none of it where it has,
// and nothing when the result is switched off, too old or forgotten.
//
// Needs a GPU and the hardware H.264 encoder; exits 77 (skipped) without them.
#import "plk/FrameEnhancer.h"
#import "plk/VideoPipeline.h"

#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <VideoToolbox/VideoToolbox.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <unistd.h>
#include <vector>

using namespace plk::video;

static int failures = 0;
#define CHECK(c) do { if(!(c)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); failures++; } } while(0)
#define NEAR(a, b, tol) do { double va = (a), vb = (b); if(std::fabs(va - vb) > (tol)) { \
	fprintf(stderr, "%s:%d: NEAR(%s, %s): %.4f vs %.4f\n", __FILE__, __LINE__, #a, #b, va, vb); failures++; } } while(0)

namespace {

constexpr int kW = 640, kH = 360;
// The picture: two flat halves. The left one never changes; the right one is kRightA or
// kRightB (luma code values, limited range), far enough apart to count as a new picture.
constexpr int kLeft = 120, kRightA = 60, kRightB = 200;
constexpr int kGreen = 40;  // what the fake enhancer adds to green, in code values of 255
constexpr int kLeftX = kW / 4, kRightX = kW * 3 / 4;

struct Encoder {
	VTCompressionSessionRef session = nullptr;
	std::vector<uint8_t> params;  // SPS and PPS, Annex-B
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
		if(CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 0, nullptr, nullptr, &sets, &lengthSize) != noErr
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
				CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, i, &data, &size, nullptr, nullptr);
				append(e->params, data, size);
			}
		}
		CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
		size_t total = CMBlockBufferGetDataLength(block);
		std::vector<uint8_t> avcc(total);
		CMBlockBufferCopyDataBytes(block, 0, total, avcc.data());
		for(size_t at = 0; at + 4 <= total;)
		{
			size_t size = ((size_t)avcc[at] << 24) | ((size_t)avcc[at + 1] << 16) | ((size_t)avcc[at + 2] << 8) | avcc[at + 3];
			at += 4;
			if(at + size > total)
				break;
			append(e->frame, avcc.data() + at, size);
			at += size;
		}
	}

	bool open()
	{
		if(VTCompressionSessionCreate(kCFAllocatorDefault, kW, kH, kCMVideoCodecType_H264, nullptr, nullptr, nullptr,
				&Encoder::output, this, &session) != noErr)
			return false;
		VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
		VTSessionSetProperty(session, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
		VTSessionSetProperty(session, kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_Main_AutoLevel);
		VTSessionSetProperty(session, kVTCompressionPropertyKey_AverageBitRate, (__bridge CFNumberRef) @(4000000));
		VTSessionSetProperty(session, kVTCompressionPropertyKey_MaxKeyFrameInterval, (__bridge CFNumberRef) @(100000));
		VTSessionSetProperty(session, kVTCompressionPropertyKey_ColorPrimaries, kCVImageBufferColorPrimaries_ITU_R_709_2);
		VTSessionSetProperty(session, kVTCompressionPropertyKey_TransferFunction, kCVImageBufferTransferFunction_ITU_R_709_2);
		VTSessionSetProperty(session, kVTCompressionPropertyKey_YCbCrMatrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2);
		return VTCompressionSessionPrepareToEncodeFrames(session) == noErr;
	}

	// One more frame of the stream: the left half as always, the right half at `rightLuma`.
	bool encode(int rightLuma)
	{
		CVPixelBufferRef buffer = nullptr;
		NSDictionary *attrs = @{(__bridge NSString *)kCVPixelBufferIOSurfacePropertiesKey : @{}};
		if(CVPixelBufferCreate(kCFAllocatorDefault, kW, kH, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
				(__bridge CFDictionaryRef)attrs, &buffer) != kCVReturnSuccess)
			return false;
		CVPixelBufferLockBaseAddress(buffer, 0);
		uint8_t *luma = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(buffer, 0);
		size_t lumaStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 0);
		for(int y = 0; y < kH; y++)
		{
			std::memset(luma + y * lumaStride, kLeft, kW / 2);
			std::memset(luma + y * lumaStride + kW / 2, rightLuma, kW / 2);
		}
		uint8_t *chroma = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(buffer, 1);
		size_t chromaStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 1);
		for(int y = 0; y < kH / 2; y++)
			std::memset(chroma + y * chromaStride, 128, kW);
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

// Takes one frame per permission (`accept`) and returns it, a little later and from
// another thread, with kGreen added to green; `fail` makes it return no picture.
class FakeEnhancer : public FrameEnhancer {
public:
	std::atomic<bool> accept{true}, fail{false};
	std::atomic<int> taken{0}, resets{0}, refused{0}, width{0}, height{0};

	bool submit(CVPixelBufferRef frame, uint64_t, bool reset, Done done) override
	{
		if(!accept.exchange(false))
		{
			refused++;
			return false;
		}
		taken++;
		if(reset)
			resets++;
		width = (int)CVPixelBufferGetWidth(frame);
		height = (int)CVPixelBufferGetHeight(frame);
		CVPixelBufferRetain(frame);
		bool failing = fail.load();
		dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
			CVPixelBufferRef out = failing ? nullptr : greener(frame);
			done(out, 1.0);
			if(out)
				CVPixelBufferRelease(out);
			CVPixelBufferRelease(frame);
		});
		return true;
	}

	std::string describe() const override { return "fake: green +40"; }

private:
	static CVPixelBufferRef greener(CVPixelBufferRef in)
	{
		size_t w = CVPixelBufferGetWidth(in), h = CVPixelBufferGetHeight(in);
		CVPixelBufferRef out = nullptr;
		NSDictionary *attrs = @{
			(__bridge NSString *)kCVPixelBufferIOSurfacePropertiesKey : @{},
			(__bridge NSString *)kCVPixelBufferMetalCompatibilityKey : @YES,
		};
		if(CVPixelBufferCreate(kCFAllocatorDefault, w, h, kCVPixelFormatType_32BGRA, (__bridge CFDictionaryRef)attrs, &out)
			!= kCVReturnSuccess)
			return nullptr;
		CVPixelBufferLockBaseAddress(in, kCVPixelBufferLock_ReadOnly);
		CVPixelBufferLockBaseAddress(out, 0);
		const uint8_t *src = (const uint8_t *)CVPixelBufferGetBaseAddress(in);
		uint8_t *dst = (uint8_t *)CVPixelBufferGetBaseAddress(out);
		size_t srcStride = CVPixelBufferGetBytesPerRow(in), dstStride = CVPixelBufferGetBytesPerRow(out);
		for(size_t y = 0; y < h; y++)
		{
			const uint8_t *s = src + y * srcStride;
			uint8_t *d = dst + y * dstStride;
			for(size_t x = 0; x < w; x++)
			{
				d[x * 4] = s[x * 4];
				d[x * 4 + 1] = (uint8_t)std::min(255, s[x * 4 + 1] + kGreen);
				d[x * 4 + 2] = s[x * 4 + 2];
				d[x * 4 + 3] = 255;
			}
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
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		Encoder encoder;
		if(!device || !encoder.open() || !encoder.encode(kRightA))
		{
			printf("skipped: needs a Metal device and the hardware H.264 encoder\n");
			return 77;
		}

		CAMetalLayer *layer = [CAMetalLayer layer];
		layer.device = device;
		PipelineConfig config;
		config.codec = Codec::H264;
		config.offscreenWidth = kW;
		config.offscreenHeight = kH;
		// What the two callbacks report: frames that carried a result, and the results.
		struct Reported {
			std::mutex m;
			uint64_t frames = 0, framesWithResult = 0, results = 0, resultsOk = 0;
			EnhanceTiming last;
		};
		auto reported = std::make_shared<Reported>();
		config.onFrame = [reported](const FrameTiming &t) {
			std::lock_guard<std::mutex> lk(reported->m);
			reported->frames++;
			if(t.enhanceAgeMs >= 0)
				reported->framesWithResult++;
		};
		config.onEnhance = [reported](const EnhanceTiming &t) {
			std::lock_guard<std::mutex> lk(reported->m);
			reported->results++;
			if(t.ok)
			{
				reported->resultsOk++;
				reported->last = t;
			}
		};
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
		// These tests show one picture again and again on purpose, and want each offered.
		enhance.skipRepeats = false;
		enhance.maxAgeMs = 600000;  // the test is about what is mixed in, not how fast the machine is
		pipeline->setEnhanceConfig(enhance);

		// Shows one more frame and waits until it is rendered. The first one is already encoded.
		uint16_t index = 0;
		auto show = [&](int rightLuma) {
			if(index > 0 && !encoder.encode(rightLuma))
				return false;
			if(index == 0)
				pipeline->submitConfig(encoder.params.data(), encoder.params.size());
			uint64_t before = pipeline->stats().framesRendered;
			pipeline->submitFrame(encoder.frame.data(), encoder.frame.size(), index++, 0, false);
			return waitFor([&] { return pipeline->stats().framesRendered > before; });
		};
		auto green = [&](int x) {
			float rgba[4] = {-1, -1, -1, -1};
			pipeline->debugReadPixel(x, kH / 2, rgba);
			return (double)rgba[1];
		};
		auto results = [&](uint64_t n) { return waitFor([&] { return pipeline->stats().enhanceCompleted >= n; }); };
		auto code = [](int luma) { return (luma - 16) / 219.0; };  // grey: R' = G' = B' = this
		const double left = code(kLeft), rightA = code(kRightA), rightB = code(kRightB), change = kGreen / 255.0;
		const double tol = 0.02;

		// Nothing has come back yet: the plain picture.
		CHECK(show(kRightA));
		NEAR(green(kLeftX), left, tol);
		NEAR(green(kRightX), rightA, tol);
		CHECK(results(1));
		CHECK(fake->taken == 1 && fake->resets == 1);  // the first frame given continues nothing
		CHECK(fake->width == kW && fake->height == kH);

		// The same picture again: the whole change, everywhere.
		CHECK(show(kRightA));
		NEAR(green(kLeftX), left + change, tol);
		NEAR(green(kRightX), rightA + change, tol);

		// The comparison: left of the line the picture stays as decoded.
		enhance.compareSplit = 0.5f;
		pipeline->setEnhanceConfig(enhance);
		CHECK(show(kRightA));
		NEAR(green(kLeftX), left, tol);
		NEAR(green(kRightX), rightA + change, tol);
		enhance.compareSplit = 0.0f;
		pipeline->setEnhanceConfig(enhance);

		// The right half is a different picture now: the change stays out of it.
		CHECK(show(kRightB));
		NEAR(green(kLeftX), left + change, tol);
		NEAR(green(kRightX), rightB, tol);

		enhance.strength = 0.5f;
		pipeline->setEnhanceConfig(enhance);
		CHECK(show(kRightB));
		NEAR(green(kLeftX), left + change / 2, tol);
		NEAR(green(kRightX), rightB, tol);
		enhance.strength = 1.0f;
		pipeline->setEnhanceConfig(enhance);

		// Every scaler has its own mixed-in variant; MetalFX mixes in its first pass.
		for(Scaler scaler : {Scaler::Bicubic, Scaler::Lanczos, Scaler::MetalFX, Scaler::Bilinear})
		{
			pipeline->setScaler(scaler);
			CHECK(show(kRightB));
			double t = scaler == Scaler::MetalFX ? 0.04 : tol;
			NEAR(green(kLeftX), left + change, t);
			NEAR(green(kRightX), rightB, t);
			if(scaler != Scaler::MetalFX)
				CHECK(pipeline->stats().scaler == toString(scaler));
		}

		// A result older than maxAgeMs is not used.
		enhance.maxAgeMs = 1;
		pipeline->setEnhanceConfig(enhance);
		usleep(5000);
		CHECK(show(kRightB));
		NEAR(green(kLeftX), left, tol);
		enhance.maxAgeMs = 600000;

		// Switched off: the plain picture. Back on: the old result is forgotten, and the next
		// frame given starts a new history.
		enhance.enabled = false;
		pipeline->setEnhanceConfig(enhance);
		CHECK(show(kRightB));
		NEAR(green(kLeftX), left, tol);
		enhance.enabled = true;
		pipeline->setEnhanceConfig(enhance);
		usleep(300000);  // past the pause that follows a refusal
		fake->accept = true;
		CHECK(show(kRightB));
		NEAR(green(kLeftX), left, tol);
		CHECK(results(2));
		CHECK(fake->taken == 2 && fake->resets == 2);
		CHECK(show(kRightB));
		NEAR(green(kLeftX), left + change, tol);
		NEAR(green(kRightX), rightB + change, tol);

		// A smaller picture for the enhancer: its change is scaled back up.
		enhance.processHeight = 320;
		pipeline->setEnhanceConfig(enhance);
		usleep(300000);
		fake->accept = true;
		CHECK(show(kRightB));
		CHECK(results(3));
		CHECK(fake->width == 570 && fake->height == 320);
		CHECK(show(kRightB));
		NEAR(green(kLeftX), left + change, tol);
		NEAR(green(kRightX), rightB + change, tol);
		CHECK(show(kRightA));
		NEAR(green(kLeftX), left + change, tol);
		NEAR(green(kRightX), rightA, tol);

		// An enhancer that returns no picture: counted, and the last good result stays.
		usleep(300000);
		fake->fail = true;
		fake->accept = true;
		uint64_t failedBefore = pipeline->stats().enhanceFailed;
		CHECK(show(kRightA));
		CHECK(waitFor([&] { return pipeline->stats().enhanceFailed > failedBefore && fake->taken == 4; }));
		CHECK(show(kRightB));
		NEAR(green(kLeftX), left + change, tol);

		Stats stats = pipeline->stats();
		CHECK(stats.enhancer == "fake: green +40");
		CHECK(stats.enhanceWidth == 570 && stats.enhanceHeight == 320);
		CHECK(stats.enhanceOffered == 4 && stats.enhanceCompleted == 3);
		CHECK(stats.framesEnhanced >= 9 && stats.framesEnhanced < stats.framesRendered);
		CHECK(stats.enhanceMs.count == 3 && stats.enhanceAgeMs.count == stats.framesEnhanced);
		CHECK(stats.enhanceInputGpuMs.count >= 3);
		CHECK(fake->refused > 0);
		// The callbacks saw the same: every rendered frame, and every frame the enhancer took.
		CHECK(waitFor([&] {
			std::lock_guard<std::mutex> lk(reported->m);
			return reported->frames == stats.framesRendered;
		}));
		{
			std::lock_guard<std::mutex> lk(reported->m);
			CHECK(reported->framesWithResult == stats.framesEnhanced);
			CHECK(reported->results == 4 && reported->resultsOk == 3);
			CHECK(reported->last.width == 570 && reported->last.height == 320);
			CHECK(reported->last.enhancerMs == 1.0 && reported->last.totalMs > 0 && reported->last.inputGpuMs > 0);
		}

		// Detached: the plain picture, and nothing more is offered.
		pipeline->setEnhancer(nullptr);
		fake->accept = true;
		CHECK(show(kRightB));
		NEAR(green(kLeftX), left, tol);
		NEAR(green(kRightX), rightB, tol);
		usleep(50000);
		CHECK(fake->taken == 4);
		CHECK(pipeline->stats().enhancer.empty());

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
