// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#import "StreamWindow.h"

#include "PipelinePanel.h"

#include "Options.h"

#include <plk/VideoPipeline.h>

namespace {

NSCursor *blankCursor()
{
	static NSCursor *cursor = nil;
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		NSImage *image = [[NSImage alloc] initWithSize:NSMakeSize(16, 16)];
		[image lockFocus];
		[[NSColor clearColor] set];
		NSRectFill(NSMakeRect(0, 0, 16, 16));
		[image unlockFocus];
		cursor = [[NSCursor alloc] initWithImage:image hotSpot:NSMakePoint(0, 0)];
	});
	return cursor;
}

} // namespace

@implementation PLKVideoView
{
	int _lastWidth;
	int _lastHeight;
	NSView *_noticeBox;
	NSTextField *_noticeLabel;
	NSUInteger _noticeSerial;
	// The pointer bar: what a moving pointer brings up over the picture's top right corner.
	NSView *_pointerBar;
	NSButton *_panelButton;
	NSTrackingArea *_tracking;
	NSUInteger _pointerSerial;
	BOOL _pointerShown;
}

- (instancetype)initWithFrame:(NSRect)frame
{
	if((self = [super initWithFrame:frame]))
	{
		self.wantsLayer = YES;
		self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;
		_compareLineX = -1;
		self.layer.backgroundColor = NSColor.blackColor.CGColor;
	}
	return self;
}

- (CALayer *)makeBackingLayer
{
	CAMetalLayer *layer = [CAMetalLayer layer];
	layer.backgroundColor = NSColor.blackColor.CGColor;
	return layer;
}

- (CAMetalLayer *)metalLayer
{
	return (CAMetalLayer *)self.layer;
}

- (BOOL)wantsUpdateLayer
{
	return YES;
}

- (BOOL)isOpaque
{
	return YES;
}

- (BOOL)acceptsFirstResponder
{
	return YES;
}

- (BOOL)acceptsFirstMouse:(NSEvent *)event
{
	return YES;
}

// The controller is the input device. Keys: the Video menu's own (1-9, 0, A, V, H, P, L, T, N,
// S, X, Z, E, C, I, G, F), M opens that menu; everything else goes nowhere (and does not beep).
- (void)keyDown:(NSEvent *)event
{
	const NSEventModifierFlags mods = event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask
			& (NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption);
	if(mods)
		return;
	// HDR brightness: + and - wherever the layout has them (= is the unshifted + of a US
	// keyboard), and a held key keeps stepping. Not through the menu's key equivalents,
	// which do not see a held key or a shifted one the same way on every layout.
	NSString *chars = event.charactersIgnoringModifiers;
	NSInteger step = 0;
	if([chars isEqualToString:@"+"] || [chars isEqualToString:@"="])
		step = 1;
	else if([chars isEqualToString:@"-"] || [chars isEqualToString:@"_"])
		step = -1;
	if(step)
	{
		NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:@"" action:@selector(plkStepHdrBrightness:) keyEquivalent:@""];
		item.tag = step;
		[NSApp sendAction:@selector(plkStepHdrBrightness:) to:nil from:item];
		return;
	}
	// Neural rendering strength: X and Z, and a held key keeps stepping as well.
	NSString *lower = chars.lowercaseString;
	const NSInteger strengthStep = [lower isEqualToString:@"x"] ? 1 : [lower isEqualToString:@"z"] ? -1 : 0;
	if(strengthStep)
	{
		NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:@"" action:@selector(plkStepNrStrength:) keyEquivalent:@""];
		item.tag = strengthStep;
		[NSApp sendAction:@selector(plkStepNrStrength:) to:nil from:item];
		return;
	}
	if(event.isARepeat)
		return;
	if([NSApp.mainMenu performKeyEquivalent:event])
		return;
}

// The comparison line follows the pointer while the button is down on the picture.
- (void)mouseDown:(NSEvent *)event
{
	if(_compareLineX < 0 || !self.onCompareDrag)
		return;
	const NSPoint at = [self convertPoint:event.locationInWindow fromView:nil];
	if(at.x <= self.bounds.size.width - _rightInset)
		self.onCompareDrag(at.x);
}

- (void)mouseDragged:(NSEvent *)event
{
	[self mouseDown:event];
	[self pointerMoved];
}

- (void)showNotice:(NSString *)text
{
	[self showNotice:text seconds:2.0];
}

