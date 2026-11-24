// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// The stream window: one resizable window whose content view is backed by the CAMetalLayer
// the video pipeline renders into. The pipeline letterboxes (aspect fit, black bars) into
// whatever drawable size the view reports; the view reports its size in pixels whenever its
// bounds or backing scale change. The cursor is hidden over the video (a blank cursor
// rect), key presses are swallowed (no beep), Cmd-F toggles native fullscreen.

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

NS_ASSUME_NONNULL_BEGIN

@interface PLKVideoView : NSView
@property(nonatomic, readonly) CAMetalLayer *metalLayer;
/** Called on the main thread with the drawable size in pixels. */
@property(nonatomic, copy, nullable) void (^onDrawableSize)(int width, int height);
- (void)reportDrawableSize;
@end

@interface PLKStreamWindow : NSWindow
/** A window sized to `aspect` (width / height) within most of the screen, centred. */
+ (instancetype)streamWindowWithTitle:(NSString *)title aspect:(CGFloat)aspect;
@property(nonatomic, readonly) PLKVideoView *videoView;
@end

/** The main menu: app (Hide, Quit), View (Toggle Full Screen, Cmd-F), Window. */
NSMenu *PLKMakeMainMenu(NSString *appName);

NS_ASSUME_NONNULL_END
