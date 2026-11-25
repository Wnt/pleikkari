// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#import "PipelinePanelView.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

using namespace plk::app;

namespace {

constexpr CGFloat kWidth = 384, kPad = 12, kTabsHeight = 30, kHeadHeight = 48, kCardGap = 8, kFooterHeight = 54;
constexpr CGFloat kInner = 12, kRowPad = 9, kSwitchWidth = 34, kSwitchHeight = 20, kControlHeight = 24;
constexpr CGFloat kStepButton = 26, kStepValue = 58;
// A choice sits beside its title while it takes no more than this share of the row.
constexpr CGFloat kInlineShare = 0.56;
constexpr double kFlashSeconds = 1.2;

NSColor *rgb(int hex, CGFloat alpha = 1.0)
{
	return [NSColor colorWithSRGBRed:((hex >> 16) & 0xff) / 255.0 green:((hex >> 8) & 0xff) / 255.0 blue:(hex & 0xff) / 255.0
							   alpha:alpha];
}

NSColor *white(CGFloat alpha) { return rgb(0xffffff, alpha); }
NSColor *kPanelFill() { return rgb(0x14121c, 0.97); }
NSColor *kCardFill() { return rgb(0x1f1c2b); }
NSColor *kText() { return rgb(0xf2f2f7); }
NSColor *kDim() { return rgb(0xa6a6b8); }
NSColor *kFaint() { return rgb(0x77778a); }
NSColor *kGood() { return rgb(0x34d17a); }
NSColor *kWarn() { return rgb(0xffb340); }
NSColor *kAccent() { return rgb(0xff8f6b); }
NSColor *kAccentStart() { return rgb(0xff5f8f); }
NSColor *kAccentEnd() { return rgb(0xffa24a); }

NSDictionary *font(CGFloat size, NSColor *color, NSFontWeight weight = NSFontWeightRegular, bool digits = false)
{
	NSFont *f = digits ? [NSFont monospacedDigitSystemFontOfSize:size weight:weight] : [NSFont systemFontOfSize:size weight:weight];
	return @{ NSFontAttributeName : f, NSForegroundColorAttributeName : color };
}

NSString *str(const std::string &s)
{
	return [NSString stringWithUTF8String:s.c_str()] ?: @"";
}

CGFloat textWidth(NSString *text, NSDictionary *a)
{
	return std::ceil([text sizeWithAttributes:a].width);
}

CGFloat lineHeight(NSDictionary *a)
{
	return std::ceil([@"Ag" sizeWithAttributes:a].height);
}

// Text over several lines within `width`: how tall it is, and the drawing of it.
CGFloat wrappedHeight(NSString *text, NSDictionary *a, CGFloat width)
{
	if(!text.length)
		return 0;
	return std::ceil([text boundingRectWithSize:NSMakeSize(width, CGFLOAT_MAX)
										 options:NSStringDrawingUsesLineFragmentOrigin
									  attributes:a].size.height);
}

void drawWrapped(NSString *text, CGFloat x, CGFloat y, CGFloat width, NSDictionary *a)
{
	[text drawWithRect:NSMakeRect(x, y, width, CGFLOAT_MAX) options:NSStringDrawingUsesLineFragmentOrigin attributes:a];
}

// One line with its top left corner at the point, cut with an ellipsis at `width` when given.
void drawLine(NSString *text, CGFloat x, CGFloat y, NSDictionary *a, CGFloat width = 0)
{
	if(width > 0)
	{
		NSMutableParagraphStyle *style = [NSMutableParagraphStyle new];
		style.lineBreakMode = NSLineBreakByTruncatingTail;
		NSMutableDictionary *cut = [a mutableCopy];
		cut[NSParagraphStyleAttributeName] = style;
		[text drawInRect:NSMakeRect(x, y, width, lineHeight(a)) withAttributes:cut];
	}
	else
		[text drawAtPoint:NSMakePoint(x, y) withAttributes:a];
}

void drawCentred(NSString *text, NSRect r, NSDictionary *a)
{
	const NSSize s = [text sizeWithAttributes:a];
	[text drawAtPoint:NSMakePoint(NSMidX(r) - s.width / 2, NSMidY(r) - s.height / 2) withAttributes:a];
}

void fillRounded(NSRect r, CGFloat radius, NSColor *color)
{
	[color setFill];
	[[NSBezierPath bezierPathWithRoundedRect:r xRadius:radius yRadius:radius] fill];
}

void fillRect(NSRect r, NSColor *color)
{
	[color setFill];
	NSRectFillUsingOperation(r, NSCompositingOperationSourceOver);
}

// An SF Symbol in one colour, centred in the box. The configured images are kept: the
// panel is drawn several times a second.
void drawSymbol(const std::string &name, NSRect box, CGFloat pointSize, int hex)
{
	if(name.empty())
		return;
	static NSMutableDictionary<NSString *, NSImage *> *cache = [NSMutableDictionary new];
	NSString *key = [NSString stringWithFormat:@"%s/%.1f/%06x", name.c_str(), pointSize, hex];
	NSImage *image = cache[key];
	if(!image)
	{
		NSImage *symbol = [NSImage imageWithSystemSymbolName:str(name) accessibilityDescription:nil];
		if(!symbol)
			return;
		NSImageSymbolConfiguration *configuration =
				[[NSImageSymbolConfiguration configurationWithPointSize:pointSize weight:NSFontWeightMedium]
						configurationByApplyingConfiguration:[NSImageSymbolConfiguration configurationWithPaletteColors:@[ rgb(hex) ]]];
		image = [symbol imageWithSymbolConfiguration:configuration] ?: symbol;
		cache[key] = image;
	}
	const NSSize s = image.size;
	[image drawInRect:NSMakeRect(std::round(NSMidX(box) - s.width / 2), std::round(NSMidY(box) - s.height / 2), s.width, s.height)
			 fromRect:NSZeroRect
			operation:NSCompositingOperationSourceOver
			 fraction:1.0
	   respectFlipped:YES
				hints:nil];
}

// On: the accent's gradient with a dark knob at the right. Off: grey, the knob at the left.
void drawSwitch(NSRect r, bool on, bool enabled)
{
	CGContextRef ctx = NSGraphicsContext.currentContext.CGContext;
	CGContextSaveGState(ctx);
	if(!enabled)
		CGContextSetAlpha(ctx, 0.4);
	NSBezierPath *track = [NSBezierPath bezierPathWithRoundedRect:r xRadius:r.size.height / 2 yRadius:r.size.height / 2];
	if(on)
		[[[NSGradient alloc] initWithStartingColor:kAccentStart() endingColor:kAccentEnd()] drawInBezierPath:track angle:0];
	else
	{
		[rgb(0x3c3a4a) setFill];
		[track fill];
	}
	const CGFloat d = r.size.height - 6;
	const NSRect knob = NSMakeRect(on ? NSMaxX(r) - d - 3 : r.origin.x + 3, r.origin.y + 3, d, d);
	[(on ? rgb(0x2a1420) : rgb(0xdcdce6)) setFill];
	[[NSBezierPath bezierPathWithOvalInRect:knob] fill];
	CGContextRestoreGState(ctx);
}

// Pointing down when the card is open, to the right when it is closed.
void drawChevron(NSRect r, bool open)
{
	NSBezierPath *path = [NSBezierPath bezierPath];
	const CGFloat cx = NSMidX(r), cy = NSMidY(r);
	if(open)
	{
		[path moveToPoint:NSMakePoint(cx - 4.5, cy - 2)];
		[path lineToPoint:NSMakePoint(cx, cy + 2.5)];
		[path lineToPoint:NSMakePoint(cx + 4.5, cy - 2)];
	}
	else
	{
		[path moveToPoint:NSMakePoint(cx - 2, cy - 4.5)];
		[path lineToPoint:NSMakePoint(cx + 2.5, cy)];
		[path lineToPoint:NSMakePoint(cx - 2, cy + 4.5)];
	}
	path.lineWidth = 1.5;
	path.lineCapStyle = NSLineCapStyleRound;
	path.lineJoinStyle = NSLineJoinStyleRound;
	[kDim() setStroke];
	[path stroke];
}

struct Hit
{
	enum Kind { Action, Tab, Stage, CardHead, Reset };
	Kind kind = Action;
	NSRect rect = NSZeroRect;
	PanelAction action;
	NSInteger page = 0;
	std::string card;
};

} // namespace