- (void)showNotice:(NSString *)text seconds:(double)seconds
{
	if(!_noticeBox)
	{
		_noticeBox = [[NSView alloc] initWithFrame:NSZeroRect];
		_noticeBox.wantsLayer = YES;
		_noticeBox.layer.backgroundColor = [NSColor colorWithWhite:0.0 alpha:0.72].CGColor;
		_noticeBox.layer.cornerRadius = 10;
		_noticeLabel = [NSTextField labelWithString:@""];
		_noticeLabel.font = [NSFont systemFontOfSize:17 weight:NSFontWeightMedium];
		_noticeLabel.textColor = NSColor.whiteColor;
		[_noticeBox addSubview:_noticeLabel];
	}
	_noticeLabel.stringValue = text;
	[_noticeLabel sizeToFit];
	const NSSize label = _noticeLabel.frame.size;
	const CGFloat padX = 18, padY = 10;
	const NSSize box = NSMakeSize(label.width + 2 * padX, label.height + 2 * padY);
	_noticeLabel.frame = NSMakeRect(padX, padY, label.width, label.height);
	_noticeBox.frame = NSMakeRect(round((self.bounds.size.width - _rightInset) / 2 - box.width / 2),
			round(MAX(self.bounds.size.height * 0.08, _bottomInset + 12)), box.width, box.height);
	_noticeBox.autoresizingMask = NSViewMinXMargin | NSViewMaxXMargin | NSViewMaxYMargin;
	if(_noticeBox.superview != self)
		[self addSubview:_noticeBox];
	// Off the picture again afterwards: nothing but the video layer is composited then.
	const NSUInteger serial = ++_noticeSerial;
	if(seconds <= 0)
		return;
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
		if(serial == self->_noticeSerial)
			[self->_noticeBox removeFromSuperview];
	});
}

- (void)keyUp:(NSEvent *)event
{
}

#pragma mark - Pointer bar

// The keys are not found by someone who was not told about them. A
// pointer that moves over the picture is shown, and with it a bar with the ways in: the
// picture settings (the pipeline panel) and fullscreen. Both go away again when
// the pointer has rested for a moment anywhere but on the bar or the panel.

- (void)updateTrackingAreas
{
	[super updateTrackingAreas];
	if(_tracking)
		[self removeTrackingArea:_tracking];
	_tracking = [[NSTrackingArea alloc] initWithRect:NSZeroRect
											 options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveInActiveApp
													 | NSTrackingInVisibleRect
											   owner:self
											userInfo:nil];
	[self addTrackingArea:_tracking];
}

- (void)mouseMoved:(NSEvent *)event
{
	[self pointerMoved];
}

- (void)mouseExited:(NSEvent *)event
{
	[self hidePointerBar];
}

- (NSButton *)barButton:(NSString *)title symbol:(NSString *)symbol tip:(NSString *)tip action:(SEL)action target:(id)target
{
	NSButton *button = [NSButton buttonWithTitle:title target:target action:action];
	button.bordered = NO;
	button.image = [NSImage imageWithSystemSymbolName:symbol accessibilityDescription:title];
	button.imagePosition = title.length ? NSImageLeading : NSImageOnly;
	button.imageHugsTitle = YES;
	button.contentTintColor = NSColor.whiteColor;
	button.font = [NSFont systemFontOfSize:13 weight:NSFontWeightMedium];
	button.attributedTitle = [[NSAttributedString alloc] initWithString:title
			attributes:@{ NSForegroundColorAttributeName: NSColor.whiteColor, NSFontAttributeName: button.font }];
	button.toolTip = tip;
	button.focusRingType = NSFocusRingTypeNone;
	button.refusesFirstResponder = YES; // the keys stay with the picture
	[button sizeToFit];
	return button;
}

- (void)layoutPointerBar
{
	const CGFloat pad = 6, gap = 4, height = 34;
	NSString *title = _rightInset > 0 ? @"Hide settings" : @"Picture settings";
	_panelButton.attributedTitle = [[NSAttributedString alloc] initWithString:title
			attributes:@{ NSForegroundColorAttributeName: NSColor.whiteColor, NSFontAttributeName: _panelButton.font }];
	CGFloat x = pad;
	for(NSButton *button in _pointerBar.subviews)
	{
		// From the button's own size every time: this runs with every move of the pointer.
		[button sizeToFit];
		const CGFloat w = button.frame.size.width + 14;
		button.frame = NSMakeRect(x, 3, w, height - 6);
		x += w + gap;
	}
	const CGFloat width = x - gap + pad, margin = 14;
	_pointerBar.frame = NSMakeRect(round(self.bounds.size.width - _rightInset - margin - width),
			round(self.bounds.size.height - margin - height), width, height);
}

