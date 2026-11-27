// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "FrameHud.h"

#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace plk::app {

namespace {

struct Color { double r, g, b, a; };
constexpr Color kLabel = { 0.204, 0.820, 0.478, 1.0 };  // the green of the analysis panel
constexpr Color kText = { 0.949, 0.949, 0.969, 1.0 };
constexpr Color kWarn = { 1.0, 0.702, 0.251, 1.0 };
constexpr Color kBad = { 1.0, 0.365, 0.333, 1.0 };

// The graph's ceiling, in frame intervals of the stream: a frame that took three is a
// stall whatever its length.
constexpr double kGraphIntervals = 3.0;

CTFontRef makeFont(double size, bool bold)
{
	CTFontRef base = CTFontCreateUIFontForLanguage(bold ? kCTFontUIFontEmphasizedSystem : kCTFontUIFontSystem, size, nullptr);
	if(!base)
		return nullptr;
	// Digits of one width, so a changing number does not shake.
	const int type = kNumberSpacingType, selector = kMonospacedNumbersSelector;
	CFNumberRef type_number = CFNumberCreate(nullptr, kCFNumberIntType, &type);
	CFNumberRef selector_number = CFNumberCreate(nullptr, kCFNumberIntType, &selector);
	CTFontDescriptorRef plain = CTFontCopyFontDescriptor(base);
	CTFontDescriptorRef tabular = CTFontDescriptorCreateCopyWithFeature(plain, type_number, selector_number);
	CTFontRef font = CTFontCreateWithFontDescriptor(tabular, size, nullptr);
	CFRelease(tabular);
	CFRelease(plain);
	CFRelease(selector_number);
	CFRelease(type_number);
	if(!font)
		return base;
	CFRelease(base);
	return font;
}

CTLineRef makeLine(const char *text, CTFontRef font, CGColorSpaceRef space, Color color)
{
	const CGFloat components[4] = { color.r, color.g, color.b, color.a };
	CGColorRef cg = CGColorCreate(space, components);
	CFStringRef string = CFStringCreateWithCString(nullptr, text, kCFStringEncodingUTF8);
	const void *keys[2] = { kCTFontAttributeName, kCTForegroundColorAttributeName };
	const void *values[2] = { font, cg };
	CFDictionaryRef attributes = CFDictionaryCreate(nullptr, keys, values, 2, &kCFTypeDictionaryKeyCallBacks,
			&kCFTypeDictionaryValueCallBacks);
	CFAttributedStringRef attributed = CFAttributedStringCreate(nullptr, string, attributes);
	CTLineRef line = CTLineCreateWithAttributedString(attributed);
	CFRelease(attributed);
	CFRelease(attributes);
	CFRelease(string);
	CGColorRelease(cg);
	return line;
}

// Text with its baseline `baseline` points below the top; x is its left edge, or its right
// edge with right_aligned. @return its width
double drawText(CGContextRef ctx, const char *text, CTFontRef font, CGColorSpaceRef space, Color color, double x,
		double baseline, bool right_aligned = false)
{
	CTLineRef line = makeLine(text, font, space, color);
	const double width = CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr);
	CGContextSetTextPosition(ctx, right_aligned ? x - width : x, kHudHeight - baseline);
	CTLineDraw(line, ctx);
	CFRelease(line);
	return width;
}

void roundedRect(CGContextRef ctx, CGRect r, double radius)
{
	CGPathRef path = CGPathCreateWithRoundedRect(r, radius, radius, nullptr);
	CGContextAddPath(ctx, path);
	CGPathRelease(path);
}

} // namespace

int paceHealth(const PaceSnapshot &pace)
{
	if(pace.budget_ms <= 0 || pace.fps <= 0)
		return 0;
	const double share = pace.fps * pace.budget_ms / 1000.0;
	return share >= 0.95 ? 0 : share >= 0.8 ? 1 : 2;
}