@implementation PLKPipelinePanelView
{
	PanelState _state;
	PanelModel _model;
	NSInteger _page;
	std::set<std::string> _closed;  // a card's settings are open until its head is clicked
	CGFloat _scroll;
	CGFloat _contentHeight;         // the page's cards, all of them
	NSRect _list;                   // where they show
	std::vector<Hit> _hits;         // as drawn last; a later one lies over an earlier one
	bool _inList;                   // while the cards are drawn: hits are cut to the list
	bool _drawing;                  // false while a row is only measured
	std::string _seek;              // a card to scroll to with the next drawing
	std::string _flash;             // a card whose outline is lit, until _flashUntil
	CFAbsoluteTime _flashUntil;
}

+ (CGFloat)preferredWidth
{
	return kWidth;
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

- (BOOL)acceptsFirstMouse:(NSEvent *)event
{
	return YES;
}

- (BOOL)mouseDownCanMoveWindow
{
	return NO;
}

// The picture's view hides the pointer over the picture; over the panel it is an arrow.
- (void)resetCursorRects
{
	[self addCursorRect:self.bounds cursor:NSCursor.arrowCursor];
}

- (void)showState:(const PanelState &)state
{
	_state = state;
	_model = buildPanel(state);
	_page = std::min<NSInteger>(std::max<NSInteger>(_page, 0), (NSInteger)_model.pages.size() - 1);
	self.needsDisplay = YES;
}

- (NSInteger)page
{
	return _page;
}

- (void)setPage:(NSInteger)page
{
	const NSInteger last = (NSInteger)_model.pages.size() - 1;
	page = std::min(std::max<NSInteger>(page, 0), std::max<NSInteger>(last, 0));
	if(page != _page)
		_scroll = 0;
	_page = page;
	self.needsDisplay = YES;
}

- (void)setExpanded:(BOOL)expanded card:(NSString *)cardId
{
	if(expanded)
		_closed.erase(cardId.UTF8String);
	else
		_closed.insert(cardId.UTF8String);
	self.needsDisplay = YES;
}

- (void)locateCard:(NSString *)cardId
{
	const std::string card = cardId.UTF8String;
	const int page = pageOfCard(_model, card);
	if(page < 0)
		return;
	self.page = page;
	_closed.erase(card);
	_seek = card;
	_flash = card;
	_flashUntil = CFAbsoluteTimeGetCurrent() + kFlashSeconds;
	self.needsDisplay = YES;
	PLKPipelinePanelView *__weak weakSelf = self;
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)((kFlashSeconds + 0.05) * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
		weakSelf.needsDisplay = YES;
	});
}