- (void)pointerMoved
{
	if(!self.window)
		return;
	if(!_pointerBar)
	{
		_pointerBar = [[NSView alloc] initWithFrame:NSZeroRect];
		_pointerBar.wantsLayer = YES;
		_pointerBar.layer.backgroundColor = [NSColor colorWithWhite:0.0 alpha:0.72].CGColor;
		_pointerBar.layer.cornerRadius = 10;
		_pointerBar.autoresizingMask = NSViewMinXMargin | NSViewMinYMargin;
		// No target: the first responder that has the action gets it, as with the menu's items.
		_panelButton = [self barButton:@"Picture settings" symbol:@"slider.horizontal.3"
								   tip:@"Scaling, colour, HDR and neural rendering, laid out as the pipeline (G)"
								action:@selector(plkTogglePanel:) target:nil];
		[_pointerBar addSubview:_panelButton];
		[_pointerBar addSubview:[self barButton:@"" symbol:@"arrow.up.left.and.arrow.down.right" tip:@"Fullscreen (Cmd-F)"
										 action:@selector(toggleFullScreen:) target:nil]];
	}
	[self layoutPointerBar];
	if(_pointerBar.superview != self)
	{
		_pointerBar.alphaValue = 0;
		[self addSubview:_pointerBar];
		_pointerBar.animator.alphaValue = 1;
	}
	if(!_pointerShown)
	{
		_pointerShown = YES;
		[self.window invalidateCursorRectsForView:self];
	}
	const NSUInteger serial = ++_pointerSerial;
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
		if(serial == self->_pointerSerial)
			[self pointerRested];
	});
}

- (void)pointerRested
{
	// Resting on the bar or on the panel is not being done with them.
	if(self.window)
	{
		const NSPoint at = [self convertPoint:self.window.mouseLocationOutsideOfEventStream fromView:nil];
		const BOOL onPanel = _rightInset > 0 && at.x >= self.bounds.size.width - _rightInset;
		if(NSPointInRect(at, self.bounds) && (onPanel || NSPointInRect(at, NSInsetRect(_pointerBar.frame, -8, -8))))
		{
			const NSUInteger serial = ++_pointerSerial;
			dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.0 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
				if(serial == self->_pointerSerial)
					[self pointerRested];
			});
			return;
		}
	}
	[self hidePointerBar];
}

- (void)hidePointerBar
{
	++_pointerSerial;
	// Off the picture, not only invisible: nothing but the video layer is composited then.
	[_pointerBar removeFromSuperview];
	if(_pointerShown)
	{
		_pointerShown = NO;
		[self.window invalidateCursorRectsForView:self];
	}
}

- (void)resetCursorRects
{
	// The pointer is hidden over the picture while it rests (a blank cursor), and shown
	// while it moves.
	if(_pointerShown)
	{
		// Over the comparison line: it can be dragged.
		if(_compareLineX >= 0)
			[self addCursorRect:NSMakeRect(_compareLineX - 8, 0, 16, self.bounds.size.height) cursor:NSCursor.resizeLeftRightCursor];
		return;
	}
	NSRect picture = self.bounds;
	picture.size.width = MAX(0, picture.size.width - _rightInset);
	[self addCursorRect:picture cursor:blankCursor()];
}

- (void)setCompareLineX:(CGFloat)x
{
	if(x == _compareLineX)
		return;
	_compareLineX = x;
	[self.window invalidateCursorRectsForView:self];
}

- (void)setRightInset:(CGFloat)inset
{
	if(inset == _rightInset)
		return;
	_rightInset = inset;
	[self.window invalidateCursorRectsForView:self];
	if(_pointerBar.superview == self)
		[self layoutPointerBar];
}

- (void)setFrameSize:(NSSize)size
{
	[super setFrameSize:size];
	[self reportDrawableSize];
}

- (void)viewDidChangeBackingProperties
{
	[super viewDidChangeBackingProperties];
	self.layer.contentsScale = self.window ? self.window.backingScaleFactor : 2.0;
	[self reportDrawableSize];
}

- (void)viewDidMoveToWindow
{
	[super viewDidMoveToWindow];
	if(self.window)
		self.layer.contentsScale = self.window.backingScaleFactor;
	[self reportDrawableSize];
}