HudImage renderFrameHud(const PaceSnapshot &pace, int scale)
{
	scale = std::min(std::max(scale, 1), 4);
	HudImage image;
	image.width = kHudWidth * scale;
	image.height = kHudHeight * scale;
	image.bytes_per_row = (size_t)image.width * 4;
	image.bgra.assign(image.bytes_per_row * (size_t)image.height, 0);
	CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
	// The first byte in memory is the top left pixel's blue; the context's y runs up from
	// the bottom, and all coordinates below are in points.
	CGContextRef ctx = CGBitmapContextCreate(image.bgra.data(), (size_t)image.width, (size_t)image.height, 8,
			image.bytes_per_row, space, (uint32_t)kCGImageAlphaPremultipliedFirst | (uint32_t)kCGBitmapByteOrder32Little);
	if(!ctx)
	{
		CGColorSpaceRelease(space);
		return HudImage();
	}
	CGContextScaleCTM(ctx, scale, scale);
	const double w = kHudWidth, h = kHudHeight, pad = 10;

	// A dark plate, so the numbers read over a bright picture.
	CGContextSetRGBFillColor(ctx, 0.04, 0.04, 0.06, 0.58);
	roundedRect(ctx, CGRectMake(0, 0, w, h), 9);
	CGContextFillPath(ctx);

	CTFontRef label = makeFont(10.5, true), small = makeFont(10, true), big = makeFont(23, true), value = makeFont(11, false);
	if(label && small && big && value)
	{
		char text[48];
		const int health = paceHealth(pace);
		drawText(ctx, "FPS", label, space, kLabel, pad, 27);
		if(pace.fps > 0)
			snprintf(text, sizeof(text), "%.0f", pace.fps);
		else
			snprintf(text, sizeof(text), "-");
		drawText(ctx, text, big, space, health == 2 ? kBad : health == 1 ? kWarn : kText, pad + 30, 27);
		drawText(ctx, "Frametime", small, space, kLabel, pad, 45);
		// What a frame takes from the network to the screen, not the time between frames:
		// the frame rate above says that already, and this is what a setting costs.
		if(pace.latency_ms > 0)
			snprintf(text, sizeof(text), pace.latency_ms >= 100 ? "%.0f ms" : "%.1f ms", pace.latency_ms);
		else
			snprintf(text, sizeof(text), "- ms");
		drawText(ctx, text, value, space, kText, w - pad, 45, true);
		// The neural rendering: what a result takes when every frame shown carries its own
		// (that time is in the frame time above), or how far behind the picture its results
		// are when it works beside the stream (it is not).
		drawText(ctx, "Neural", small, space, kLabel, pad, 61);
		Color colour = kText;
		if(pace.nr_result_ms <= 0)
			snprintf(text, sizeof(text), "off");
		else if(pace.nr_age_ms < 1.0)
			snprintf(text, sizeof(text), "%.1f ms", pace.nr_result_ms);
		else
		{
			snprintf(text, sizeof(text), "%.0f ms behind", pace.nr_age_ms);
			colour = kWarn;
		}
		drawText(ctx, text, value, space, colour, w - pad, 61, true);
	}
	for(CTFontRef font : { label, small, big, value })
		if(font)
			CFRelease(font);

	// The last present intervals, the newest at the right, against the stream's own.
	const CGRect graph = CGRectMake(pad, 8, w - 2 * pad, 20);
	CGContextSetRGBFillColor(ctx, 1, 1, 1, 0.07);
	CGContextFillRect(ctx, graph);
	double top = pace.budget_ms > 0 ? pace.budget_ms * kGraphIntervals : 0;
	if(top <= 0)
	{
		for(float v : pace.intervals_ms)
			top = std::max(top, (double)v);
		top = std::max(top * 1.25, 1000.0 / 30.0);
	}
	if(pace.budget_ms > 0)
	{
		const double y = graph.origin.y + graph.size.height * (pace.budget_ms / top);
		const CGFloat dash[2] = { 2, 2 };
		CGContextSetLineDash(ctx, 0, dash, 2);
		CGContextSetLineWidth(ctx, 0.75);
		CGContextSetRGBStrokeColor(ctx, 1, 1, 1, 0.38);
		CGContextMoveToPoint(ctx, graph.origin.x, y);
		CGContextAddLineToPoint(ctx, graph.origin.x + graph.size.width, y);
		CGContextStrokePath(ctx);
		CGContextSetLineDash(ctx, 0, nullptr, 0);
	}
	const size_t n = pace.intervals_ms.size();
	if(n >= 2)
	{
		// The graph holds a fixed number of frames: a short history starts in from the left.
		const size_t slots = std::max<size_t>(n, 120);
		CGContextSaveGState(ctx);
		CGContextClipToRect(ctx, graph);
		for(size_t i = 0; i < n; i++)
		{
			const double v = std::min(top, (double)pace.intervals_ms[i]);
			const double x = graph.origin.x + graph.size.width * ((double)(slots - n + i) / (double)(slots - 1));
			const double y = graph.origin.y + 0.75 + (graph.size.height - 1.5) * (v / top);
			if(i == 0)
				CGContextMoveToPoint(ctx, x, y);
			else
				CGContextAddLineToPoint(ctx, x, y);
		}
		CGContextSetLineWidth(ctx, 1.1);
		CGContextSetLineJoin(ctx, kCGLineJoinRound);
		CGContextSetRGBStrokeColor(ctx, kLabel.r, kLabel.g, kLabel.b, 1.0);
		CGContextStrokePath(ctx);
		CGContextRestoreGState(ctx);
	}
	CGContextRelease(ctx);
	CGColorSpaceRelease(space);
	return image;
}

} // namespace plk::app
