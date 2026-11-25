// Content insets and the overlay, end to end and without a console: a flat grey H.264
// stream encoded here, decoded and rendered offscreen. Checks where the picture lands when
// something else takes part of the target, and what an overlay leaves in the rendered
// frame: its opaque pixels, its half transparent ones over the picture, nothing beside it.
//
// Needs a GPU and the hardware H.264 encoder; exits 77 (skipped) without them.
#import "plk/VideoPipeline.h"

#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <VideoToolbox/VideoToolbox.h>

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
constexpr int kLuma = 126;  // limited range: R' = G' = B' = (126 - 16) / 219

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
			for(size_t i = 0; i < sets; i++)
			{
				const uint8_t *data = nullptr;
				size_t size = 0;
				CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, i, &data, &size, nullptr, nullptr);
				append(e->params, data, size);
			}
		// AVCC (four-byte lengths) to Annex-B.
		CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
		size_t total = CMBlockBufferGetDataLength(block);
		std::vector<uint8_t> avcc(total);
		if(CMBlockBufferCopyDataBytes(block, 0, total, avcc.data()) != noErr)
		{
			e->failed = true;
			return;
		}
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

	// One more frame of the stream: flat grey.
	bool encode()
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
			std::memset(luma + y * lumaStride, kLuma, kW);
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

struct Rgb { double r, g, b; };

}  // namespace