#pragma mark - Mouse

- (void)mouseDown:(NSEvent *)event
{
	[self clickAt:[self convertPoint:event.locationInWindow fromView:nil]];
}

// Not the picture's menu: the panel is not the picture.
- (void)rightMouseDown:(NSEvent *)event
{
}

- (void)scrollWheel:(NSEvent *)event
{
	const CGFloat most = std::max<CGFloat>(0, _contentHeight - _list.size.height);
	const CGFloat to = std::min(std::max<CGFloat>(_scroll - event.scrollingDeltaY, 0), most);
	if(to != _scroll)
	{
		_scroll = to;
		self.needsDisplay = YES;
	}
}

- (BOOL)clickAt:(NSPoint)point
{
	for(auto it = _hits.rbegin(); it != _hits.rend(); ++it)
	{
		if(!NSPointInRect(point, it->rect))
			continue;
		// A copy: what the click does may bring a new state, and with it new hits.
		const Hit hit = *it;
		switch(hit.kind)
		{
			case Hit::Tab:
				self.page = hit.page;
				break;
			case Hit::Stage:
				[self locateCard:str(hit.card)];
				break;
			case Hit::CardHead:
				if(_closed.count(hit.card))
					_closed.erase(hit.card);
				else
					_closed.insert(hit.card);
				self.needsDisplay = YES;
				break;
			case Hit::Reset:
			{
				if(_page < 0 || _page >= (NSInteger)_model.pages.size())
					break;
				const std::vector<PanelAction> actions = _model.pages[(size_t)_page].reset;
				for(const PanelAction &a : actions)
					if(self.onAction)
						self.onAction(a);
				break;
			}
			case Hit::Action:
				if(self.onAction)
					self.onAction(hit.action);
				break;
		}
		return YES;
	}
	return NO;
}

