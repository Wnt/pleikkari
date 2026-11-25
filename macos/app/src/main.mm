// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Pleikkari.app: native macOS PS5 Remote Play client (AppKit, no Qt/SDL/libplacebo).
// See macos/app/README.md.

#import "StreamWindow.h"

#include "Log.h"
#include "Options.h"
#include "Registration.h"
#include "Runner.h"
#include "SavedChoices.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <memory>
#include <mutex>

using namespace plk;
using namespace plk::app;

namespace {

// For the whole stream: no App Nap, no timer coalescing, no display sleep.
constexpr NSActivityOptions kStreamActivity =
		NSActivityLatencyCritical | NSActivityUserInitiated | NSActivityIdleDisplaySleepDisabled;

int listConsoles(const Options &o)
{
	std::vector<RegisteredConsole> consoles;
	std::string err;
	if(!loadRegisteredConsoles(qtSettingsPlistPath(o.profile), consoles, &err))
	{
		fprintf(stderr, "Pleikkari: %s\n", err.c_str());
		return 1;
	}
	// Nickname and type only: everything else in a registration is secret.
	for(const auto &c : consoles)
		printf("%s\t%s%s\n", c.nickname.c_str(), c.consoleTypeName(), c.usable() ? "" : "\t(incomplete registration)");
	return 0;
}

float mainScreenRefreshHz()
{
	NSScreen *screen = NSScreen.mainScreen;
	return screen ? (float)screen.maximumFramesPerSecond : 120.0f;
}

/** No window and no NSApplication: video renders offscreen, the controller is read-only. */
int runHeadless(const Options &o)
{
	Runner runner(o);
	int code = 0;
	if(!runner.prepare(&code))
		return code;
	CAMetalLayer *layer = [CAMetalLayer layer];
	layer.drawableSize = CGSizeMake(o.offscreen_width, o.offscreen_height);
	// No AppKit here at all (not even NSScreen): a headless run never touches the window server.
	if(!runner.createPipeline((__bridge void *)layer, 120.0f))
		return runner.exitCode() ? runner.exitCode() : 1;

	// SIGINT / SIGTERM end the run cleanly.
	Runner *r = &runner;
	signal(SIGINT, SIG_IGN);
	signal(SIGTERM, SIG_IGN);
	dispatch_queue_t q = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
	dispatch_source_t sigint = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGINT, 0, q);
	dispatch_source_t sigterm = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, q);
	dispatch_source_set_event_handler(sigint, ^{
		PLK_LOGI("SIGINT: ending");
		r->requestStop();
	});
	dispatch_source_set_event_handler(sigterm, ^{
		PLK_LOGI("SIGTERM: ending");
		r->requestStop();
	});
	dispatch_resume(sigint);
	dispatch_resume(sigterm);

	id<NSObject> activity = [NSProcessInfo.processInfo beginActivityWithOptions:kStreamActivity
																		 reason:@"Pleikkari stream"];
	std::mutex m;
	std::condition_variable cv;
	bool done = false;
	const bool started = runner.start([&] {
		std::lock_guard<std::mutex> lk(m);
		done = true;
		cv.notify_all();
	});
	if(started)
	{
		std::unique_lock<std::mutex> lk(m);
		cv.wait(lk, [&] { return done; });
	}
	runner.requestStop();
	runner.shutdownWorkers();
	runner.shutdownVideo();
	[NSProcessInfo.processInfo endActivity:activity];
	dispatch_source_cancel(sigint);
	dispatch_source_cancel(sigterm);
	const int exit_code = started ? runner.exitCode() : (runner.exitCode() ? runner.exitCode() : 1);
	Log::instance().close();
	return exit_code;
}

} // namespace

@interface PLKAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate, NSMenuItemValidation, PLKVideoMenuActions>
/** savedNote: which of the menu's remembered choices this run starts with, for the log. */
- (instancetype)initWithOptions:(const Options &)options savedNote:(const std::string &)savedNote;
@property(nonatomic, readonly) int exitCode;
@end

