// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#import "StreamWindow.h"

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

// The controller is the input device: keys and mouse clicks go nowhere (and do not beep).
- (void)keyDown:(NSEvent *)event
{
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

	NSMenuItem *windowItem = [[NSMenuItem alloc] initWithTitle:@"Window" action:nil keyEquivalent:@""];
	NSMenu *windowMenu = [[NSMenu alloc] initWithTitle:@"Window"];
	[windowMenu addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
	[windowMenu addItemWithTitle:@"Close" action:@selector(performClose:) keyEquivalent:@"w"];
	windowItem.submenu = windowMenu;
	[main addItem:windowItem];
	return main;
}
