// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Pleikkari.app: native macOS PS5 Remote Play client (AppKit, no Qt/SDL/libplacebo).
// See macos/app/README.md.

#import "StreamWindow.h"

#include "Log.h"
#include "Options.h"
#include "Registration.h"
#include "Runner.h"

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

@interface PLKAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
- (instancetype)initWithOptions:(const Options &)options;
@property(nonatomic, readonly) int exitCode;
@end

@implementation PLKAppDelegate
{
	Options _options;
	std::unique_ptr<Runner> _runner;
	PLKStreamWindow *_window;
	id<NSObject> _activity;
	BOOL _terminating;
	BOOL _cleanedUp;
	int _exitCode;
}

- (instancetype)initWithOptions:(const Options &)options
{
	if((self = [super init]))
		_options = options;
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
	[_window.videoView reportDrawableSize];
	if(@available(macOS 14.0, *))
		[NSApp activate];
	else
		[NSApp activateIgnoringOtherApps:YES];
	[NSCursor setHiddenUntilMouseMoves:YES];
	if(_options.fullscreen)
		[_window toggleFullScreen:nil];

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
		[NSApp terminate:nil];
}

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

		NSApplication *app = NSApplication.sharedApplication;
		app.activationPolicy = NSApplicationActivationPolicyRegular;
		app.mainMenu = PLKMakeMainMenu(@"Pleikkari");
		PLKAppDelegate *delegate = [[PLKAppDelegate alloc] initWithOptions:o];
		app.delegate = delegate;
		[app run];
		return delegate.exitCode;
	}
}