@implementation PLKAppDelegate
{
	Options _options;
	std::string _savedNote;
	std::unique_ptr<Runner> _runner;
	PLKStreamWindow *_window;
	id<NSObject> _activity;
	BOOL _terminating;
	BOOL _cleanedUp;
	int _exitCode;
	NSUInteger _hdrSwitchSerial;
	float _headroom;
	float _loggedHeadroom;
	NSTimer *_headroomTimer;
	id _screenObserver;
	NSArray *_fullscreenObservers;
}

- (instancetype)initWithOptions:(const Options &)options savedNote:(const std::string &)savedNote
{
	if((self = [super init]))
	{
		_options = options;
		_savedNote = savedNote;
	}
	return self;
}

- (int)exitCode
{
	return _exitCode;
}

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
	_runner = std::make_unique<Runner>(_options);
	int code = 0;
	if(!_runner->prepare(&code))
	{
		_exitCode = code;
		_cleanedUp = YES;
		[NSApp terminate:nil];
		return;
	}
	_activity = [NSProcessInfo.processInfo beginActivityWithOptions:kStreamActivity reason:@"Pleikkari stream"];
	if(!_savedNote.empty())
		PLK_LOGI("Remembered from the Video menu: %s", _savedNote.c_str());

	const StreamConfig &sc = _runner->streamConfig();
	const CGFloat aspect = sc.height > 0 ? (CGFloat)sc.width / (CGFloat)sc.height : 16.0 / 9.0;
	_window = [PLKStreamWindow streamWindowWithTitle:@(_runner->title().c_str()) aspect:aspect];
	_window.delegate = self;
	if(!_runner->createPipeline((__bridge void *)_window.videoView.metalLayer, mainScreenRefreshHz()))
	{
		_exitCode = 1;
		[NSApp terminate:nil];
		return;
	}
	Runner *runner = _runner.get();
	_window.videoView.onDrawableSize = ^(int w, int h) {
		runner->setDrawableSize(w, h);
	};
	[_window makeKeyAndOrderFront:nil];
	[self updateDisplayHeadroom];
	PLKAppDelegate *__weak headroomSelf = self;
	_screenObserver = [NSNotificationCenter.defaultCenter addObserverForName:NSApplicationDidChangeScreenParametersNotification
																	  object:nil
																	   queue:NSOperationQueue.mainQueue
																  usingBlock:^(NSNotification *note) {
																	  [headroomSelf updateDisplayHeadroom];
																  }];
	_headroomTimer = [NSTimer timerWithTimeInterval:0.25
											repeats:YES
											  block:^(NSTimer *timer) {
												  [headroomSelf updateDisplayHeadroom];
											  }];
	_headroomTimer.tolerance = 0.05;
	[NSRunLoop.mainRunLoop addTimer:_headroomTimer forMode:NSRunLoopCommonModes];
	// Fullscreen from the end of the animation in to the start of the one out: the time
	// the system can hand the layer straight to the display.
	NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
	_fullscreenObservers = @[
		[center addObserverForName:NSWindowDidEnterFullScreenNotification
							object:_window
							 queue:NSOperationQueue.mainQueue
						usingBlock:^(NSNotification *note) {
							[headroomSelf windowFullscreenChanged:YES];
						}],
		[center addObserverForName:NSWindowWillExitFullScreenNotification
							object:_window
							 queue:NSOperationQueue.mainQueue
						usingBlock:^(NSNotification *note) {
							[headroomSelf windowFullscreenChanged:NO];
						}],
	];
	[_window.videoView reportDrawableSize];
	if(@available(macOS 14.0, *))
		[NSApp activate];
	else
		[NSApp activateIgnoringOtherApps:YES];
	[NSCursor setHiddenUntilMouseMoves:YES];
	if(_options.fullscreen)
	{
		// After the window is on screen: a toggle on a window still being ordered in can be
		// dropped.
		PLKStreamWindow *window = _window;
		dispatch_async(dispatch_get_main_queue(), ^{
			if(!(window.styleMask & NSWindowStyleMaskFullScreen))
				[window toggleFullScreen:nil];
		});
	}

	PLKAppDelegate *__weak weakSelf = self;
	const bool started = _runner->start([weakSelf] {
		// The session or replay ended (console quit, error, --seconds): quit the app.
		dispatch_async(dispatch_get_main_queue(), ^{
			PLKAppDelegate *strongSelf = weakSelf;
			if(strongSelf && !strongSelf->_terminating)
				[NSApp terminate:nil];
		});
	});
	if(!started)
	{
		[NSApp terminate:nil];
		return;
	}
	[self scheduleKeyScript];
}