- (NSRect)rectOfCommand:(PanelCommand)command tag:(int)tag
{
	for(const Hit &h : _hits)
		if(h.kind == Hit::Action && h.action.command == command && h.action.tag == tag)
			return h.rect;
	return NSZeroRect;
}

- (NSRect)rectOfTab:(NSInteger)page
{
	for(const Hit &h : _hits)
		if(h.kind == Hit::Tab && h.page == page)
			return h.rect;
	return NSZeroRect;
}

- (NSRect)rectOfStage:(NSString *)cardId
{
	for(const Hit &h : _hits)
		if(h.kind == Hit::Stage && h.card == cardId.UTF8String)
			return h.rect;
	return NSZeroRect;
}

- (NSRect)rectOfCardHead:(NSString *)cardId
{
	for(const Hit &h : _hits)
		if(h.kind == Hit::CardHead && h.card == cardId.UTF8String)
			return h.rect;
	return NSZeroRect;
}

- (NSRect)rectOfReset
{
	for(const Hit &h : _hits)
		if(h.kind == Hit::Reset)
			return h.rect;
	return NSZeroRect;
}

- (void)layoutNow
{
	NSBitmapImageRep *nothing = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
																		 pixelsWide:1
																		 pixelsHigh:1
																	  bitsPerSample:8
																	samplesPerPixel:4
																		   hasAlpha:YES
																		   isPlanar:NO
																	 colorSpaceName:NSCalibratedRGBColorSpace
																		bytesPerRow:0
																	   bitsPerPixel:0];
	[NSGraphicsContext saveGraphicsState];
	NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithBitmapImageRep:nothing];
	[self drawPanel];
	[NSGraphicsContext restoreGraphicsState];
}

#pragma mark - Drawing

- (void)addHit:(Hit)hit
{
	if(!_drawing)
		return;
	if(_inList)
		hit.rect = NSIntersectionRect(hit.rect, _list);
	if(hit.rect.size.width > 0 && hit.rect.size.height > 0)
		_hits.push_back(hit);
}

- (void)addAction:(const PanelAction &)action at:(NSRect)rect
{
	Hit hit;
	hit.kind = Hit::Action;
	hit.rect = rect;
	hit.action = action;
	[self addHit:hit];
}

- (void)drawRect:(NSRect)dirty
{
	[self drawPanel];
}

- (void)drawPanel
{
	// A second time when the first found that the list has to be somewhere else.
	if([self drawOnce])
		[self drawOnce];
}

