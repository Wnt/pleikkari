// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#import "AnalysisView.h"

#include <algorithm>
#include <cmath>

using plk::app::AnalysisReport;
using plk::app::AnalysisSnapshot;
using plk::app::StageCost;

namespace {

constexpr CGFloat kHeight = 172, kGap = 8, kPad = 12, kRow = 16, kTitle = 26;
// The neural rendering takes several frame intervals for one result: its bars are scaled
// to this many of them, with a tick at each.
constexpr double kNrFrames = 6.0;

NSColor *rgb(int hex, CGFloat alpha = 1.0)
{
	return [NSColor colorWithSRGBRed:((hex >> 16) & 0xff) / 255.0 green:((hex >> 8) & 0xff) / 255.0 blue:(hex & 0xff) / 255.0
							   alpha:alpha];
}

NSColor *stageColor(int stage)
{
	static const int colors[AnalysisSnapshot::kStages] = { 0x8fa3b8, 0x5ac8fa, 0xbf8cff, 0x4f8dff, 0x7d7aff, 0x34d17a, 0x9aa0a6 };
	return rgb(colors[stage]);
}

// A rectangle blended over what is there (NSRectFill copies its colour, alpha and all).
void fillRect(NSRect r)
{
	NSRectFillUsingOperation(r, NSCompositingOperationSourceOver);
}

NSColor *kText() { return rgb(0xf2f2f7); }
NSColor *kDim() { return rgb(0xa0a3ad); }
NSColor *kGood() { return rgb(0x34d17a); }
NSColor *kWarn() { return rgb(0xffb340); }
NSColor *kBad() { return rgb(0xff5d55); }
NSColor *kNr() { return rgb(0xff9f43); }

NSDictionary *attrs(CGFloat size, NSColor *color, NSFontWeight weight = NSFontWeightRegular)
{
	return @{ NSFontAttributeName : [NSFont monospacedDigitSystemFontOfSize:size weight:weight],
			  NSForegroundColorAttributeName : color };
}

// Text with its top left corner at the point (the view is flipped); cut to `width` when given.
void drawLeft(NSString *text, CGFloat x, CGFloat y, NSDictionary *a, CGFloat width = 0)
{
	if(width > 0)
	{
		NSMutableParagraphStyle *style = [NSMutableParagraphStyle new];
		style.lineBreakMode = NSLineBreakByTruncatingTail;
		NSMutableDictionary *cut = [a mutableCopy];
		cut[NSParagraphStyleAttributeName] = style;
		[text drawInRect:NSMakeRect(x, y, width, [text sizeWithAttributes:a].height) withAttributes:cut];
	}
	else
		[text drawAtPoint:NSMakePoint(x, y) withAttributes:a];
}

void drawRight(NSString *text, CGFloat right, CGFloat y, NSDictionary *a)
{
	[text drawAtPoint:NSMakePoint(right - [text sizeWithAttributes:a].width, y) withAttributes:a];
}

void drawCentred(NSString *text, CGFloat centre, CGFloat y, NSDictionary *a)
{
	[text drawAtPoint:NSMakePoint(centre - [text sizeWithAttributes:a].width / 2, y) withAttributes:a];
}

void card(NSRect r, NSString *title, NSString *note)
{
	[rgb(0x14151c, 0.86) setFill];
	[[NSBezierPath bezierPathWithRoundedRect:r xRadius:10 yRadius:10] fill];
	[rgb(0xffffff, 0.10) setStroke];
	NSBezierPath *edge = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(r, 0.5, 0.5) xRadius:10 yRadius:10];
	edge.lineWidth = 1;
	[edge stroke];
	drawLeft(title, r.origin.x + kPad, r.origin.y + 8, attrs(10.5, kDim(), NSFontWeightMedium));
	if(note)
		drawRight(note, NSMaxX(r) - kPad, r.origin.y + 8, attrs(10.5, kDim()));
}

NSString *msText(double ms)
{
	return ms >= 100 ? [NSString stringWithFormat:@"%.0f ms", ms] : ms >= 10 ? [NSString stringWithFormat:@"%.1f ms", ms]
																			 : [NSString stringWithFormat:@"%.2f ms", ms];
}