#pragma mark - Video menu

- (BOOL)validateMenuItem:(NSMenuItem *)item
{
	const SEL action = item.action;
	const bool ours = action == @selector(plkSelectScaler:) || action == @selector(plkSelectSdrColor:)
			|| action == @selector(plkSelectHdrOutput:) || action == @selector(plkToggleHdr:)
			|| action == @selector(plkStepHdrBrightness:) || action == @selector(plkToggleHdrToneMap:)
			|| action == @selector(plkToggleFullscreenSync:);
	if(!ours)
		return YES;
	if(!_runner || _terminating || _cleanedUp)
		return NO;
	const Runner::ViewState v = _runner->viewState();
	bool on = false, enabled = true;
	if(action == @selector(plkSelectScaler:))
		on = (NSInteger)v.scaler == item.tag;
	else if(action == @selector(plkSelectSdrColor:))
		on = (NSInteger)v.sdr_color == item.tag;
	else if(action == @selector(plkSelectHdrOutput:))
		on = (NSInteger)v.hdr_output == item.tag;
	else if(action == @selector(plkToggleHdrToneMap:))
		on = v.hdr_tone_map == video::HdrToneMap::System;
	else if(action == @selector(plkToggleFullscreenSync:))
	{
		on = v.fullscreen_sync;
		enabled = v.fullscreen_sync_applies;
	}
	else if(action == @selector(plkStepHdrBrightness:))
	{
		if(item.tag == 0)
		{
			on = v.hdr_brightness == kHdrBrightnessDefault;
			item.title = on ? @"Standard brightness (100 %)"
							: [NSString stringWithFormat:@"Brightness %d %%: back to 100 %%", v.hdr_brightness];
		}
		else
			enabled = stepHdrBrightness(v.hdr_brightness, (int)item.tag) != v.hdr_brightness;
	}
	else
	{
		on = v.hdr;
		enabled = v.hdr_switchable;
	}
	item.state = on ? NSControlStateValueOn : NSControlStateValueOff;
	return enabled;
}

- (void)plkSelectScaler:(id)sender
{
	const auto scaler = (video::Scaler)[sender tag];
	_runner->setScaler(scaler);
	if(_options.saved_choices)
		saveScaler(scaler);
	[_window.videoView showNotice:[@"Scaling: " stringByAppendingString:[sender title]]];
}

- (void)plkSelectSdrColor:(id)sender
{
	const auto color = (video::SdrColor)[sender tag];
	_runner->setSdrColor(color);
	if(_options.saved_choices)
		saveSdrColor(color);
	NSString *text = [@"Colour: " stringByAppendingString:[sender title]];
	if(_runner->pictureIsHdr())
		text = [text stringByAppendingString:@" - for an SDR picture; this one is HDR"];
	[_window.videoView showNotice:text];
}

- (void)plkSelectHdrOutput:(id)sender
{
	const auto output = (video::HdrOutput)[sender tag];
	_runner->setHdrOutput(output);
	if(_options.saved_choices)
		saveHdrOutput(output);
	NSString *text = [@"HDR: " stringByAppendingString:[sender title]];
	if(!_runner->pictureIsHdr())
		text = [text stringByAppendingString:@" - for an HDR picture; this one is SDR"];
	[_window.videoView showNotice:text];
}