// @return whether the scroll position changed while drawing
- (BOOL)drawOnce
{
	_hits.clear();
	_drawing = true;
	_inList = false;
	const NSRect bounds = self.bounds;
	fillRect(bounds, kPanelFill());
	fillRect(NSMakeRect(0, 0, 1, bounds.size.height), white(0.09));  // the edge towards the picture
	if(_model.pages.empty() || _page < 0 || _page >= (NSInteger)_model.pages.size())
		return NO;
	const PanelPage &page = _model.pages[(size_t)_page];
	const CGFloat x = kPad, width = bounds.size.width - 2 * kPad;
	CGFloat y = [self drawTabsAt:kPad x:x width:width];
	y = [self drawOrderAt:y x:x width:width];

	const CGFloat footerTop = bounds.size.height - kFooterHeight;
	_list = NSMakeRect(0, y, bounds.size.width, std::max<CGFloat>(0, footerTop - y));
	CGFloat scrollTo = _scroll;
	[NSGraphicsContext saveGraphicsState];
	NSRectClip(_list);
	_inList = true;
	CGFloat cy = y + 2 - _scroll;
	for(const PanelCard &card : page.cards)
	{
		if(card.id == _seek)
			scrollTo = cy + _scroll - (y + 2);
		cy += [self drawCard:card at:cy x:x width:width] + kCardGap;
	}
	_inList = false;
	[NSGraphicsContext restoreGraphicsState];
	_contentHeight = cy + _scroll - y;
	_seek.clear();
	// Where the list ends there is more of it: a fade says so.
	const CGFloat most = std::max<CGFloat>(0, _contentHeight - _list.size.height);
	if(_scroll < most - 0.5)
		[[[NSGradient alloc] initWithStartingColor:rgb(0x14121c, 0.0) endingColor:rgb(0x14121c, 0.95)]
				drawInRect:NSMakeRect(1, footerTop - 22, bounds.size.width - 1, 22)
					 angle:90];
	[self drawFooter:page top:footerTop x:x width:width];

	scrollTo = std::min(std::max<CGFloat>(scrollTo, 0), most);
	if(std::fabs(scrollTo - _scroll) > 0.5)
	{
		_scroll = scrollTo;
		return YES;
	}
	return NO;
}

- (CGFloat)drawTabsAt:(CGFloat)y x:(CGFloat)x width:(CGFloat)width
{
	fillRounded(NSMakeRect(x, y, width, kTabsHeight), 9, white(0.06));
	const size_t count = _model.pages.size();
	const CGFloat each = (width - 4) / (CGFloat)count;
	for(size_t i = 0; i < count; i++)
	{
		const NSRect tab = NSMakeRect(x + 2 + each * (CGFloat)i, y + 2, each, kTabsHeight - 4);
		const bool shown = (NSInteger)i == _page;
		if(shown)
			fillRounded(tab, 7, white(0.15));
		drawCentred(str(_model.pages[i].title), tab, font(12, shown ? kText() : kDim(), shown ? NSFontWeightSemibold : NSFontWeightRegular));
		Hit hit;
		hit.kind = Hit::Tab;
		hit.rect = tab;
		hit.page = (NSInteger)i;
		[self addHit:hit];
	}
	return y + kTabsHeight + 12;
}

// The stages as chips, an arrow before every one but the first; a stage that does nothing
// to this picture is struck out and has no chip around it.
- (CGFloat)drawOrderAt:(CGFloat)y x:(CGFloat)x width:(CGFloat)width
{
	drawLine(@"Processing order", x, y, font(10.5, kDim(), NSFontWeightMedium));
	NSDictionary *hint = font(9.5, kFaint());
	drawLine(@"Click to locate", x + width - textWidth(@"Click to locate", hint), y + 1, hint);
	y += 19;
	const CGFloat chipHeight = 20;
	NSDictionary *arrow = font(10.5, kFaint());
	const CGFloat arrowWidth = textWidth(@"→", arrow) + 8;
	CGFloat cx = x;
	for(size_t i = 0; i < _model.order.size(); i++)
	{
		const PanelStage &stage = _model.order[i];
		NSDictionary *a = font(10.5, stage.active ? kText() : kFaint(), stage.active ? NSFontWeightMedium : NSFontWeightRegular);
		if(!stage.active)
		{
			NSMutableDictionary *struck = [a mutableCopy];
			struck[NSStrikethroughStyleAttributeName] = @(NSUnderlineStyleSingle);
			struck[NSStrikethroughColorAttributeName] = kFaint();
			a = struck;
		}
		NSString *title = str(stage.title);
		const CGFloat chipWidth = textWidth(title, a) + (stage.active ? 14 : 4);
		const CGFloat lead = i > 0 ? arrowWidth : 0;
		if(cx > x && cx + lead + chipWidth > x + width + 0.5)
		{
			cx = x;
			y += chipHeight + 5;
		}
		if(i > 0)
		{
			drawCentred(@"→", NSMakeRect(cx, y, arrowWidth, chipHeight), arrow);
			cx += arrowWidth;
		}
		const NSRect chip = NSMakeRect(cx, y, chipWidth, chipHeight);
		if(stage.active)
			fillRounded(chip, 6, white(0.10));
		drawCentred(title, chip, a);
		Hit hit;
		hit.kind = Hit::Stage;
		hit.rect = chip;
		hit.card = stage.card;
		[self addHit:hit];
		cx += chipWidth;
	}
	return y + chipHeight + 12;
}