int main()
{
	@autoreleasepool
	{
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		Encoder encoder;
		if(!device || !encoder.open() || !encoder.encode())
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
		std::string error;
		auto pipeline = VideoPipeline::create((__bridge void *)layer, config,
				[](const Error &e) { fprintf(stderr, "pipeline error: %s\n", e.message.c_str()); }, &error);
		if(!pipeline)
		{
			fprintf(stderr, "VideoPipeline::create: %s\n", error.c_str());
			return 1;
		}
		uint16_t index = 0;
		auto show = [&] {
			if(index > 0 && !encoder.encode())
				return false;
			if(index == 0)
				pipeline->submitConfig(encoder.params.data(), encoder.params.size());
			uint64_t before = pipeline->stats().framesRendered;
			pipeline->submitFrame(encoder.frame.data(), encoder.frame.size(), index++, 0, false);
			return waitFor([&] { return pipeline->stats().framesRendered > before; });
		};
		auto pixel = [&](int x, int y) {
			float rgba[4] = {-1, -1, -1, -1};
			pipeline->debugReadPixel(x, y, rgba);
			return Rgb{rgba[0], rgba[1], rgba[2]};
		};
		const double grey = (kLuma - 16) / 219.0, tol = 0.02;
		auto isGrey = [&](Rgb p) { return std::fabs(p.r - grey) < tol && std::fabs(p.g - grey) < tol && std::fabs(p.b - grey) < tol; };
		auto isBlack = [&](Rgb p) { return p.r < tol && p.g < tol && p.b < tol; };

		// The picture fills a target of its own shape.
		CHECK(show());
		CHECK(isGrey(pixel(4, 4)) && isGrey(pixel(kW - 4, kH - 4)));
		Stats stats = pipeline->stats();
		CHECK(stats.pictureWidth == kW && stats.pictureHeight == kH);

		// An overlay of 32 x 16: the left half opaque red, the right half white at half
		// alpha (premultiplied), eight pixels in from the picture's corner.
		constexpr int kOw = 32, kOh = 16, kOx = 8, kOy = 8;
		std::vector<uint8_t> bytes(kOw * kOh * 4);
		for(int y = 0; y < kOh; y++)
			for(int x = 0; x < kOw; x++)
			{
				uint8_t *p = &bytes[(size_t)(y * kOw + x) * 4];
				if(x < kOw / 2) { p[0] = 0; p[1] = 0; p[2] = 255; p[3] = 255; }
				else { p[0] = 128; p[1] = 128; p[2] = 128; p[3] = 128; }
			}
		OverlayImage overlay;
		overlay.bgra = bytes.data();
		overlay.width = kOw;
		overlay.height = kOh;
		overlay.bytesPerRow = kOw * 4;
		overlay.x = kOx;
		overlay.y = kOy;
		pipeline->setOverlay(overlay);
		CHECK(show());
		Rgb red = pixel(kOx + 4, kOy + 4);
		NEAR(red.r, 1.0, tol);
		NEAR(red.g, 0.0, tol);
		NEAR(red.b, 0.0, tol);
		Rgb half = pixel(kOx + kOw - 4, kOy + 4);
		NEAR(half.g, 128.0 / 255.0 + grey * (1.0 - 128.0 / 255.0), tol);
		// One to one: the pixels around it are the picture's.
		CHECK(isGrey(pixel(kOx - 2, kOy + 4)) && isGrey(pixel(kOx + kOw + 1, kOy + 4)));
		CHECK(isGrey(pixel(kOx + 4, kOy - 2)) && isGrey(pixel(kOx + 4, kOy + kOh + 1)));

		// The right half of the target is taken: the picture is fitted into the left half,
		// and the overlay goes with its corner.
		pipeline->setContentInsets(0, 0, kW / 2, 0);
		CHECK(show());
		stats = pipeline->stats();
		CHECK(stats.pictureWidth == kW / 2 && stats.pictureHeight == kH / 2);
		CHECK(stats.targetWidth == kW && stats.targetHeight == kH);
		const int top = kH / 4;  // 320 x 180, centred in 320 x 360
		CHECK(isGrey(pixel(kW / 4, kH / 2)));
		CHECK(isBlack(pixel(kW * 3 / 4, kH / 2)));  // under the panel
		CHECK(isBlack(pixel(kW / 4, top - 4)) && isBlack(pixel(kW / 4, top + kH / 2 + 4)));
		red = pixel(kOx + 4, top + kOy + 4);
		NEAR(red.r, 1.0, tol);
		NEAR(red.g, 0.0, tol);
		CHECK(isBlack(pixel(kOx + 4, kOy + 4)));  // where it was

		// Insets on every side.
		pipeline->setContentInsets(64, 20, 96, 20);
		CHECK(show());
		stats = pipeline->stats();
		CHECK(stats.pictureWidth == 480 && stats.pictureHeight == 270);
		CHECK(isGrey(pixel(64 + 240, kH / 2)) && isBlack(pixel(32, kH / 2)) && isBlack(pixel(kW - 48, kH / 2)));

		// Insets that leave nothing worth a picture are not used.
		pipeline->setContentInsets(0, 0, kW - 32, 0);
		CHECK(show());
		stats = pipeline->stats();
		CHECK(stats.pictureWidth == kW && stats.pictureHeight == kH);
		pipeline->setContentInsets(0, 0, 0, 0);

		// An overlay that does not fit where it is asked for is left out, not cut.
		overlay.x = kW - 16;
		pipeline->setOverlay(overlay);
		CHECK(show());
		CHECK(isGrey(pixel(kW - 8, kOy + 4)));
		overlay.x = kOx;

		// Gone again.
		pipeline->setOverlay(OverlayImage());
		CHECK(show());
		CHECK(isGrey(pixel(kOx + 4, kOy + 4)));

		// And with every scaler's last pass, the MetalFX one included (it upscales only:
		// a target larger than the stream is not what this test has, so it falls back).
		pipeline->setOverlay(overlay);
		for(Scaler scaler : {Scaler::Bicubic, Scaler::Lanczos, Scaler::MetalFX, Scaler::Bilinear})
		{
			pipeline->setScaler(scaler);
			CHECK(show());
			red = pixel(kOx + 4, kOy + 4);
			NEAR(red.r, 1.0, tol);
			NEAR(red.b, 0.0, tol);
			CHECK(isGrey(pixel(kW / 2, kH / 2)));
		}
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
