// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// The pipeline panel's view: PipelinePanel.h's model, drawn down the right edge of the
// window and worked with the mouse. From the top:
//
//   tabs              one per page
//   processing order  the stages as chips with arrows between; a click on one shows its card
//   cards             the page's, in a list that scrolls: a click on a card's head opens
//                     and closes its settings, its switch turns the stage on and off
//   footer            the sizes a frame goes through, and "Reset this page"
//
// It draws everything itself (no controls of AppKit's), so it looks the same over a
// picture as beside one and can be drawn without a window, and it keeps no state of the
// run: every click is handed to onAction, and what is shown comes from the next
// showState:. Keys are not its business; they stay with the picture's view.
#import <AppKit/AppKit.h>

#include "PipelinePanel.h"

NS_ASSUME_NONNULL_BEGIN

@interface PLKPipelinePanelView : NSView

/** The width the panel wants, in points. */
+ (CGFloat)preferredWidth;

/** A click on something that changes the run. Called on the main thread. */
@property(nonatomic, copy, nullable) void (^onAction)(const plk::app::PanelAction &action);

- (void)showState:(const plk::app::PanelState &)state;

/** The page shown: 0 to the number of pages less one. */
@property(nonatomic) NSInteger page;
/** Opens or closes a card's settings. */
- (void)setExpanded:(BOOL)expanded card:(NSString *)cardId;
/** Shows the page a card is on and scrolls to the card, opened. */
- (void)locateCard:(NSString *)cardId;

// What a mouse click does, as mouseDown: does it; and for a test, where things are, as of
// the last time the panel was drawn. Points in the view's own coordinates (flipped: the
// origin is the top left corner).
- (BOOL)clickAt:(NSPoint)point;
/** Where a click sends this command with this tag, or NSZeroRect. */
- (NSRect)rectOfCommand:(plk::app::PanelCommand)command tag:(int)tag;
- (NSRect)rectOfTab:(NSInteger)page;
- (NSRect)rectOfStage:(NSString *)cardId;
- (NSRect)rectOfCardHead:(NSString *)cardId;
- (NSRect)rectOfReset;
/** Draws into nothing, so that the rects above are those of the state just shown. */
- (void)layoutNow;

@end

NS_ASSUME_NONNULL_END