- (void)reportDrawableSize
{
	// Nothing is remembered until someone listens: the first report after the callback is
	// set must always get through.
	if(!self.window || !self.onDrawableSize)
		return;
	const NSSize px = [self convertSizeToBacking:self.bounds.size];
	const int w = (int)lround(px.width), h = (int)lround(px.height);
	if(w <= 0 || h <= 0 || (w == _lastWidth && h == _lastHeight))
		return;
	_lastWidth = w;
	_lastHeight = h;
	self.onDrawableSize(w, h);
}

@end

@implementation PLKStreamWindow
{
	PLKVideoView *_videoView;
}

+ (instancetype)streamWindowWithTitle:(NSString *)title aspect:(CGFloat)aspect
{
	NSScreen *screen = NSScreen.mainScreen;
	const NSRect vf = screen ? screen.visibleFrame : NSMakeRect(0, 0, 1440, 900);
	CGFloat w = vf.size.width * 0.85, h = w / aspect;
	if(h > vf.size.height * 0.85)
	{
		h = vf.size.height * 0.85;
		w = h * aspect;
	}
	const NSRect frame = NSMakeRect(vf.origin.x + (vf.size.width - w) / 2, vf.origin.y + (vf.size.height - h) / 2, w, h);
	PLKStreamWindow *window = [[PLKStreamWindow alloc]
			initWithContentRect:frame
					  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
								| NSWindowStyleMaskResizable
						backing:NSBackingStoreBuffered
						  defer:NO];
	window.title = title;
	window.releasedWhenClosed = NO;
	window.backgroundColor = NSColor.blackColor;
	window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary | NSWindowCollectionBehaviorManaged;
	window.contentMinSize = NSMakeSize(320, 180);
	window.tabbingMode = NSWindowTabbingModeDisallowed;
	window->_videoView = [[PLKVideoView alloc] initWithFrame:NSMakeRect(0, 0, w, h)];
	window->_videoView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
	window.contentView = window->_videoView;
	[window makeFirstResponder:window->_videoView];
	return window;
}

- (PLKVideoView *)videoView
{
	return _videoView;
}

- (BOOL)canBecomeKeyWindow
{
	return YES;
}

@end

namespace {

void addHeader(NSMenu *menu, NSString *title)
{
	if(menu.numberOfItems)
		[menu addItem:NSMenuItem.separatorItem];
	if(@available(macOS 14.0, *))
		[menu addItem:[NSMenuItem sectionHeaderWithTitle:title]];
	else
	{
		NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
		item.enabled = NO;
		[menu addItem:item];
	}
}

// A choice with a key of its own, pressed without a modifier.
void addChoice(NSMenu *menu, NSString *title, SEL action, NSInteger tag, NSString *key)
{
	NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key];
	item.keyEquivalentModifierMask = 0;
	item.tag = tag;
	[menu addItem:item];
}

template <size_t N>
void addChoices(NSMenu *menu, const plk::app::NamedChoice (&choices)[N], SEL action)
{
	for(const plk::app::NamedChoice &c : choices)
		addChoice(menu, @(c.title), action, c.tag, @(c.key));
}

} // namespace