// One row: name, a bar of `cost` against `scale` (with ticks every `tick` when given), the
// mean, and how far that is from the value without neural rendering.
void stageRow(NSRect row, NSString *name, const StageCost &cost, double scale, double tick, NSColor *color)
{
	const CGFloat x = row.origin.x, right = NSMaxX(row), y = row.origin.y;
	drawLeft(name, x, y, attrs(11, kText()), 86);
	const CGFloat barLeft = x + 90, barRight = right - 112;
	if(barRight - barLeft > 12)
	{
		const NSRect track = NSMakeRect(barLeft, y + 6, barRight - barLeft, 4);
		[rgb(0xffffff, 0.10) setFill];
		[[NSBezierPath bezierPathWithRoundedRect:track xRadius:2 yRadius:2] fill];
		if(cost.samples && scale > 0)
		{
			const double share = std::min(1.0, cost.mean_ms / scale);
			NSRect fill = track;
			fill.size.width = std::max<CGFloat>(2, track.size.width * share);
			[(cost.mean_ms > scale ? kBad() : color) setFill];
			[[NSBezierPath bezierPathWithRoundedRect:fill xRadius:2 yRadius:2] fill];
			// Where 95 % of the frames end: a thin mark.
			const double p95 = std::min(1.0, cost.p95_ms / scale);
			[rgb(0xffffff, 0.55) setFill];
			fillRect(NSMakeRect(track.origin.x + track.size.width * p95 - 0.5, y + 4, 1, 8));
		}
		if(tick > 0 && scale > 0)
		{
			[rgb(0xffffff, 0.28) setFill];
			for(double at = tick; at < scale - 1e-6; at += tick)
				fillRect(NSMakeRect(track.origin.x + track.size.width * (at / scale), y + 5, 1, 6));
		}
	}
	if(!cost.samples)
	{
		drawRight(@"not measured", right, y, attrs(10.5, kDim()));
		return;
	}
	drawRight(msText(cost.mean_ms), right - 46, y, attrs(11, kText()));
	if(cost.baseline_ms >= 0)
	{
		const double delta = cost.mean_ms - cost.baseline_ms;
		// A tenth of a millisecond either way is the noise of two different seconds.
		NSColor *c = delta > 0.1 ? kWarn() : delta < -0.1 ? kGood() : kDim();
		drawRight([NSString stringWithFormat:@"%+.2f", delta], right, y, attrs(10.5, c));
	}
}

// A ring gauge: `share` of the circle (1 = full), the value in the middle, a label below.
void ring(CGFloat cx, CGFloat cy, CGFloat radius, double share, NSColor *color, NSString *value, NSString *label)
{
	NSBezierPath *track = [NSBezierPath bezierPath];
	[track appendBezierPathWithArcWithCenter:NSMakePoint(cx, cy) radius:radius startAngle:0 endAngle:360];
	track.lineWidth = 4;
	[rgb(0xffffff, 0.10) setStroke];
	[track stroke];
	if(share > 0)
	{
		// From twelve o'clock, clockwise as seen (the view is flipped).
		NSBezierPath *arc = [NSBezierPath bezierPath];
		[arc appendBezierPathWithArcWithCenter:NSMakePoint(cx, cy)
										radius:radius
									startAngle:-90
									  endAngle:-90 + 360.0 * std::min(1.0, share)
									 clockwise:NO];
		arc.lineWidth = 4;
		arc.lineCapStyle = NSLineCapStyleRound;
		[color setStroke];
		[arc stroke];
	}
	NSDictionary *a = attrs(10, kText(), NSFontWeightMedium);
	drawCentred(value, cx, cy - [value sizeWithAttributes:a].height / 2, a);
	drawCentred(label, cx, cy + radius + 5, attrs(9.5, kDim()));
}

NSColor *loadColor(double share)
{
	return share > 1.0 ? kBad() : share >= 0.7 ? kWarn() : kGood();
}

} // namespace

@implementation PLKAnalysisView
{
	AnalysisReport _report;
}

+ (CGFloat)preferredHeight
{
	return kHeight;
}

- (instancetype)initWithFrame:(NSRect)frame
{
	if((self = [super initWithFrame:frame]))
		self.wantsLayer = YES;
	return self;
}

- (BOOL)isFlipped
{
	return YES;
}

- (BOOL)isOpaque
{
	return NO;
}

// The picture's own keys and clicks go through the panel.
- (NSView *)hitTest:(NSPoint)point
{
	return nil;
}

- (void)showReport:(const AnalysisReport &)report
{
	_report = report;
	self.needsDisplay = YES;
}

