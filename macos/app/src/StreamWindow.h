// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// The stream window: one resizable window whose content view is backed by the CAMetalLayer
// the video pipeline renders into. The pipeline letterboxes (aspect fit, black bars) into
// whatever drawable size the view reports; the view reports its size in pixels whenever its
// bounds or backing scale change. The cursor is hidden over the video (a blank cursor
// rect), Cmd-F toggles native fullscreen.
//
// The Video menu changes the picture while the stream runs: scaling, the colour of an SDR
// picture, HDR on or off, HDR output. It is in the menu bar, under a right click on the
// picture and under the M key (arrow keys and Return work in it), and each choice has a
// key of its own without a modifier: the DualSense is the input device, so the keyboard is
// free. Other key presses are swallowed (no beep).

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

NS_ASSUME_NONNULL_BEGIN

@interface PLKVideoView : NSView
@property(nonatomic, readonly) CAMetalLayer *metalLayer;
/** Called on the main thread with the drawable size in pixels. */
@property(nonatomic, copy, nullable) void (^onDrawableSize)(int width, int height);
- (void)reportDrawableSize;
/** Opens the Video menu in the middle of the picture (the M key), or where the event was. */
- (void)popUpVideoMenuWithEvent:(nullable NSEvent *)event;
/** A line of text over the picture for two seconds: what a key press just changed. */
- (void)showNotice:(NSString *)text;
@end

/**
 * The actions of the Video menu. The items have no target: the first object in the responder
 * chain that implements these gets them (the app delegate, which is the window's delegate),
 * and its validateMenuItem: sets the check marks. A sender's tag is the value of the choice
 * (plk::video::Scaler, SdrColor or HdrOutput as an integer).
 */
@protocol PLKVideoMenuActions
- (void)plkSelectScaler:(id)sender;
- (void)plkSelectSdrColor:(id)sender;
- (void)plkSelectHdrOutput:(id)sender;
/** The sender's tag: +1 brighter, -1 darker, 0 back to 100 %. */
- (void)plkStepHdrBrightness:(id)sender;
- (void)plkToggleHdr:(id)sender;
@end

/** A new Video menu (an item belongs to one menu: the menu bar and the pop-up each get their own). */
NSMenu *PLKMakeVideoMenu(void);

@interface PLKStreamWindow : NSWindow
/** A window sized to `aspect` (width / height) within most of the screen, centred. */
+ (instancetype)streamWindowWithTitle:(NSString *)title aspect:(CGFloat)aspect;
@property(nonatomic, readonly) PLKVideoView *videoView;
@end

/** The main menu: app (Hide, Quit), View (Toggle Full Screen, Cmd-F), Video, Window. */
NSMenu *PLKMakeMainMenu(NSString *appName);

NS_ASSUME_NONNULL_END