// @return the card's height
- (CGFloat)drawCard:(const PanelCard &)card at:(CGFloat)y x:(CGFloat)x width:(CGFloat)width
{
	const bool open = !card.rows.empty() && !_closed.count(card.id);
	const CGFloat inner = width - 2 * kInner;
	CGFloat height = kHeadHeight;
	if(open)
	{
		_drawing = false;
		for(const PanelRow &row : card.rows)
			height += [self row:row at:0 x:0 width:inner];
		_drawing = true;
		height += 3;
	}
	const NSRect r = NSMakeRect(x, y, width, height);
	// Out of sight: only its height matters.
	if(NSMaxY(r) < NSMinY(_list) || NSMinY(r) > NSMaxY(_list))
		return height;
	fillRounded(r, 11, kCardFill());
	const bool lit = card.id == _flash && CFAbsoluteTimeGetCurrent() < _flashUntil;
	NSBezierPath *edge = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(r, 0.5, 0.5) xRadius:11 yRadius:11];
	edge.lineWidth = lit ? 1.5 : 1;
	[(lit ? kAccent() : white(0.08)) setStroke];
	[edge stroke];

	CGContextRef ctx = NSGraphicsContext.currentContext.CGContext;
	CGContextSaveGState(ctx);
	if(card.dimmed)
		CGContextSetAlpha(ctx, 0.5);

	// The head: icon, title with its badge, one line about now; switch and chevron at the right.
	const bool lively = card.has_toggle ? card.on : !card.dimmed;
	const NSRect tile = NSMakeRect(x + 10, y + 10, 28, 28);
	fillRounded(tile, 8, white(0.07));
	drawSymbol(card.icon, tile, 13, lively ? 0xff8f6b : 0x8c8c9e);
	CGFloat right = NSMaxX(r) - 10;
	if(!card.rows.empty())
	{
		Hit head;
		head.kind = Hit::CardHead;
		head.rect = NSMakeRect(x, y, width, kHeadHeight);
		head.card = card.id;
		[self addHit:head];
		drawChevron(NSMakeRect(right - 14, y + (kHeadHeight - 14) / 2, 14, 14), open);
		right -= 14 + 8;
	}
	if(card.has_toggle)
	{
		const NSRect sw = NSMakeRect(right - kSwitchWidth, y + (kHeadHeight - kSwitchHeight) / 2, kSwitchWidth, kSwitchHeight);
		drawSwitch(sw, card.on, card.toggle_enabled);
		if(card.toggle_enabled)
			[self addAction:card.toggle at:NSInsetRect(sw, -6, -8)];
		right -= kSwitchWidth + 10;
	}
	const CGFloat textX = NSMaxX(tile) + 10, textWidthMax = right - textX;
	NSDictionary *titleFont = font(12.5, kText(), NSFontWeightSemibold);
	NSString *title = str(card.title);
	CGFloat badgeWidth = 0;
	NSDictionary *badgeFont = font(9.5, rgb(0xcfc6ff), NSFontWeightSemibold);
	if(!card.badge.empty())
		badgeWidth = textWidth(str(card.badge), badgeFont) + 14 + 8;
	const CGFloat titleWidth = std::min(textWidth(title, titleFont), std::max<CGFloat>(20, textWidthMax - badgeWidth));
	drawLine(title, textX, y + 9, titleFont, titleWidth + 1);
	if(badgeWidth > 0)
	{
		const NSRect badge = NSMakeRect(textX + titleWidth + 8, y + 9, badgeWidth - 8, 16);
		fillRounded(badge, 8, rgb(0x5b4bc4, 0.5));
		drawCentred(str(card.badge), badge, badgeFont);
	}
	drawLine(str(card.subtitle), textX, y + 27, font(10.5, kDim(), NSFontWeightRegular, true), textWidthMax);

	if(open)
	{
		CGFloat ry = y + kHeadHeight;
		for(const PanelRow &row : card.rows)
		{
			fillRect(NSMakeRect(x + kInner, ry, inner, 1), white(0.07));
			ry += [self row:row at:ry x:x + kInner width:inner];
		}
	}
	CGContextRestoreGState(ctx);
	return height;
}