NSMenu *PLKMakeVideoMenu(void)
{
	// The named choices are PipelinePanel.h's tables: the panel offers the same ones.
	using namespace plk::app;
	NSMenu *menu = [[NSMenu alloc] initWithTitle:@"Video"];
	addHeader(menu, @"Scaling");
	addChoices(menu, kScalerChoices, @selector(plkSelectScaler:));
	addHeader(menu, @"Colour of an SDR picture");
	addChoices(menu, kSdrColorChoices, @selector(plkSelectSdrColor:));
	addHeader(menu, @"HDR");
	addChoice(menu, @"HDR stream (reconnects)", @selector(plkToggleHdr:), 0, @"h");
	addChoices(menu, kHdrOutputChoices, @selector(plkSelectHdrOutput:));
	// The first one's title carries the current value (the app delegate sets it).
	addChoice(menu, @"Standard brightness (100 %)", @selector(plkStepHdrBrightness:), 0, @"0");
	addChoice(menu, @"Brighter", @selector(plkStepHdrBrightness:), 1, @"+");
	addChoice(menu, @"Darker", @selector(plkStepHdrBrightness:), -1, @"-");
	addChoice(menu, @"Tone mapping by macOS (stalls in fullscreen)", @selector(plkToggleHdrToneMap:), 0, @"t");
	addHeader(menu, @"Fullscreen");
	addChoice(menu, @"In step with the display: no tearing", @selector(plkToggleFullscreenSync:), 0, @"s");
	// The effect and its settings, in the order they matter: on or off, which model, how
	// much picture it is given, how much of its change is shown, which look.
	addHeader(menu, @"Neural rendering");
	// Its title carries the state (the app delegate sets it).
	addChoice(menu, @"Neural rendering", @selector(plkToggleNr:), 0, @"n");
	addChoices(menu, kNrNetworkChoices, @selector(plkSelectNrNetwork:));
	addChoices(menu, kNrHeightChoices, @selector(plkSelectNrHeight:));
	// The first one's title carries the current value.
	addChoice(menu, @"Strength 100 %", @selector(plkStepNrStrength:), 0, @"");
	addChoice(menu, @"Stronger", @selector(plkStepNrStrength:), 1, @"x");
	addChoice(menu, @"Weaker", @selector(plkStepNrStrength:), -1, @"z");
	addChoices(menu, kNrLookChoices, @selector(plkSelectNrLook:));
	addChoice(menu, @"Next look", @selector(plkSelectNrLook:), -1, @"e");
	addChoices(menu, kNrTimingChoices, @selector(plkSelectNrTiming:));
	addChoice(menu, @"Next timing", @selector(plkSelectNrTiming:), -1, @"w");
	addChoice(menu, @"Compare: left of the line without", @selector(plkToggleNrCompare:), 0, @"c");
	addHeader(menu, @"Stream");
	addChoices(menu, kResolutionChoices, @selector(plkSelectResolution:));
	addChoices(menu, kBitrateChoices, @selector(plkSelectBitrate:));
	addHeader(menu, @"Frame generation");
	addChoice(menu, @"Frame generation: the picture between two (later by about 25 ms)", @selector(plkToggleFrameGeneration:), 0, @"j");
	addChoices(menu, kFrameGenerationChoices, @selector(plkSelectFrameGenerationFactor:));
	addHeader(menu, @"Input lag");
	addChoice(menu, @"Camera warp: the picture follows the right stick at once", @selector(plkToggleCameraWarp:), 0, @"k");
	addHeader(menu, @"Panels");
	addChoice(menu, @"Pipeline panel", @selector(plkTogglePanel:), 0, @"g");
	addChoice(menu, @"Frame rate overlay", @selector(plkToggleFpsOverlay:), 0, @"f");
	addChoice(menu, @"Frame time analysis", @selector(plkToggleAnalysis:), 0, @"i");
	return menu;
}

NSMenu *PLKMakeMainMenu(NSString *appName)
{
	NSMenu *main = [[NSMenu alloc] initWithTitle:@"Main"];

	NSMenuItem *appItem = [[NSMenuItem alloc] initWithTitle:appName action:nil keyEquivalent:@""];
	NSMenu *appMenu = [[NSMenu alloc] initWithTitle:appName];
	[appMenu addItemWithTitle:[@"Hide " stringByAppendingString:appName] action:@selector(hide:) keyEquivalent:@"h"];
	[appMenu addItem:NSMenuItem.separatorItem];
	[appMenu addItemWithTitle:[@"Quit " stringByAppendingString:appName] action:@selector(terminate:) keyEquivalent:@"q"];
	appItem.submenu = appMenu;
	[main addItem:appItem];

	NSMenuItem *viewItem = [[NSMenuItem alloc] initWithTitle:@"View" action:nil keyEquivalent:@""];
	NSMenu *viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
	// Cmd-F (not the system's Ctrl-Cmd-F) toggles fullscreen.
	[viewMenu addItemWithTitle:@"Toggle Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
	viewItem.submenu = viewMenu;
	[main addItem:viewItem];

	NSMenuItem *videoItem = [[NSMenuItem alloc] initWithTitle:@"Video" action:nil keyEquivalent:@""];
	videoItem.submenu = PLKMakeVideoMenu();
	[main addItem:videoItem];

	NSMenuItem *windowItem = [[NSMenuItem alloc] initWithTitle:@"Window" action:nil keyEquivalent:@""];
	NSMenu *windowMenu = [[NSMenu alloc] initWithTitle:@"Window"];
	[windowMenu addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
	[windowMenu addItemWithTitle:@"Close" action:@selector(performClose:) keyEquivalent:@"w"];
	windowItem.submenu = windowMenu;
	[main addItem:windowItem];
	return main;
}
