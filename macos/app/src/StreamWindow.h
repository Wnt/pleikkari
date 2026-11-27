// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// The stream window: one resizable window whose content view is backed by the CAMetalLayer
// the video pipeline renders into. The pipeline letterboxes (aspect fit, black bars) into
// whatever drawable size the view reports; the view reports its size in pixels whenever its
// bounds or backing scale change. The cursor is hidden over the video while it rests (a blank
// cursor rect); when it moves it is shown, together with a bar of buttons that open the
// settings. Cmd-F toggles native fullscreen.
//
// The Video menu changes the picture while the stream runs: scaling, the colour of an SDR
// picture, HDR on or off, HDR output. It is in the menu bar only, and each choice has a
// key of its own without a modifier: the DualSense is the input device, so the keyboard is
// free. Other key presses are swallowed (no beep). The same choices, laid out as the
// pipeline they change, are in the pipeline panel (G; PipelinePanelView.h).

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

NS_ASSUME_NONNULL_BEGIN

@interface PLKVideoView : NSView
@property(nonatomic, readonly) CAMetalLayer *metalLayer;
/** Called on the main thread with the drawable size in pixels. */
@property(nonatomic, copy, nullable) void (^onDrawableSize)(int width, int height);
- (void)reportDrawableSize;
/** A line of text over the picture for two seconds: what a key press just changed. */
- (void)showNotice:(NSString *)text;
/** seconds 0: stays until the next notice. */
- (void)showNotice:(NSString *)text seconds:(double)seconds;
/** Points at the bottom of the view that something else covers (the analysis panel): notices go above. */
@property(nonatomic) CGFloat bottomInset;
/**
 * Points at the right of the view that the pipeline panel covers: notices and the menu
 * opened from the keyboard are centred in the rest, and the pointer is not hidden there.
 */
@property(nonatomic) CGFloat rightInset;
/**
 * The pointer moved over the picture: it is shown, and with it a bar over the picture's top
 * right corner with the ways into the settings (the pipeline panel, fullscreen). Both go away when the pointer has rested for 2.5 s off the bar and the panel.
 */
- (void)pointerMoved;
/**
 * The comparison line (neural rendering's Compare): where it is in the view, in points from
 * the left, or a negative number when there is none. While there is one, a press or a drag
 * on the picture moves it: onCompareDrag gets the pointer's x in the view.
 */
@property(nonatomic) CGFloat compareLineX;
@property(nonatomic, copy, nullable) void (^onCompareDrag)(CGFloat x);
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
/** Between the app's tone mapping and macOS's. */
- (void)plkToggleHdrToneMap:(id)sender;
/** Frames of a fullscreen picture in step with the display's refresh, or shown at once. */
- (void)plkToggleFullscreenSync:(id)sender;
- (void)plkToggleHdr:(id)sender;
// Neural rendering (NeuralEnhancer.h).
- (void)plkToggleNr:(id)sender;
/** The sender's tag: a plk::app::NrNetwork. */
- (void)plkSelectNrNetwork:(id)sender;
/** The sender's tag: the height of the picture the network is given, 0 for the stream's own. */
- (void)plkSelectNrHeight:(id)sender;
/** The sender's tag: +1 stronger, -1 weaker, 0 back to 100 %. */
- (void)plkStepNrStrength:(id)sender;
/** The sender's tag: a plk::app::NrLook, or -1 for the next one. */
- (void)plkSelectNrLook:(id)sender;
/** The left half of the picture as decoded, the right half with neural rendering. */
- (void)plkToggleNrCompare:(id)sender;
/** The sender's tag: the ChiakiVideoResolutionPreset the console is asked for. Reconnects. */
- (void)plkSelectResolution:(id)sender;
/** The sender's tag: the bitrate the console is asked for, in kbit/s. Reconnects. */
- (void)plkSelectBitrate:(id)sender;
/** The picture moved by what the right stick has asked for and the console has not shown yet, or not. */
- (void)plkToggleCameraWarp:(id)sender;
/** The sender's tag: 2 or 4 times the game's pictures. */
- (void)plkSelectFrameGenerationFactor:(id)sender;
/** The picture between two of the game's generated and shown, or not. */
- (void)plkToggleFrameGeneration:(id)sender;
/** The sender's tag: a plk::app::NrTiming, or -1 for the next one. */
- (void)plkSelectNrTiming:(id)sender;
/** The analysis panel (AnalysisView.h). */
- (void)plkToggleAnalysis:(id)sender;
/** The pipeline panel (PipelinePanelView.h). */
- (void)plkTogglePanel:(id)sender;
/** The frame rate over the picture's corner (FrameHud.h). */
- (void)plkToggleFpsOverlay:(id)sender;
@end

/** The Video menu of the menu bar: the same choices as the pipeline panel, each with its key. */
NSMenu *PLKMakeVideoMenu(void);

@interface PLKStreamWindow : NSWindow
/** A window sized to `aspect` (width / height) within most of the screen, centred. */
+ (instancetype)streamWindowWithTitle:(NSString *)title aspect:(CGFloat)aspect;
@property(nonatomic, readonly) PLKVideoView *videoView;
@end

/** The main menu: app (Hide, Quit), View (Toggle Full Screen, Cmd-F), Video, Window. */
NSMenu *PLKMakeMainMenu(NSString *appName);

NS_ASSUME_NONNULL_END
