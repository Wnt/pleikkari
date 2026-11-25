// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// plk-analysis-render OUT.png [WIDTH]: the analysis panel (AnalysisView) drawn into a PNG
// from made-up numbers, over a grey that stands for the picture. A look at the panel's
// layout without a window, a stream or a screen recording permission.

#import "AnalysisView.h"

#include <cstdio>
#include <cstdlib>

using namespace plk;
using namespace plk::app;

int main(int argc, const char *argv[])
{
	@autoreleasepool
	{
		if(argc < 2)
		{
			fprintf(stderr, "usage: plk-analysis-render OUT.png [WIDTH]\n");
			return 2;
		}
		const CGFloat width = argc > 2 ? atof(argv[2]) : 1400;

		// Three seconds of a 60 fps stream with neural rendering on after the first second.
		FrameAnalysis analysis;
		double at = 1e6;
		const double frameUs = 1e6 / 60.0;
		for(int i = 0; i < 240; i++, at += frameUs)
		{
			const bool on = i >= 90;
			if(i == 90)
				analysis.setEnhancerActive(true);
			video::FrameTiming t;
			t.readyUs = at - 150;
			t.submitUs = at;
			t.decodedUs = at + 3100 + (i % 7) * 60;
			t.renderStartUs = t.decodedUs + 90;
			t.committedUs = t.renderStartUs + 260;
			t.gpuStartUs = t.committedUs + 80;
			t.gpuEndUs = t.gpuStartUs + (on ? 1750 : 1200) + (i % 5) * 70;
			t.presented = i != 200;
			t.presentedUs = t.presented ? t.gpuEndUs + 4300 + (i % 11 == 0 ? 2500 : 0) : 0;
			t.enhanceAgeMs = on && i > 96 ? 30 + (i % 3) * 17 : -1;
			analysis.onFrame(t);
			if(on && i % 3 == 0)
			{
				video::EnhanceTiming e;
				e.ok = true;
				e.width = 1280;
				e.height = 720;
				e.inputGpuMs = 0.35;
				e.enhancerMs = 43.0 + (i % 4);
				e.totalMs = e.enhancerMs + 1.1;
				analysis.onEnhance(e, at);
			}
		}
		AnalysisReport report;
		report.timing = analysis.snapshot(at);
		report.signal = { { "Stream", "1920x1080 hevc 8 bit SDR at 60.0 fps" },
			{ "Output", "3456x1944, lanczos" },
			{ "Colour", "kCGColorSpaceSRGB" },
			{ "Neural rendering", "performance model, 1280x720, strength 100 %, standard" },
			{ "Order", "decode > scale + mix > display; network beside" } };
		report.enhancer = report.signal[3].second;
		report.status = "Normal. Neural rendering 20 results/s, on 97 % of the frames";

		const CGFloat height = [PLKAnalysisView preferredHeight], margin = 12;
		PLKAnalysisView *view = [[PLKAnalysisView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
		[view showReport:report];
		NSBitmapImageRep *panel = [view bitmapImageRepForCachingDisplayInRect:view.bounds];
		[view cacheDisplayInRect:view.bounds toBitmapImageRep:panel];

		// Onto a background, as it sits over the picture.
		const NSSize size = NSMakeSize(width + 2 * margin, height + 2 * margin);
		NSBitmapImageRep *out = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
																		 pixelsWide:(NSInteger)(size.width * 2)
																		 pixelsHigh:(NSInteger)(size.height * 2)
																	  bitsPerSample:8
																	samplesPerPixel:4
																		   hasAlpha:YES
																		   isPlanar:NO
																	 colorSpaceName:NSCalibratedRGBColorSpace
																		bytesPerRow:0
																	   bitsPerPixel:0];
		out.size = size;
		[NSGraphicsContext saveGraphicsState];
		NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithBitmapImageRep:out];
		[[NSColor colorWithCalibratedRed:0.45 green:0.52 blue:0.48 alpha:1] setFill];
		NSRectFill(NSMakeRect(0, 0, size.width, size.height));
		[panel drawInRect:NSMakeRect(margin, margin, width, height)];
		[NSGraphicsContext restoreGraphicsState];
		NSData *png = [out representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
		if(![png writeToFile:@(argv[1]) atomically:YES])
		{
			fprintf(stderr, "cannot write %s\n", argv[1]);
			return 1;
		}
		printf("%s: %.0fx%.0f points\n", argv[1], size.width, size.height);
		return 0;
	}
}
