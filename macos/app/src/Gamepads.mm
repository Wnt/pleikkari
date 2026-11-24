// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "Gamepads.h"

#include "GamepadMapper.h"
#include "GamepadRumble.h"

#import <CoreHaptics/CoreHaptics.h>
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

// One rumble motor of one controller: a CoreHaptics engine for one place on the controller,
// playing an endless continuous event whose intensity is changed while it plays. Every
// method runs on the hub's haptics queue.
@interface PLKRumbleMotor : NSObject
- (instancetype)initWithController:(GCController *)controller
						  locality:(GCHapticsLocality)locality
							 queue:(dispatch_queue_t)queue;
/** 0 stops the motor. @return NO if the system refused */
- (BOOL)setIntensity:(float)intensity;
- (void)shutdown;
@end

@implementation PLKRumbleMotor
{
	CHHapticEngine *_engine;
	id<CHHapticPatternPlayer> _player;
	BOOL _playing;
	BOOL _stopped; // the system stopped the engine: started again when next needed
}

- (instancetype)initWithController:(GCController *)controller
						  locality:(GCHapticsLocality)locality
							 queue:(dispatch_queue_t)queue
{
	if(!(self = [super init]))
		return nil;
	_engine = [controller.haptics createEngineWithLocality:locality];
	if(!_engine)
		return nil;
	NSError *error = nil;
	if(![_engine startAndReturnError:&error])
		return nil;
	__weak PLKRumbleMotor *weak = self;
	// Both are called on a thread of the system's.
	_engine.stoppedHandler = ^(CHHapticEngineStoppedReason reason) {
		dispatch_async(queue, ^{
			PLKRumbleMotor *me = weak;
			if(!me)
				return;
			me->_player = nil;
			me->_playing = NO;
			me->_stopped = YES;
		});
	};
	_engine.resetHandler = ^{
		dispatch_async(queue, ^{
			PLKRumbleMotor *me = weak;
			if(!me)
				return;
			me->_player = nil;
			me->_playing = NO;
			me->_stopped = YES;
		});
	};
	return self;
}