- (void)plkStepHdrBrightness:(id)sender
{
	// Also reached straight from the + and - keys, not only through a validated menu item.
	if(!_runner || _terminating || _cleanedUp)
		return;
	const int direction = (int)[sender tag];
	const int before = _runner->viewState().hdr_brightness;
	const int percent = _runner->setHdrBrightness(direction ? stepHdrBrightness(before, direction) : kHdrBrightnessDefault);
	if(_options.saved_choices && percent != before)
		saveHdrBrightness(percent);
	NSString *text = [NSString stringWithFormat:@"HDR brightness %d %%", percent];
	if(!_runner->pictureIsHdr())
		text = [text stringByAppendingString:@" - for an HDR picture; this one is SDR"];
	[_window.videoView showNotice:text];
}

- (void)plkToggleHdrToneMap:(id)sender
{
	const bool system = _runner->viewState().hdr_tone_map != video::HdrToneMap::System;
	const video::HdrToneMap tone_map = system ? video::HdrToneMap::System : video::HdrToneMap::App;
	_runner->setHdrToneMap(tone_map);
	if(_options.saved_choices)
		saveHdrToneMap(tone_map);
	NSString *text = system ? @"HDR tone mapping by macOS" : @"HDR tone mapping by the app";
	if(!_runner->pictureIsHdr())
		text = [text stringByAppendingString:@" - for an HDR picture; this one is SDR"];
	[_window.videoView showNotice:text];
}

- (void)plkToggleFullscreenSync:(id)sender
{
	const bool on = !_runner->viewState().fullscreen_sync;
	_runner->setFullscreenSync(on);
	if(_options.saved_choices)
		saveFullscreenSync(on);
	[_window.videoView showNotice:on ? @"Fullscreen: frames in step with the display, no tearing"
									 : @"Fullscreen: frames shown at once, the picture can tear"];
}

- (void)windowFullscreenChanged:(BOOL)fullscreen
{
	if(!_runner || _terminating || _cleanedUp)
		return;
	_runner->setWindowFullscreen(fullscreen);
}

// What the display can show above SDR white right now. It changes with the display's
// brightness and while the system makes room for an HDR picture; the notification does not
// come for every step of that, so a timer asks as well.
- (void)updateDisplayHeadroom
{
	if(!_runner || _terminating || _cleanedUp)
		return;
	NSScreen *screen = _window.screen ?: NSScreen.mainScreen;
	const float current = (float)screen.maximumExtendedDynamicRangeColorComponentValue;
	const float potential = (float)screen.maximumPotentialExtendedDynamicRangeColorComponentValue;
	const float headroom = video::usableDisplayHeadroom(current, potential);
	if(headroom == _headroom)
		return;
	_headroom = headroom;
	_runner->setDisplayHeadroom(headroom);
	// The log gets the settled values, not every step of a ramp.
	if(_loggedHeadroom <= 0 || headroom > _loggedHeadroom * 1.05f || headroom < _loggedHeadroom * 0.95f)
	{
		_loggedHeadroom = headroom;
		PLK_LOGI("Display headroom %.2f (the screen says %.2f now, %.2f at most)", headroom, current, potential);
	}
}

- (void)plkToggleHdr:(id)sender
{
	const bool on = !_runner->viewState().hdr;
	if(!_runner->setHdr(on))
	{
		[_window.videoView showNotice:@"HDR can be switched in a stream of a PS5 only"];
		return;
	}
	if(_options.saved_choices)
		saveHdr(on);
	// The console takes about ten seconds to accept the new session: the notice stays
	// until it has.
	[_window.videoView showNotice:on ? @"HDR on: reconnecting..." : @"HDR off: reconnecting..." seconds:0];
	[self pollHdrSwitch:++_hdrSwitchSerial on:on since:CFAbsoluteTimeGetCurrent()];
}

