// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#import "StreamWindow.h"

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
}

- (instancetype)initWithFrame:(NSRect)frame
{
	if((self = [super initWithFrame:frame]))
	{
		self.wantsLayer = YES;
		self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;
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

// The controller is the input device. Keys: the Video menu's own (1-4, A, V, H, P, L), M opens
// that menu; everything else goes nowhere (and does not beep).
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
	if(event.isARepeat)
		return;
	if([NSApp.mainMenu performKeyEquivalent:event])
		return;
	if([chars.lowercaseString isEqualToString:@"m"])
		[self popUpVideoMenuWithEvent:nil];
}

- (void)rightMouseDown:(NSEvent *)event
{
	[self popUpVideoMenuWithEvent:event];
}

- (void)popUpVideoMenuWithEvent:(NSEvent *)event
{
	NSMenu *menu = PLKMakeVideoMenu();
	NSPoint at;
	if(event)
		at = [self convertPoint:event.locationInWindow fromView:nil];
	else
	{
		// From the keyboard: in the middle of the picture, top edge a little above it.
		const NSSize size = menu.size;
		at = NSMakePoint(NSMidX(self.bounds) - size.width / 2, NSMidY(self.bounds) + size.height / 2);
	}
	// The view's cursor is blank; the menu needs one.
	[NSCursor.arrowCursor push];
	[NSCursor unhide];
	[menu popUpMenuPositioningItem:nil atLocation:at inView:self];
	[NSCursor pop];
	[self.window invalidateCursorRectsForView:self];
}

- (void)showNotice:(NSString *)text
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
	_noticeBox.frame = NSMakeRect(round(NSMidX(self.bounds) - box.width / 2), round(self.bounds.size.height * 0.08),
			box.width, box.height);
	_noticeBox.autoresizingMask = NSViewMinXMargin | NSViewMaxXMargin | NSViewMaxYMargin;
	if(_noticeBox.superview != self)
		[self addSubview:_noticeBox];
	// Off the picture again afterwards: nothing but the video layer is composited then.
	const NSUInteger serial = ++_noticeSerial;
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2.0 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
		if(serial == self->_noticeSerial)
			[self->_noticeBox removeFromSuperview];
	});
}

- (void)keyUp:(NSEvent *)event
{
}

- (void)resetCursorRects
{
	[self addCursorRect:self.bounds cursor:blankCursor()];
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

} // namespace

NSMenu *PLKMakeVideoMenu(void)
{
	using namespace plk::video;
	NSMenu *menu = [[NSMenu alloc] initWithTitle:@"Video"];
	addHeader(menu, @"Scaling");
	addChoice(menu, @"Bilinear (fastest)", @selector(plkSelectScaler:), (NSInteger)Scaler::Bilinear, @"1");
	addChoice(menu, @"Bicubic", @selector(plkSelectScaler:), (NSInteger)Scaler::Bicubic, @"2");
	addChoice(menu, @"Lanczos (sharpest)", @selector(plkSelectScaler:), (NSInteger)Scaler::Lanczos, @"3");
	addChoice(menu, @"MetalFX upscaler", @selector(plkSelectScaler:), (NSInteger)Scaler::MetalFX, @"4");
	addHeader(menu, @"Colour of an SDR picture");
	addChoice(menu, @"Accurate", @selector(plkSelectSdrColor:), (NSInteger)SdrColor::Accurate, @"a");
	addChoice(menu, @"Vivid (the display's full gamut)", @selector(plkSelectSdrColor:), (NSInteger)SdrColor::Vivid, @"v");
	addHeader(menu, @"HDR");
	addChoice(menu, @"HDR stream (reconnects)", @selector(plkToggleHdr:), 0, @"h");
	addChoice(menu, @"Output as PQ, tone mapped by macOS", @selector(plkSelectHdrOutput:), (NSInteger)HdrOutput::PQ, @"p");
	addChoice(menu, @"Output as linear light", @selector(plkSelectHdrOutput:), (NSInteger)HdrOutput::ExtendedLinear, @"l");
	// The first one's title carries the current value (the app delegate sets it).
	addChoice(menu, @"Brightness as the console sends it (100 %)", @selector(plkStepHdrBrightness:), 0, @"0");
	addChoice(menu, @"Brighter", @selector(plkStepHdrBrightness:), 1, @"+");
	addChoice(menu, @"Darker", @selector(plkStepHdrBrightness:), -1, @"-");
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