- (BOOL)setIntensity:(float)intensity
{
	NSError *error = nil;
	if(!_engine)
		return NO;
	if(intensity <= 0)
	{
		BOOL ok = YES;
		if(_player && _playing)
			ok = [_player stopAtTime:CHHapticTimeImmediate error:&error];
		_playing = NO;
		return ok;
	}
	if(_stopped)
	{
		if(![_engine startAndReturnError:&error])
			return NO;
		_stopped = NO;
	}
	if(!_player)
	{
		CHHapticEventParameter *full =
				[[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:1.0f];
		CHHapticEvent *event = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous
															 parameters:@[full]
														   relativeTime:0
															   duration:GCHapticDurationInfinite];
		CHHapticPattern *pattern = [[CHHapticPattern alloc] initWithEvents:@[event] parameters:@[] error:&error];
		if(!pattern)
			return NO;
		_player = [_engine createPlayerWithPattern:pattern error:&error];
		if(!_player)
			return NO;
		_playing = NO;
	}
	CHHapticDynamicParameter *level =
			[[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl
															value:std::min(intensity, 1.0f)
													 relativeTime:0];
	if(![_player sendParameters:@[level] atTime:CHHapticTimeImmediate error:&error])
		return NO;
	if(!_playing)
	{
		if(![_player startAtTime:CHHapticTimeImmediate error:&error])
			return NO;
		_playing = YES;
	}
	return YES;
}

- (void)shutdown
{
	if(_player && _playing)
		[_player stopAtTime:CHHapticTimeImmediate error:nil];
	_player = nil;
	_playing = NO;
	_engine.stoppedHandler = ^(CHHapticEngineStoppedReason reason) {};
	_engine.resetHandler = ^{};
	[_engine stopWithCompletionHandler:nil];
	_engine = nil;
}

@end

namespace plk::app {

namespace {

double steadySeconds()
{
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// The system's own shortcuts on these buttons (Launchpad on Home, a screenshot on Share)
// would fire on top of what the console does with them.
void takeButtonFromSystem(GCControllerElement *element)
{
	if(element)
		element.preferredSystemGestureState = GCSystemGestureStateDisabled;
}

bool isDualSense(GCController *controller)
{
	return [controller.extendedGamepad isKindOfClass:GCDualSenseGamepad.class];
}

GamepadInput readGamepad(GCExtendedGamepad *pad)
{
	GamepadInput in;
	in.a = pad.buttonA.pressed;
	in.b = pad.buttonB.pressed;
	in.x = pad.buttonX.pressed;
	in.y = pad.buttonY.pressed;
	in.left_shoulder = pad.leftShoulder.pressed;
	in.right_shoulder = pad.rightShoulder.pressed;
	in.left_stick_click = pad.leftThumbstickButton.pressed;
	in.right_stick_click = pad.rightThumbstickButton.pressed;
	in.dpad_up = pad.dpad.up.pressed;
	in.dpad_down = pad.dpad.down.pressed;
	in.dpad_left = pad.dpad.left.pressed;
	in.dpad_right = pad.dpad.right.pressed;
	in.menu = pad.buttonMenu.pressed;
	in.options = pad.buttonOptions.pressed;
	in.home = pad.buttonHome.pressed;
	if([pad isKindOfClass:GCXboxGamepad.class])
		in.touchpad_click = ((GCXboxGamepad *)pad).buttonShare.pressed;
	else if([pad isKindOfClass:GCDualShockGamepad.class])
		in.touchpad_click = ((GCDualShockGamepad *)pad).touchpadButton.pressed;
	in.left_trigger = pad.leftTrigger.value;
	in.right_trigger = pad.rightTrigger.value;
	in.left_x = pad.leftThumbstick.xAxis.value;
	in.left_y = pad.leftThumbstick.yAxis.value;
	in.right_x = pad.rightThumbstick.xAxis.value;
	in.right_y = pad.rightThumbstick.yAxis.value;
	return in;
}

} // namespace

struct GamepadHub::Impl
{
	struct Pad
	{
		GCController *controller = nil;
		ChiakiControllerState state;
		bool seen_input = false;
	};

	// A controller's motors. With both handles, low is the left one and high the right;
	// a controller that only offers one place gets the stronger of the two in low.
	struct RumblePad
	{
		GCController *controller = nil;
		PLKRumbleMotor *low = nil;
		PLKRumbleMotor *high = nil;
	};

	dispatch_queue_t queue = nullptr; // input handlers, the sink and pads
	std::vector<Pad> pads;            // on queue
	StateSink sink;
	LogSink log;
	Config config;
	id connectObserver = nil;
	id disconnectObserver = nil;
	bool started = false;
	std::atomic<size_t> devices{0};
	std::atomic<uint64_t> changes{0};

	// Rumble. Its own queue: a slow answer from the haptics service must not hold up input.
	dispatch_queue_t hapticsQueue = nullptr;
	dispatch_source_t rumbleTimer = nullptr;
	std::vector<RumblePad> rumblePads; // on hapticsQueue
	int appliedLow = 0, appliedHigh = 0; // on hapticsQueue, in hundredths
	std::mutex rumbleMutex;
	RumbleMixer mixer;          // under rumbleMutex
	bool sessionActive = false; // under rumbleMutex
	std::atomic<bool> rumbleOn{false}; // output wanted and not stopped
	std::atomic<size_t> motors{0};
	std::atomic<uint64_t> rumbleUpdates{0};
	std::atomic<uint64_t> rumbleErrors{0};
	std::atomic<uint64_t> hapticsFrames{0};

	// On hapticsQueue. force: a controller has just arrived and knows nothing yet.
	void applyRumble(bool force)
	{
		RumbleMixer::Motors m;
		{
			std::lock_guard<std::mutex> lk(rumbleMutex);
			if(sessionActive && rumbleOn.load())
				m = mixer.motors(steadySeconds());
		}
		const int low = (int)std::lround(m.low * 100.0f);
		const int high = (int)std::lround(m.high * 100.0f);
		if(!force && low == appliedLow && high == appliedHigh)
			return;
		appliedLow = low;
		appliedHigh = high;
		if(rumblePads.empty())
			return;
		for(RumblePad &p : rumblePads)
		{
			BOOL ok = YES;
			if(p.high)
			{
				ok = [p.low setIntensity:low / 100.0f];
				ok = [p.high setIntensity:high / 100.0f] && ok;
			}
			else
				ok = [p.low setIntensity:std::max(low, high) / 100.0f];
			if(!ok)
				rumbleErrors.fetch_add(1, std::memory_order_relaxed);
		}
		rumbleUpdates.fetch_add(1, std::memory_order_relaxed);
	}

	void kickRumble()
	{
		if(!rumbleOn.load())
			return;
		Impl *impl = this;
		dispatch_async(hapticsQueue, ^{
			impl->applyRumble(false);
		});
	}

	// Main thread.
	void addRumble(GCController *controller, NSString *name)
	{
		if(!config.output)
			return;
		GCDeviceHaptics *haptics = controller.haptics;
		if(!haptics)
		{
			say('I', [NSString stringWithFormat:@"%@ has no rumble", name]);
			return;
		}
		Impl *impl = this;
		dispatch_async(hapticsQueue, ^{
			for(const RumblePad &p : impl->rumblePads)
				if(p.controller == controller)
					return;
			NSSet<GCHapticsLocality> *places = haptics.supportedLocalities;
			RumblePad pad;
			pad.controller = controller;
			NSString *what = nil;
			if([places containsObject:GCHapticsLocalityLeftHandle] && [places containsObject:GCHapticsLocalityRightHandle])
			{
				pad.low = [[PLKRumbleMotor alloc] initWithController:controller
															locality:GCHapticsLocalityLeftHandle
															   queue:impl->hapticsQueue];
				pad.high = [[PLKRumbleMotor alloc] initWithController:controller
															 locality:GCHapticsLocalityRightHandle
																queue:impl->hapticsQueue];
				what = @"rumble in both handles";
				if(!pad.low || !pad.high)
				{
					[pad.low shutdown];
					[pad.high shutdown];
					pad.low = pad.high = nil;
				}
			}
			if(!pad.low)
			{
				GCHapticsLocality place = [places containsObject:GCHapticsLocalityHandles] ? GCHapticsLocalityHandles
																						   : GCHapticsLocalityDefault;
				pad.low = [[PLKRumbleMotor alloc] initWithController:controller locality:place queue:impl->hapticsQueue];
				what = @"one rumble motor";
			}
			if(!pad.low)
			{
				impl->say('W', [NSString stringWithFormat:@"%@: the haptics engine did not start, no rumble", name]);
				return;
			}
			impl->rumblePads.push_back(pad);
			size_t count = 0;
			for(const RumblePad &p : impl->rumblePads)
				count += p.high ? 2 : 1;
			impl->motors = count;
			impl->say('I', [NSString stringWithFormat:@"%@: %@", name, what]);
			impl->applyRumble(true);
		});
	}

	// Main thread.
	void removeRumble(GCController *controller)
	{
		Impl *impl = this;
		dispatch_async(hapticsQueue, ^{
			for(size_t i = 0; i < impl->rumblePads.size(); i++)
				if(impl->rumblePads[i].controller == controller)
				{
					[impl->rumblePads[i].low shutdown];
					[impl->rumblePads[i].high shutdown];
					impl->rumblePads.erase(impl->rumblePads.begin() + (long)i);
					break;
				}
			size_t count = 0;
			for(const RumblePad &p : impl->rumblePads)
				count += p.high ? 2 : 1;
			impl->motors = count;
		});
	}

	void say(char level, NSString *text)
	{
		if(log)
			log(level, std::string("Gamepad: ") + text.UTF8String);
	}

	// On queue.
	void push()
	{
		ChiakiControllerState merged;
		chiaki_controller_state_set_idle(&merged);
		for(Pad &p : pads)
			chiaki_controller_state_or(&merged, &merged, &p.state);
		if(sink)
			sink(merged);
	}

	// Main thread.
	void add(GCController *controller)
	{
		NSString *name = controller.vendorName ?: @"controller";
		if(isDualSense(controller))
			return; // ControllerHub's
		GCExtendedGamepad *gamepad = controller.extendedGamepad;
		if(!gamepad)
		{
			say('I', [NSString stringWithFormat:@"%@ has no standard layout, ignored", name]);
			return;
		}
		say('I', [NSString stringWithFormat:@"%@ connected (%@)", name, controller.productCategory]);
		takeButtonFromSystem(gamepad.buttonHome);
		takeButtonFromSystem(gamepad.buttonMenu);
		takeButtonFromSystem(gamepad.buttonOptions);
		if([gamepad isKindOfClass:GCXboxGamepad.class])
			takeButtonFromSystem(((GCXboxGamepad *)gamepad).buttonShare);
		addRumble(controller, name);
		Impl *impl = this;
		// Known to the queue before its first input can arrive there.
		dispatch_async(queue, ^{
			for(const Pad &p : impl->pads)
				if(p.controller == controller)
					return;
			Pad pad;
			pad.controller = controller;
			chiaki_controller_state_set_idle(&pad.state);
			impl->pads.push_back(pad);
			impl->devices = impl->pads.size();
		});
		// Handlers on our queue, not the main thread: a busy UI must not delay a button.
		controller.handlerQueue = queue;
		gamepad.valueChangedHandler = ^(GCExtendedGamepad *pad, GCControllerElement *element) {
			const ChiakiControllerState state = mapGamepad(readGamepad(pad));
			for(Pad &p : impl->pads)
				if(p.controller == pad.controller)
				{
					p.state = state;
					if(!p.seen_input)
					{
						p.seen_input = true;
						impl->say('I', [NSString stringWithFormat:@"first input from %@",
															   pad.controller.vendorName ?: @"controller"]);
					}
				}
			impl->changes.fetch_add(1, std::memory_order_relaxed);
			impl->push();
		};
	}

	// Main thread.
	void remove(GCController *controller)
	{
		controller.extendedGamepad.valueChangedHandler = nil;
		removeRumble(controller);
		Impl *impl = this;
		NSString *name = controller.vendorName ?: @"controller";
		dispatch_async(queue, ^{
			bool found = false;
			for(size_t i = 0; i < impl->pads.size(); i++)
				if(impl->pads[i].controller == controller)
				{
					impl->pads.erase(impl->pads.begin() + (long)i);
					found = true;
					break;
				}
			if(!found)
				return;
			impl->devices = impl->pads.size();
			impl->say('I', [NSString stringWithFormat:@"%@ disconnected", name]);
			// Whatever it held down is released.
			impl->push();
		});
	}
};

GamepadHub::GamepadHub() : impl_(new Impl)
{
	impl_->queue = dispatch_queue_create("plk.gamepads",
			dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
	impl_->hapticsQueue = dispatch_queue_create("plk.gamepads.rumble",
			dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0));
}

// Main thread. The controllers' handlers point at impl_: they are taken off, and one that
// is already on the queue runs out, before it is freed.
GamepadHub::~GamepadHub()
{
	stop();
	Impl *impl = impl_;
	NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
	if(impl->connectObserver)
		[center removeObserver:impl->connectObserver];
	if(impl->disconnectObserver)
		[center removeObserver:impl->disconnectObserver];
	for(GCController *controller in GCController.controllers)
		controller.extendedGamepad.valueChangedHandler = nil;
	dispatch_sync(impl->queue, ^{
		impl->pads.clear();
	});
	if(impl->rumbleTimer)
		dispatch_source_cancel(impl->rumbleTimer);
	// Whatever was queued for the motors, a timer tick included, runs out here.
	dispatch_sync(impl->hapticsQueue, ^{
		for(Impl::RumblePad &p : impl->rumblePads)
		{
			[p.low shutdown];
			[p.high shutdown];
		}
		impl->rumblePads.clear();
	});
	delete impl; // the queues go with it (ARC)
}

void GamepadHub::start(const Config &config, StateSink sink, LogSink log)
{
	Impl *impl = impl_;
	if(impl->started)
		return;
	impl->started = true;
	impl->config = config;
	impl->sink = std::move(sink);
	impl->log = std::move(log);
	if(config.output)
	{
		impl->rumbleOn = true;
		// The haptics stream ends without a last word: the tick is what notices, and what
		// follows its loudness while it runs. Rumble events do not wait for it.
		impl->rumbleTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, impl->hapticsQueue);
		dispatch_source_set_timer(impl->rumbleTimer, dispatch_time(DISPATCH_TIME_NOW, 0), 10 * NSEC_PER_MSEC,
				2 * NSEC_PER_MSEC);
		dispatch_source_set_event_handler(impl->rumbleTimer, ^{
			if(impl->motors.load(std::memory_order_relaxed))
				impl->applyRumble(false);
		});
		dispatch_resume(impl->rumbleTimer);
	}
	// The stream is controlled while another app is in front too.
	GCController.shouldMonitorBackgroundEvents = YES;
	NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
	impl->connectObserver = [center addObserverForName:GCControllerDidConnectNotification
												object:nil
												 queue:NSOperationQueue.mainQueue
											usingBlock:^(NSNotification *note) {
												impl->add((GCController *)note.object);
											}];
	impl->disconnectObserver = [center addObserverForName:GCControllerDidDisconnectNotification
												   object:nil
													queue:NSOperationQueue.mainQueue
											   usingBlock:^(NSNotification *note) {
												   impl->remove((GCController *)note.object);
											   }];
	for(GCController *controller in GCController.controllers)
		impl->add(controller);
	impl->say('I', @"listening for controllers other than the DualSense");
}

void GamepadHub::stop()
{
	Impl *impl = impl_;
	// A handler that is running now finishes before this returns; none after it has a
	// sink to call. The framework's objects are not touched here: this may be any thread.
	dispatch_sync(impl->queue, ^{
		impl->sink = nullptr;
	});
	impl->rumbleOn = false;
	dispatch_sync(impl->hapticsQueue, ^{
		impl->applyRumble(false); // rumbleOn is off: every motor to rest
	});
}

void GamepadHub::setSessionActive(bool active)
{
	{
		std::lock_guard<std::mutex> lk(impl_->rumbleMutex);
		impl_->sessionActive = active;
		if(!active)
			impl_->mixer.reset();
	}
	impl_->kickRumble();
}

void GamepadHub::onRumble(uint8_t left, uint8_t right)
{
	{
		std::lock_guard<std::mutex> lk(impl_->rumbleMutex);
		impl_->mixer.setRumble(left, right);
	}
	impl_->kickRumble();
}

void GamepadHub::onHapticsFrame(const uint8_t *buf, size_t size)
{
	// Interleaved 16-bit stereo; 10 ms at 3 kHz is 30 frames.
	int16_t pcm[2 * 240];
	const size_t frames = std::min(size / 4, sizeof(pcm) / sizeof(pcm[0]) / 2);
	if(!buf || !frames)
		return;
	memcpy(pcm, buf, frames * 4);
	impl_->hapticsFrames.fetch_add(1, std::memory_order_relaxed);
	std::lock_guard<std::mutex> lk(impl_->rumbleMutex);
	impl_->mixer.pushHaptics(pcm, frames, steadySeconds());
}

void GamepadHub::onHapticIntensity(ChiakiDualSenseEffectIntensity intensity)
{
	{
		std::lock_guard<std::mutex> lk(impl_->rumbleMutex);
		impl_->mixer.setIntensity(intensity);
	}
	impl_->kickRumble();
}

std::string GamepadHub::statsLine() const
{
	char buf[192];
	snprintf(buf, sizeof(buf), "gamepads: devices=%zu changes=%llu rumble_motors=%zu rumble_updates=%llu (errors %llu) haptics_frames=%llu",
			impl_->devices.load(), (unsigned long long)impl_->changes.load(), impl_->motors.load(),
			(unsigned long long)impl_->rumbleUpdates.load(), (unsigned long long)impl_->rumbleErrors.load(),
			(unsigned long long)impl_->hapticsFrames.load());
	return buf;
}

size_t GamepadHub::deviceCount() const
{
	return impl_->devices.load();
}

} // namespace plk::app
