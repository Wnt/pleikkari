// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// plk-panel-render: the pipeline panel (PipelinePanelView), the frame rate overlay
// (FrameHud) and the analysis panel drawn into PNGs from made-up numbers, and the panel's
// mouse handling checked, all without a window, a stream or a screen recording permission.
//
//   plk-panel-render OUTDIR [--picture FILE] [--hdr] [--size WxH]
//       panel-quality.png, panel-colour.png, panel-display.png   the three pages
//       hud.png                                                  the overlay, at 2x
//       window-beside.png, window-under.png                      the whole window as it is
//                                                                laid out with the panel
//                                                                open: the picture (FILE,
//                                                                or a grey), the overlay,
//                                                                the analysis panel
//   plk-panel-render --check
//       clicks on what the panel drew and checks what each click sends; exit 1 on a failure
#import "AnalysisView.h"
#import "PipelinePanelView.h"

#include "FrameHud.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace plk;
using namespace plk::app;

namespace {

int failures = 0;
#define CHECK(c) do { if(!(c)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); failures++; } } while(0)

// Three seconds of a 60 fps stream, neural rendering on after the first, a stall now and then.
PanelState madeUpState(bool hdr, int pictureWidth, int pictureHeight)
{
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
		t.presented = true;
		t.presentedUs = t.gpuEndUs + 4300 + (i % 37 == 0 ? 9000 : i % 11 == 0 ? 2500 : 0);
		t.enhanceAgeMs = on && i > 96 ? 60 + (i % 6) * 17 : -1;
		analysis.onFrame(t);
		if(on && i % 6 == 0)
		{
			video::EnhanceTiming e;
			e.ok = true;
			e.width = 1280;
			e.height = 720;
			e.inputGpuMs = 0.35;
			e.enhancerMs = 93.0 + (i % 4);
			e.totalMs = e.enhancerMs + 1.1;
			analysis.onEnhance(e, at);
		}
	}
	PanelState s;
	s.report.timing = analysis.snapshot(at);
	s.report.info.stream_width = 1920;
	s.report.info.stream_height = 1080;
	s.report.info.nr_width = 1280;
	s.report.info.nr_height = 720;
	s.report.info.picture_width = pictureWidth;
	s.report.info.picture_height = pictureHeight;
	s.report.info.target_width = 2880;
	s.report.info.target_height = 1620;
	s.report.info.bit_depth = hdr ? 10 : 8;
	s.report.info.hdr = hdr;
	s.report.info.codec = "hevc";
	s.report.info.scaler = "lanczos";
	s.report.info.pacing = "immediate";
	s.report.signal = { { "Stream", hdr ? "1920x1080 hevc 10 bit HDR at 60.0 fps" : "1920x1080 hevc 8 bit SDR at 60.0 fps" },
		{ "Output", std::to_string(pictureWidth) + "x" + std::to_string(pictureHeight) + ", lanczos, immediate" },
		{ "Colour", hdr ? "kCGColorSpaceITUR_2100_PQ" : "kCGColorSpaceSRGB" },
		{ "Neural rendering", "performance model, 1280x720, strength 100 %, standard" },
		{ "Order", "decode > scale + mix > display; network beside" } };
	s.report.enhancer = s.report.signal[3].second;
	s.report.status = "Normal. Neural rendering 10 results/s, on 97 % of the frames";
	s.picture_hdr = hdr;
	s.analysis_shown = true;
	Runner::ViewState &v = s.view;
	v.scaler = video::Scaler::Lanczos;
	v.hdr = hdr;
	v.hdr_switchable = true;
	v.nr = true;
	v.nr_available = true;
	v.nr_status = "NeuralRendering-Small.dlssmodel";
	v.fps_overlay = true;
	v.panel_beside = true;
	return s;
}

PaceSnapshot madeUpPace()
{
	PaceSnapshot p;
	p.fps = 59.6;
	p.frame_ms = 16.8;
	p.latency_ms = 21.4;
	p.budget_ms = 1000.0 / 60.0;
	for(int i = 0; i < 120; i++)
		p.intervals_ms.push_back((float)(16.7 + ((i * 7) % 5 - 2) * 0.5 + (i % 37 == 20 ? 17.0 : 0.0) + (i == 95 ? 60.0 : 0.0)));
	return p;
}