- (void)pollHdrSwitch:(NSUInteger)serial on:(BOOL)on since:(CFAbsoluteTime)since
{
	if(serial != _hdrSwitchSerial || !_runner || _terminating || _cleanedUp)
		return;
	if(_runner->hdrSwitchSettled())
	{
		PLK_LOGI("HDR %s: connected again after %.1f s", on ? "on" : "off", CFAbsoluteTimeGetCurrent() - since);
		[_window.videoView showNotice:on ? @"HDR on" : @"HDR off"];
		return;
	}
	if(CFAbsoluteTimeGetCurrent() - since > 60)
	{
		[_window.videoView showNotice:@"Still reconnecting"];
		return;
	}
	PLKAppDelegate *__weak weakSelf = self;
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.2 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
		[weakSelf pollHdrSwitch:serial on:on since:since];
	});
}

#pragma mark - --keys (test tool)

// The app presses the keys itself: the events go through the application's own queue, the
// way a key on the keyboard does, so this needs no permission to control the computer.
- (void)scheduleKeyScript
{
	std::vector<KeyPress> presses;
	if(_options.keys.empty() || !parseKeyScript(_options.keys, presses, nullptr))
		return;
	PLKAppDelegate *__weak weakSelf = self;
	for(const KeyPress &press : presses)
	{
		NSString *key = @(press.key.c_str());
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(press.at_s * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
			[weakSelf pressKey:key];
		});
	}
}

- (void)pressKey:(NSString *)name
{
	if(_terminating || !_window)
		return;
	struct Special { const char *name; unichar character; unsigned short code; };
	static const Special specials[] = {
		{ "up", NSUpArrowFunctionKey, 126 }, { "down", NSDownArrowFunctionKey, 125 }, { "left", NSLeftArrowFunctionKey, 123 },
		{ "right", NSRightArrowFunctionKey, 124 }, { "enter", '\r', 36 }, { "esc", 0x1b, 53 }, { "space", ' ', 49 },
	};
	// ANSI key codes of the characters the menu uses; any other character goes out with the
	// code of a key that types nothing (kVK_F20), which only the characters then identify.
	static const struct { char character; unsigned short code; } codes[] = {
		{ '1', 18 }, { '2', 19 }, { '3', 20 }, { '4', 21 }, { 'a', 0 }, { 'v', 9 }, { 'h', 4 }, { 'p', 35 },
		{ 'l', 37 }, { 'm', 46 }, { 'f', 3 },
	};
	NSString *characters = name;
	unsigned short code = 90;
	NSEventModifierFlags flags = 0;
	for(const Special &s : specials)
		if([name isEqualToString:@(s.name)])
		{
			characters = [NSString stringWithCharacters:&s.character length:1];
			code = s.code;
			if(s.character >= 0xF700)
				flags = NSEventModifierFlagFunction | NSEventModifierFlagNumericPad;
		}
	if(name.length == 1)
		for(const auto &c : codes)
			if([name characterAtIndex:0] == (unichar)c.character)
				code = c.code;
	PLK_LOGI("--keys: %s%s", name.UTF8String, _window.isKeyWindow ? "" : " (the window is not the key window)");
	for(const NSEventType type : { NSEventTypeKeyDown, NSEventTypeKeyUp })
	{
		NSEvent *event = [NSEvent keyEventWithType:type
										  location:NSZeroPoint
									 modifierFlags:flags
										 timestamp:NSProcessInfo.processInfo.systemUptime
									  windowNumber:_window.windowNumber
										   context:nil
										characters:characters
					   charactersIgnoringModifiers:characters
										 isARepeat:NO
										   keyCode:code];
		[NSApp postEvent:event atStart:NO];
	}
}