// A row of a card's settings, measured (_drawing false) or drawn. @return its height
- (CGFloat)row:(const PanelRow &)row at:(CGFloat)y x:(CGFloat)x width:(CGFloat)width
{
	NSDictionary *titleFont = font(11.5, kText(), NSFontWeightMedium);
	NSDictionary *detailFont = font(9.5, kFaint());
	NSString *title = str(row.title), *detail = str(row.detail);
	const CGFloat titleHeight = row.title.empty() ? 0 : lineHeight(titleFont);
	CGContextRef ctx = _drawing ? NSGraphicsContext.currentContext.CGContext : nullptr;
	if(ctx)
	{
		CGContextSaveGState(ctx);
		if(!row.enabled)
			CGContextSetAlpha(ctx, 0.45);
	}
	CGFloat height = 0;
	// Title and detail in a column `left` wide; @return the column's height.
	auto words = [&](CGFloat left) {
		const CGFloat detailHeight = wrappedHeight(detail, detailFont, left);
		if(_drawing)
		{
			if(titleHeight > 0)
				drawLine(title, x, y + kRowPad, titleFont, left);
			if(detailHeight > 0)
				drawWrapped(detail, x, y + kRowPad + titleHeight + (titleHeight > 0 ? 2 : 0), left, detailFont);
		}
		return titleHeight + (titleHeight > 0 && detailHeight > 0 ? 2 : 0) + detailHeight;
	};
	switch(row.kind)
	{
		case PanelRow::Kind::Note:
		{
			NSDictionary *a = font(10, row.warning ? kWarn() : kDim());
			const CGFloat indent = 18, textHeight = wrappedHeight(detail, a, width - indent);
			if(_drawing)
			{
				drawSymbol(row.warning ? "exclamationmark.triangle" : "info.circle", NSMakeRect(x, y + kRowPad - 1, 13, 14), 10,
						row.warning ? 0xffb340 : 0x8c8c9e);
				drawWrapped(detail, x + indent, y + kRowPad, width - indent, a);
			}
			height = kRowPad + std::max<CGFloat>(textHeight, 13) + kRowPad;
			break;
		}
		case PanelRow::Kind::Toggle:
		{
			const CGFloat left = width - kSwitchWidth - 14;
			const CGFloat textHeight = words(left);
			if(_drawing)
			{
				const NSRect sw = NSMakeRect(x + width - kSwitchWidth, y + kRowPad, kSwitchWidth, kSwitchHeight);
				drawSwitch(sw, row.on, row.enabled);
				if(row.enabled)
					[self addAction:row.action at:NSInsetRect(sw, -6, -6)];
			}
			height = kRowPad + std::max(textHeight, kSwitchHeight) + kRowPad;
			break;
		}
		case PanelRow::Kind::Stepper:
		{
			const CGFloat control = 2 * kStepButton + kStepValue + 4;
			const CGFloat textHeight = words(width - control - 14);
			if(_drawing)
			{
				const NSRect box = NSMakeRect(x + width - control, y + kRowPad, control, kControlHeight);
				fillRounded(box, 7, white(0.06));
				const NSRect less = NSMakeRect(box.origin.x + 2, box.origin.y + 2, kStepButton, kControlHeight - 4);
				const NSRect value = NSMakeRect(NSMaxX(less), box.origin.y + 2, kStepValue, kControlHeight - 4);
				const NSRect more = NSMakeRect(NSMaxX(value), box.origin.y + 2, kStepButton, kControlHeight - 4);
				drawCentred(@"−", less, font(13, row.can_less ? kText() : kFaint(), NSFontWeightMedium));
				drawCentred(@"+", more, font(13, row.can_more ? kText() : kFaint(), NSFontWeightMedium));
				drawCentred(str(row.value), value, font(11.5, row.at_default ? kText() : kAccent(), NSFontWeightMedium, true));
				if(row.enabled)
				{
					if(row.can_less)
						[self addAction:row.less at:less];
					if(row.can_more)
						[self addAction:row.more at:more];
					if(!row.at_default)
						[self addAction:row.reset at:value];
				}
			}
			height = kRowPad + std::max(textHeight, kControlHeight) + kRowPad;
			break;
		}
		case PanelRow::Kind::Choice:
		{
			NSDictionary *plain = font(11, kDim()), *chosen = font(11, kText(), NSFontWeightSemibold);
			std::vector<CGFloat> widths;
			CGFloat natural = 4;
			for(const PanelOption &o : row.options)
			{
				widths.push_back(textWidth(str(o.title), chosen) + 18);
				natural += widths.back();
			}
			const bool beside = natural <= width * kInlineShare;
			CGFloat textHeight;
			NSRect box;
			if(beside)
			{
				textHeight = words(width - natural - 14);
				box = NSMakeRect(x + width - natural, y + kRowPad, natural, kControlHeight);
				height = kRowPad + std::max(textHeight, kControlHeight) + kRowPad;
			}
			else
			{
				// Under its title, across the row, in buttons of one width.
				textHeight = words(width);
				box = NSMakeRect(x, y + kRowPad + textHeight + 7, width, kControlHeight);
				for(CGFloat &w : widths)
					w = (width - 4) / (CGFloat)row.options.size();
				height = kRowPad + textHeight + 7 + kControlHeight + kRowPad;
			}
			if(_drawing)
			{
				fillRounded(box, 7, white(0.06));
				CGFloat cx = box.origin.x + 2;
				for(size_t i = 0; i < row.options.size(); i++)
				{
					const PanelOption &o = row.options[i];
					const NSRect button = NSMakeRect(cx, box.origin.y + 2, widths[i], kControlHeight - 4);
					if(o.selected)
						fillRounded(button, 5.5, white(0.17));
					drawCentred(str(o.title), button, o.selected ? chosen : o.enabled ? plain : font(11, kFaint()));
					if(o.enabled && row.enabled)
						[self addAction:o.action at:button];
					cx += widths[i];
				}
			}
			break;
		}
	}
	if(ctx)
		CGContextRestoreGState(ctx);
	return height;
}