NSBitmapImageRep *bitmap(NSSize points, CGFloat scale)
{
	NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
																	 pixelsWide:(NSInteger)(points.width * scale)
																	 pixelsHigh:(NSInteger)(points.height * scale)
																  bitsPerSample:8
																samplesPerPixel:4
																	   hasAlpha:YES
																	   isPlanar:NO
																 colorSpaceName:NSCalibratedRGBColorSpace
																	bytesPerRow:0
																   bitsPerPixel:0];
	rep.size = points;
	return rep;
}

bool writePng(NSBitmapImageRep *rep, NSString *path)
{
	NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
	if(![png writeToFile:path atomically:YES])
	{
		fprintf(stderr, "cannot write %s\n", path.UTF8String);
		return false;
	}
	printf("%s: %.0fx%.0f points\n", path.UTF8String, rep.size.width, rep.size.height);
	return true;
}

// A view as its own bitmap, at 2x.
NSBitmapImageRep *snapshot(NSView *view)
{
	NSBitmapImageRep *rep = bitmap(view.bounds.size, 2);
	[view cacheDisplayInRect:view.bounds toBitmapImageRep:rep];
	return rep;
}

// The overlay's bytes (BGRA, premultiplied, top row first) as an image.
NSImage *hudImage(int scale)
{
	const HudImage hud = renderFrameHud(madeUpPace(), scale);
	if(hud.width <= 0)
		return nil;
	CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
	CFDataRef data = CFDataCreate(nullptr, hud.bgra.data(), (CFIndex)hud.bgra.size());
	CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
	CGImageRef cg = CGImageCreate((size_t)hud.width, (size_t)hud.height, 8, 32, hud.bytes_per_row, space,
			(CGBitmapInfo)kCGImageAlphaPremultipliedFirst | (CGBitmapInfo)kCGBitmapByteOrder32Little, provider, nullptr, false,
			kCGRenderingIntentDefault);
	NSImage *image = cg ? [[NSImage alloc] initWithCGImage:cg size:NSMakeSize(hud.width / (CGFloat)scale, hud.height / (CGFloat)scale)] : nil;
	CGImageRelease(cg);
	CGDataProviderRelease(provider);
	CFRelease(data);
	CGColorSpaceRelease(space);
	return image;
}

// The window as the app lays it out with the panel open (main.mm's layoutPanels).
NSBitmapImageRep *window(NSSize size, bool beside, bool hdr, NSImage *picture)
{
	const CGFloat margin = 12, panelWidth = std::min<CGFloat>([PLKPipelinePanelView preferredWidth], std::floor(size.width * 0.5));
	const CGFloat analysisHeight = [PLKAnalysisView preferredHeight];
	// The free area the picture is fitted into, 16:9.
	const CGFloat freeW = beside ? size.width - panelWidth : size.width;
	const CGFloat freeH = beside ? size.height - analysisHeight - 2 * margin : size.height;
	CGFloat pw = freeW, ph = pw * 9.0 / 16.0;
	if(ph > freeH)
	{
		ph = freeH;
		pw = ph * 16.0 / 9.0;
	}
	// In a canvas with the origin at the bottom left, like the picture's view.
	const NSRect pictureRect = NSMakeRect((freeW - pw) / 2, (beside ? analysisHeight + 2 * margin : 0) + (freeH - ph) / 2, pw, ph);
	PanelState state = madeUpState(hdr, (int)(pw * 2), (int)(ph * 2));
	state.view.panel_beside = beside;

	PLKPipelinePanelView *panel = [[PLKPipelinePanelView alloc] initWithFrame:NSMakeRect(0, 0, panelWidth, size.height)];
	[panel showState:state];
	PLKAnalysisView *analysis = [[PLKAnalysisView alloc]
			initWithFrame:NSMakeRect(0, 0, size.width - panelWidth - 2 * margin, analysisHeight)];
	[analysis showReport:state.report];
	NSBitmapImageRep *panelRep = snapshot(panel), *analysisRep = snapshot(analysis);

	NSBitmapImageRep *out = bitmap(size, 2);
	[NSGraphicsContext saveGraphicsState];
	NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithBitmapImageRep:out];
	[NSColor.blackColor setFill];
	NSRectFill(NSMakeRect(0, 0, size.width, size.height));
	if(picture)
		[picture drawInRect:pictureRect fromRect:NSZeroRect operation:NSCompositingOperationCopy fraction:1.0];
	else
		[[[NSGradient alloc] initWithStartingColor:[NSColor colorWithCalibratedRed:0.30 green:0.36 blue:0.42 alpha:1]
									   endingColor:[NSColor colorWithCalibratedRed:0.62 green:0.66 blue:0.60 alpha:1]]
				drawInRect:pictureRect
					 angle:60];
	NSImage *hud = hudImage(2);
	if(hud)
		[hud drawInRect:NSMakeRect(NSMinX(pictureRect) + kHudMargin, NSMaxY(pictureRect) - kHudMargin - hud.size.height, hud.size.width,
								   hud.size.height)
			   fromRect:NSZeroRect
			  operation:NSCompositingOperationSourceOver
			   fraction:1.0];
	[analysisRep drawInRect:NSMakeRect(margin, margin, analysis.bounds.size.width, analysisHeight)
				   fromRect:NSZeroRect
				  operation:NSCompositingOperationSourceOver
				   fraction:1.0
			 respectFlipped:NO
					  hints:nil];
	[panelRep drawInRect:NSMakeRect(size.width - panelWidth, 0, panelWidth, size.height)
				fromRect:NSZeroRect
			   operation:NSCompositingOperationSourceOver
				fraction:1.0
		  respectFlipped:NO
				   hints:nil];
	[NSGraphicsContext restoreGraphicsState];
	return out;
}

