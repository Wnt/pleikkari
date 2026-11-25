// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// The analysis panel: a strip over the bottom of the picture that shows, about four times a
// second, what each stage of a frame's way costs (FrameAnalysis.h). Three cards:
//
//   Signal            what comes in, what goes out, what the neural rendering works with
//   Stages            a bar per stage, scaled to the stream's frame interval (the budget):
//                     the frame's own way on the left; the neural rendering, which runs
//                     beside it, and the two sums on the right. While neural rendering is
//                     on, a stage also shows how far it is from its value without it.
//   Frame rate        frames on glass per second and the 1 % low, the last present
//                     intervals against the budget, three gauges and a status line
//
// The panel is a view over the Metal layer, so while it is shown the picture is composited
// with it and a fullscreen picture does not go straight to the display: it is a measuring
// tool, switched on with I and off again.

#import <AppKit/AppKit.h>

#include "FrameAnalysis.h"

NS_ASSUME_NONNULL_BEGIN

@interface PLKAnalysisView : NSView
/** The height the panel wants, in points. */
+ (CGFloat)preferredHeight;
- (void)showReport:(const plk::app::AnalysisReport &)report;
@end

NS_ASSUME_NONNULL_END