- (void)drawRect:(NSRect)dirty
{
	const AnalysisSnapshot &t = _report.timing;
	const NSRect bounds = self.bounds;
	const double budget = t.budget_ms > 0 ? t.budget_ms : 1000.0 / 60.0;

	// Narrow windows lose the signal card first, then the frame rate card.
	const CGFloat width = bounds.size.width;
	const bool showSignal = width >= 1100, showRate = width >= 780;
	CGFloat x = 0;
	const CGFloat signalW = showSignal ? std::floor(width * 0.24) : 0;
	const CGFloat rateW = showRate ? std::floor(std::max<CGFloat>(390, width * 0.28)) : 0;
	const CGFloat stagesW = width - signalW - rateW - (showSignal ? kGap : 0) - (showRate ? kGap : 0);

	if(showSignal)
	{
		const NSRect r = NSMakeRect(x, 0, signalW, bounds.size.height);
		card(r, @"Signal", nil);
		CGFloat y = kTitle + 2;
		for(const auto &row : _report.signal)
		{
			drawLeft(@(row.first.c_str()), r.origin.x + kPad, y, attrs(10.5, kDim()));
			y += 13;
			drawLeft(@(row.second.c_str()), r.origin.x + kPad, y, attrs(11, kText()), r.size.width - 2 * kPad);
			y += 15;
			if(y > bounds.size.height - 16)
				break;
		}
		x += signalW + kGap;
	}

	{
		const NSRect r = NSMakeRect(x, 0, stagesW, bounds.size.height);
		card(r, @"Stages, average of the last second", [NSString stringWithFormat:@"Budget %.1f ms / frame", budget]);
		const CGFloat colW = std::floor((r.size.width - 2 * kPad - 18) / 2);
		const CGFloat leftX = r.origin.x + kPad, rightX = leftX + colW + 18;
		CGFloat y = kTitle + 4;
		for(int i = 0; i < AnalysisSnapshot::kStages; i++, y += kRow)
			stageRow(NSMakeRect(leftX, y, colW, kRow), @(plk::app::stageName((AnalysisSnapshot::Stage)i)), t.stage[i], budget, 0,
					stageColor(i));
		drawLeft(t.baseline ? @"bar: share of the budget, mark: 95 % of frames, right: change against neural rendering off"
							: t.enhancer_active
								? @"bar: share of the budget, mark: 95 % of frames. Neural rendering off (N) for a second measures the comparison"
								: @"bar: share of the budget, mark: 95 % of frames",
				leftX, bounds.size.height - 20, attrs(9.5, kDim()), r.size.width - 2 * kPad);

		y = kTitle + 4;
		const double nrScale = budget * kNrFrames;
		stageRow(NSMakeRect(rightX, y, colW, kRow), @"NR input pass", t.nr_input, budget, 0, kNr());
		y += kRow;
		stageRow(NSMakeRect(rightX, y, colW, kRow), @"NR network", t.nr_network, nrScale, budget, kNr());
		y += kRow;
		stageRow(NSMakeRect(rightX, y, colW, kRow), @"NR round trip", t.nr_round_trip, nrScale, budget, kNr());
		y += kRow;
		stageRow(NSMakeRect(rightX, y, colW, kRow), @"NR result age", t.nr_age, nrScale, budget, rgb(0xffc56b));
		y += kRow + 6;
		[rgb(0xffffff, 0.10) setFill];
		fillRect(NSMakeRect(rightX, y - 3, colW, 1));
		stageRow(NSMakeRect(rightX, y, colW, kRow), @"Processing", t.processing, budget, 0, rgb(0x4f8dff));
		y += kRow;
		stageRow(NSMakeRect(rightX, y, colW, kRow), @"To glass", t.total, budget * 2, budget, rgb(0xd0d3da));
		x += stagesW + kGap;
	}

	if(showRate)
	{
		const NSRect r = NSMakeRect(x, 0, rateW, bounds.size.height);
		card(r, @"Frame rate and pacing", nil);
		const CGFloat left = r.origin.x + kPad, right = NSMaxX(r) - kPad;
		NSString *fps = t.frames ? [NSString stringWithFormat:@"%.1f", t.fps] : @"-";
		NSDictionary *big = attrs(24, _report.warning ? kWarn() : kGood(), NSFontWeightSemibold);
		drawLeft(fps, left, kTitle - 2, big);
		const CGFloat after = left + [fps sizeWithAttributes:big].width + 8;
		drawLeft(t.offscreen ? @"fps rendered" : @"fps on glass", after, kTitle + 1, attrs(10, kDim()));
		drawLeft(t.low1_fps > 0 ? [NSString stringWithFormat:@"%.1f 1 %% low", t.low1_fps] : @"- 1 % low", after, kTitle + 14,
				attrs(10, kText()));

		// The last present intervals, against the budget (dashed).
		const CGFloat ringStep = 64, ringsW = 3 * ringStep;
		const NSRect graph = NSMakeRect(left, kTitle + 36, std::max<CGFloat>(40, right - left - ringsW - 8), 34);
		[rgb(0xffffff, 0.05) setFill];
		fillRect(graph);
		const double top = budget * 3;  // the graph's ceiling: three frame intervals
		NSBezierPath *dash = [NSBezierPath bezierPath];
		const CGFloat budgetY = NSMaxY(graph) - graph.size.height * (budget / top);
		[dash moveToPoint:NSMakePoint(graph.origin.x, budgetY)];
		[dash lineToPoint:NSMakePoint(NSMaxX(graph), budgetY)];
		const CGFloat pattern[2] = { 3, 3 };
		[dash setLineDash:pattern count:2 phase:0];
		dash.lineWidth = 1;
		[rgb(0xffffff, 0.35) setStroke];
		[dash stroke];
		if(t.intervals_ms.size() >= 2)
		{
			NSBezierPath *line = [NSBezierPath bezierPath];
			const size_t n = t.intervals_ms.size();
			for(size_t i = 0; i < n; i++)
			{
				const double v = std::min(top, (double)t.intervals_ms[i]);
				const NSPoint p = NSMakePoint(graph.origin.x + graph.size.width * ((double)i / (double)(n - 1)),
						NSMaxY(graph) - graph.size.height * (v / top));
				if(i == 0)
					[line moveToPoint:p];
				else
					[line lineToPoint:p];
			}
			line.lineWidth = 1.25;
			[rgb(0x5ac8fa) setStroke];
			[line stroke];
		}

		// Three gauges: the app's own share of the budget, how many frames carry a result,
		// and how old that result is.
		const CGFloat cy = kTitle + 26, radius = 21;
		CGFloat cx = right - ringsW + ringStep / 2;
		const double processing = t.processing.samples ? t.processing.mean_ms / budget : 0;
		ring(cx, cy, radius, processing, loadColor(processing),
				t.processing.samples ? [NSString stringWithFormat:@"%.1f ms", t.processing.mean_ms] : @"-", @"Processing");
		cx += ringStep;
		const double enhanced = t.enhanced_share;
		ring(cx, cy, radius, enhanced, enhanced >= 0.7 ? kGood() : enhanced >= 0.3 ? kWarn() : kDim(),
				t.frames ? [NSString stringWithFormat:@"%.0f %%", enhanced * 100.0] : @"-", @"Enhanced");
		cx += ringStep;
		const double age = t.nr_age.samples ? t.nr_age.mean_ms / (budget * kNrFrames) : 0;
		ring(cx, cy, radius, age, age >= 1.0 ? kBad() : age >= 0.5 ? kWarn() : kGood(),
				t.nr_age.samples ? [NSString stringWithFormat:@"%.0f ms", t.nr_age.mean_ms] : @"-", @"Result age");

		CGFloat y = kTitle + 82;
		drawLeft(@"Neural rendering results", left, y, attrs(10.5, kDim()));
		drawRight(t.enhancer_active ? [NSString stringWithFormat:@"%.0f / s", t.results_per_s] : @"-", right, y, attrs(10.5, kText()));
		y += 14;
		drawLeft(@"Frames not shown", left, y, attrs(10.5, kDim()));
		drawRight([NSString stringWithFormat:@"%llu", (unsigned long long)t.not_presented], right, y,
				attrs(10.5, t.not_presented ? kWarn() : kText()));
		y += 16;
		// The status: a bar in its colour and one line.
		NSColor *statusColor = _report.warning ? kWarn() : kGood();
		[statusColor setFill];
		[[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(left, y + 5, 26, 3) xRadius:1.5 yRadius:1.5] fill];
		drawLeft(@(_report.status.c_str()), left + 34, y, attrs(10.5, statusColor), right - left - 34);
	}
}

@end