NSPoint centre(NSRect r)
{
	return NSMakePoint(NSMidX(r), NSMidY(r));
}

bool empty(NSRect r)
{
	return r.size.width <= 0 || r.size.height <= 0;
}

int check()
{
	PanelState state = madeUpState(false, 2656, 1494);
	state.view.nr_strength = 120;
	PLKPipelinePanelView *panel = [[PLKPipelinePanelView alloc] initWithFrame:NSMakeRect(0, 0, [PLKPipelinePanelView preferredWidth], 2000)];
	// What the clicks sent, in a box the block and the lambda below can both hold.
	const auto box = std::make_shared<std::vector<PanelAction>>();
	std::vector<PanelAction> &sent = *box;
	panel.onAction = ^(const PanelAction &a) {
		box->push_back(a);
	};
	[panel showState:state];
	[panel layoutNow];
	auto click = [&](PanelCommand command, int tag) {
		const NSRect r = [panel rectOfCommand:command tag:tag];
		if(empty(r))
			return false;
		sent.clear();
		return [panel clickAt:centre(r)] && sent.size() == 1 && sent[0].command == command && sent[0].tag == tag;
	};

	// The first page: every setting of the neural rendering and the scaler answers a click
	// with its own command.
	CHECK(panel.page == 0);
	// What comes in is the first card: HDR, the resolution and the bitrate asked for.
	CHECK(click(PanelCommand::ToggleHdr, 0));
	CHECK(click(PanelCommand::SelectResolution, (int)CHIAKI_VIDEO_RESOLUTION_PRESET_720p));
	CHECK(click(PanelCommand::SelectBitrate, 25000));
	CHECK(click(PanelCommand::ToggleNr, 0));
	CHECK(click(PanelCommand::SelectNrNetwork, (int)NrNetwork::HT));
	CHECK(click(PanelCommand::SelectNrHeight, 576));
	CHECK(click(PanelCommand::SelectNrHeight, 0));
	CHECK(click(PanelCommand::StepNrStrength, 1));
	CHECK(click(PanelCommand::StepNrStrength, -1));
	CHECK(click(PanelCommand::StepNrStrength, 0));  // the number itself, while it is not 100 %
	CHECK(click(PanelCommand::SelectNrLook, (int)NrLook::Cinematic));
	CHECK(click(PanelCommand::ToggleNrCompare, 0));
	CHECK(click(PanelCommand::SelectScaler, (int)video::Scaler::MetalFX));
	CHECK(sent.size() == 1 && sent[0].title == "MetalFX upscaler");  // the menu's wording goes with it
	// Nothing of another page is there to be clicked.
	CHECK(empty([panel rectOfCommand:PanelCommand::SelectSdrColor tag:(int)video::SdrColor::Vivid]));
	// Between the controls a click does nothing, and says so.
	sent.clear();
	CHECK(![panel clickAt:NSMakePoint(2, 300)] && sent.empty());

	// A card's head closes and opens its settings.
	CHECK([panel clickAt:NSMakePoint(NSMidX([panel rectOfCardHead:@"scaling"]) - 40, NSMidY([panel rectOfCardHead:@"scaling"]))]);
	[panel layoutNow];
	CHECK(empty([panel rectOfCommand:PanelCommand::SelectScaler tag:(int)video::Scaler::MetalFX]));
	CHECK(!empty([panel rectOfCommand:PanelCommand::SelectNrLook tag:(int)NrLook::Natural]));
	[panel setExpanded:YES card:@"scaling"];
	[panel layoutNow];
	CHECK(!empty([panel rectOfCommand:PanelCommand::SelectScaler tag:(int)video::Scaler::MetalFX]));
	// The switch in a head is the switch, not the head.
	CHECK(click(PanelCommand::ToggleNr, 0));
	[panel layoutNow];
	CHECK(!empty([panel rectOfCommand:PanelCommand::SelectNrLook tag:(int)NrLook::Natural]));

	// Tabs.
	CHECK([panel clickAt:centre([panel rectOfTab:1])] && panel.page == 1);
	[panel layoutNow];
	CHECK(click(PanelCommand::SelectSdrColor, (int)video::SdrColor::Vivid));
	CHECK(empty([panel rectOfCommand:PanelCommand::ToggleHdr tag:0]));  // the stream's own switches are on the first page
	CHECK(click(PanelCommand::SelectHdrOutput, (int)video::HdrOutput::ExtendedLinear));
	CHECK(click(PanelCommand::StepHdrBrightness, 1));
	CHECK(empty([panel rectOfCommand:PanelCommand::StepHdrBrightness tag:0]));  // at 100 %: nothing to go back to
	CHECK(click(PanelCommand::ToggleHdrToneMap, 0));
	CHECK(empty([panel rectOfCommand:PanelCommand::SelectScaler tag:(int)video::Scaler::Bicubic]));
	CHECK([panel clickAt:centre([panel rectOfTab:2])] && panel.page == 2);
	[panel layoutNow];
	CHECK(click(PanelCommand::ToggleFullscreenSync, 0));
	CHECK(click(PanelCommand::ToggleFpsOverlay, 0));
	CHECK(click(PanelCommand::ToggleAnalysis, 0));
	CHECK(click(PanelCommand::SelectPanelPicture, 0));

	// A stage of the processing order shows its card's page.
	CHECK([panel clickAt:centre([panel rectOfStage:@"scaling"])] && panel.page == 0);
	[panel layoutNow];
	CHECK([panel clickAt:centre([panel rectOfStage:@"hdr-picture"])] && panel.page == 1);
	[panel layoutNow];
	CHECK(!empty([panel rectOfCardHead:@"hdr-picture"]));

	// Reset sends what brings the page back: here the strength and the scaler.
	panel.page = 0;
	[panel layoutNow];
	sent.clear();
	CHECK(!empty([panel rectOfReset]) && [panel clickAt:centre([panel rectOfReset])]);
	CHECK(sent.size() == 2);
	CHECK(sent.size() == 2 && sent[0].command == PanelCommand::StepNrStrength && sent[0].tag == 0);
	CHECK(sent.size() == 2 && sent[1].command == PanelCommand::SelectScaler && sent[1].tag == (int)video::Scaler::Bilinear);
	// With everything at its default there is nothing to reset.
	state.view.nr_strength = kNrStrengthDefault;
	state.view.scaler = video::Scaler::Bilinear;
	[panel showState:state];
	[panel layoutNow];
	CHECK(empty([panel rectOfReset]));

	// A switch that cannot be used takes no click.
	state.view.fullscreen_sync_applies = false;
	state.view.hdr_switchable = false;
	[panel showState:state];
	panel.page = 2;
	[panel layoutNow];
	CHECK(empty([panel rectOfCommand:PanelCommand::ToggleFullscreenSync tag:0]));
	panel.page = 1;
	[panel layoutNow];
	CHECK(empty([panel rectOfCommand:PanelCommand::ToggleHdr tag:0]));

	// A window too low for the page: the list scrolls, what is out of sight takes no click,
	// and a stage's click brings its card into sight.
	PLKPipelinePanelView *low = [[PLKPipelinePanelView alloc] initWithFrame:NSMakeRect(0, 0, [PLKPipelinePanelView preferredWidth], 360)];
	[low showState:madeUpState(false, 2656, 1494)];
	[low layoutNow];
	CHECK(empty([low rectOfCommand:PanelCommand::SelectScaler tag:(int)video::Scaler::MetalFX]));
	CHECK([low clickAt:centre([low rectOfStage:@"scaling"])]);
	[low layoutNow];
	CHECK(!empty([low rectOfCommand:PanelCommand::SelectScaler tag:(int)video::Scaler::MetalFX]));
	const NSRect scalingHead = [low rectOfCardHead:@"scaling"];
	CHECK(!empty(scalingHead) && NSMaxY(scalingHead) <= 360);

	if(failures)
	{
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}

} // namespace