// A dot in the colour of how the run is doing, the sizes a frame goes through, and the reset.
- (void)drawFooter:(const PanelPage &)page top:(CGFloat)top x:(CGFloat)x width:(CGFloat)width
{
	fillRect(NSMakeRect(1, top, self.bounds.size.width - 1, 1), white(0.08));
	NSDictionary *resetFont = font(10.5, page.reset.empty() ? kFaint() : kText(), NSFontWeightMedium);
	NSString *reset = @"Reset this page";
	const CGFloat resetWidth = textWidth(reset, resetFont);
	const NSRect resetRect = NSMakeRect(x + width - resetWidth - 16, top + 10, resetWidth + 16, 24);
	if(!page.reset.empty())
	{
		fillRounded(resetRect, 7, white(0.08));
		Hit hit;
		hit.kind = Hit::Reset;
		hit.rect = resetRect;
		[self addHit:hit];
	}
	drawCentred(reset, resetRect, resetFont);
	[(_model.warning ? kWarn() : kGood()) setFill];
	[[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(x + 1, top + 13, 7, 7)] fill];
	NSDictionary *a = font(9.5, kDim(), NSFontWeightRegular, true);
	const CGFloat textX = x + 15, textW = NSMinX(resetRect) - 10 - textX;
	[NSGraphicsContext saveGraphicsState];
	NSRectClip(NSMakeRect(textX, top + 6, textW, kFooterHeight - 10));
	drawWrapped(str(_model.footer), textX, top + 9, textW, a);
	[NSGraphicsContext restoreGraphicsState];
}

@end