#pragma mark - Ending

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app
{
	return NO; // windowWillClose: asks for termination itself, exactly once
}

- (void)windowWillClose:(NSNotification *)note
{
	if(!_terminating)
		[NSApp terminate:nil];
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)app
{
	if(_cleanedUp)
		return NSTerminateNow;
	if(_terminating)
		return NSTerminateCancel; // the first request is already shutting down
	_terminating = YES;
	// SessionHost::stop() + shutdown(), the DualSense manager (effects off) and the stats
	// thread on a background thread; the pipeline on the main thread after that.
	Runner *runner = _runner.get();
	runner->requestStop();
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
		runner->shutdownWorkers();
		[self performSelectorOnMainThread:@selector(finishTermination)
							   withObject:nil
							waitUntilDone:NO
									modes:@[ NSRunLoopCommonModes, NSModalPanelRunLoopMode ]];
	});
	return NSTerminateLater;
}

- (void)finishTermination
{
	[_headroomTimer invalidate];
	_headroomTimer = nil;
	if(_screenObserver)
		[NSNotificationCenter.defaultCenter removeObserver:_screenObserver];
	_screenObserver = nil;
	for(id observer in _fullscreenObservers)
		[NSNotificationCenter.defaultCenter removeObserver:observer];
	_fullscreenObservers = nil;
	_runner->shutdownVideo();
	_exitCode = _runner->exitCode();
	if(_activity)
	{
		[NSProcessInfo.processInfo endActivity:_activity];
		_activity = nil;
	}
	_cleanedUp = YES;
	[NSApp replyToApplicationShouldTerminate:YES];
}

- (void)applicationWillTerminate:(NSNotification *)note
{
	Log::instance().close();
	fflush(stdout);
	fflush(stderr);
	// AppKit exits with 0 after this; keep the session's result.
	exit(_exitCode);
}

@end

int main(int argc, const char *argv[])
{
	@autoreleasepool
	{
		Options o;
		std::string err;
		if(!parseOptions(argc, argv, o, &err))
		{
			fprintf(stderr, "Pleikkari: %s\n\n%s", err.c_str(), usageText().c_str());
			return 2;
		}
		switch(o.command)
		{
			case Command::Help:
				fputs(usageText().c_str(), stdout);
				return 0;
			case Command::List:
				return listConsoles(o);
			case Command::Stream:
			case Command::Replay:
				break;
		}
		if(o.headless)
			return runHeadless(o);

		// What the Video menu was last left at, where this command line says nothing. Not
		// headless: those runs are measurements and take their settings from the command line.
		std::string saved_note;
		if(o.saved_choices)
			saved_note = applySavedChoices(loadSavedChoices(), o);

		NSApplication *app = NSApplication.sharedApplication;
		app.activationPolicy = NSApplicationActivationPolicyRegular;
		app.mainMenu = PLKMakeMainMenu(@"Pleikkari");
		PLKAppDelegate *delegate = [[PLKAppDelegate alloc] initWithOptions:o savedNote:saved_note];
		app.delegate = delegate;
		// SIGINT / SIGTERM quit the app the way Cmd-Q does: the session is closed, the
		// controllers' effects are switched off and AWDL comes back. Left to the default
		// action, a kill ended the process on the spot and none of that happened.
		signal(SIGINT, SIG_IGN);
		signal(SIGTERM, SIG_IGN);
		dispatch_source_t signals[2];
		const int numbers[2] = { SIGINT, SIGTERM };
		for(int i = 0; i < 2; i++)
		{
			const int number = numbers[i];
			signals[i] = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, (uintptr_t)number, 0, dispatch_get_main_queue());
			dispatch_source_set_event_handler(signals[i], ^{
				PLK_LOGI("%s: quitting", number == SIGINT ? "SIGINT" : "SIGTERM");
				[NSApp terminate:nil];
			});
			dispatch_resume(signals[i]);
		}
		[app run];
		return delegate.exitCode;
	}
}