int main(int argc, const char *argv[])
{
	@autoreleasepool
	{
		if(argc >= 2 && !strcmp(argv[1], "--check"))
			return check();
		if(argc < 2 || argv[1][0] == '-')
		{
			fprintf(stderr, "usage: plk-panel-render OUTDIR [--picture FILE] [--hdr] [--size WxH]\n       plk-panel-render --check\n");
			return 2;
		}
		NSString *dir = @(argv[1]);
		NSImage *picture = nil;
		bool hdr = false;
		NSSize size = NSMakeSize(1440, 810);
		for(int i = 2; i < argc; i++)
		{
			if(!strcmp(argv[i], "--picture") && i + 1 < argc)
				picture = [[NSImage alloc] initWithContentsOfFile:@(argv[++i])];
			else if(!strcmp(argv[i], "--hdr"))
				hdr = true;
			else if(!strcmp(argv[i], "--size") && i + 1 < argc)
			{
				int w = 0, h = 0;
				if(sscanf(argv[++i], "%dx%d", &w, &h) == 2 && w >= 640 && h >= 360)
					size = NSMakeSize(w, h);
			}
		}
		[NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
		bool ok = true;
		const PanelState state = madeUpState(hdr, 2112, 1188);
		static const char *names[] = { "quality", "colour", "display" };
		for(NSInteger page = 0; page < 3; page++)
		{
			PLKPipelinePanelView *panel = [[PLKPipelinePanelView alloc]
					initWithFrame:NSMakeRect(0, 0, [PLKPipelinePanelView preferredWidth], size.height)];
			[panel showState:state];
			panel.page = page;
			ok = writePng(snapshot(panel), [dir stringByAppendingFormat:@"/panel-%s.png", names[page]]) && ok;
		}
		// The overlay over a mid grey, as it lies over a picture.
		NSImage *hud = hudImage(2);
		if(hud)
		{
			NSBitmapImageRep *rep = bitmap(NSMakeSize(hud.size.width + 24, hud.size.height + 24), 2);
			[NSGraphicsContext saveGraphicsState];
			NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
			[[NSColor colorWithCalibratedRed:0.55 green:0.6 blue:0.58 alpha:1] setFill];
			NSRectFill(NSMakeRect(0, 0, rep.size.width, rep.size.height));
			[hud drawInRect:NSMakeRect(12, 12, hud.size.width, hud.size.height)
				   fromRect:NSZeroRect
				  operation:NSCompositingOperationSourceOver
				   fraction:1.0];
			[NSGraphicsContext restoreGraphicsState];
			ok = writePng(rep, [dir stringByAppendingString:@"/hud.png"]) && ok;
		}
		ok = writePng(window(size, true, hdr, picture), [dir stringByAppendingString:@"/window-beside.png"]) && ok;
		ok = writePng(window(size, false, hdr, picture), [dir stringByAppendingString:@"/window-under.png"]) && ok;
		return ok ? 0 : 1;
	}
}
