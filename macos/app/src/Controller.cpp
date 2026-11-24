// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "Controller.h"

#include <chiaki/controller.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace plk::app {

namespace {

double steadySeconds()
{
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}

const char *intensityName(ChiakiDualSenseEffectIntensity i)
{
	switch(i)
	{
		case Off: return "off";
		case Weak: return "weak";
		case Medium: return "medium";
		case Strong: return "strong";
	}
	return "?";
}

const char *connectionName(const dualsense::DeviceInfo &info)
{
	return info.connection == dualsense::Connection::Bluetooth ? "Bluetooth" : "USB";
}

const char *modelName(const dualsense::DeviceInfo &info)
{
	return info.model == dualsense::Model::DualSenseEdge ? "DualSense Edge" : "DualSense";
}

} // namespace

uint8_t encodeMotorPower(ChiakiDualSenseEffectIntensity trigger, ChiakiDualSenseEffectIntensity rumble)
{
	// StreamSession's CHIAKI_EVENT_HAPTIC_INTENSITY / CHIAKI_EVENT_TRIGGER_INTENSITY cases.
	uint8_t t = 0x00;
	switch(trigger)
	{
		case Off: t = 0xF0; break;
		case Strong: t = 0x00; break;
		case Weak: t = 0x90; break;
		case Medium: t = 0x60; break;
	}
	uint8_t r = 0x00;
	switch(rumble)
	{
		case Off: r = 0x0F; break;
		case Strong: r = 0x00; break;
		case Weak: r = 0x03; break;
		case Medium: r = 0x02; break;
	}
	return (uint8_t)(t | r);
}

struct ControllerHub::Pad
{
	PadMapper mapper;
	uint32_t motion_reset_gen = 0;
	uint64_t registry_id = 0;
};

struct ControllerHub::Out
{
	std::shared_ptr<dualsense::Device> device;
	uint64_t registry_id = 0;
	bool bluetooth = false;
	bool rumble_suppressed = false;
	bool haptics_failed_logged = false;
};

ControllerHub::ControllerHub()
{
	chiaki_controller_state_set_idle(&merged_);
	usb_queue_ = dispatch_queue_create("plk.usb-haptics",
			dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
}

ControllerHub::~ControllerHub()
{
	stop();
	dispatch_sync(usb_queue_, ^{});
	dispatch_release(usb_queue_);
}

bool ControllerHub::start(const Config &config, StateSink sink, LogSink log)
{
	if(started_)
		return true;
	config_ = config;
	sink_ = std::move(sink);
	log_ = std::move(log);
	manager_ = std::make_unique<dualsense::Manager>();

	dualsense::Manager::Options options;
	options.allow_output = config.output;
	options.realtime_threads = config.realtime_threads;
	dualsense::Manager::Callbacks cb;
	cb.connected = [this](const std::shared_ptr<dualsense::Device> &d) { onConnected(d); };
	cb.disconnected = [this](const std::shared_ptr<dualsense::Device> &d) { onDisconnected(d); };
	cb.input = [this](dualsense::Device &d, const dualsense::InputState &in) { onInput(d, in); };
	cb.log = [this](const char *message) {
		if(log_)
			log_('I', std::string("DualSense: ") + message);
	};
	if(!manager_->Start(options, std::move(cb)))
	{
		if(log_)
			log_('E', "DualSense: the HID manager did not start");
		manager_.reset();
		return false;
	}
	started_ = true;
	last_stats_time_ = steadySeconds();
	if(log_)
		log_('I', std::string("DualSense: HID manager running, output ") + (config.output ? "on" : "off")
				+ ", real-time threads " + (config.realtime_threads ? "on" : "off"));
	return true;
}

void ControllerHub::stop()
{
	if(!started_)
		return;
	started_ = false;
	{
		std::lock_guard<std::mutex> lk(out_mutex_);
		session_active_ = false;
		updateUsbHapticsLocked();
	}
	dispatch_sync(usb_queue_, ^{}); // the USB haptics output is closed after this
	// Stop() stops haptics, sends the effects-off report and joins the HID thread; after
	// it no callback runs, so pads_ and outs_ can be dropped without the HID thread.
	manager_->Stop();
	{
		std::lock_guard<std::mutex> lk(out_mutex_);
		outs_.clear();
	}
	pads_.clear();
	manager_.reset();
}

size_t ControllerHub::deviceCount() const
{
	std::lock_guard<std::mutex> lk(out_mutex_);
	return outs_.size();
}

void ControllerHub::onConnected(const std::shared_ptr<dualsense::Device> &device)
{
	const dualsense::DeviceInfo &info = device->info();
	auto pad = std::make_unique<Pad>();
	pad->registry_id = info.registry_id;
	pad->motion_reset_gen = motion_reset_gen_.load();
	pads_[info.registry_id] = std::move(pad);

	if(log_)
	{
		char buf[256];
		snprintf(buf, sizeof(buf), "DualSense: connected %s over %s, firmware %s%04x, %s rumble, calibration %s",
				modelName(info), connectionName(info), info.firmware_valid ? "0x" : "unknown ",
				info.firmware_valid ? info.update_version : 0, info.rumble_v2 ? "current" : "legacy",
				info.calibration_valid ? "read" : "nominal");
		log_('I', buf);
	}

	auto out = std::make_unique<Out>();
	out->device = device;
	out->registry_id = info.registry_id;
	out->bluetooth = info.connection == dualsense::Connection::Bluetooth;
	std::lock_guard<std::mutex> lk(out_mutex_);
	if(config_.output && device->output_enabled())
	{
		applyInitialEffects(*out);
		if(session_active_)
			startHapticsLocked(*out);
	}
	outs_.push_back(std::move(out));
	updateUsbHapticsLocked();
}

void ControllerHub::onDisconnected(const std::shared_ptr<dualsense::Device> &device)
{
	const uint64_t id = device->info().registry_id;
	pads_.erase(id);
	{
		std::lock_guard<std::mutex> lk(out_mutex_);
		outs_.erase(std::remove_if(outs_.begin(), outs_.end(), [&](const std::unique_ptr<Out> &o) { return o->registry_id == id; }),
				outs_.end());
		updateUsbHapticsLocked();
	}
	if(log_)
		log_('I', std::string("DualSense: disconnected (") + connectionName(device->info()) + ")");
	// Nothing held down by a controller that is gone.
	pushMergedState();
}

void ControllerHub::onInput(dualsense::Device &device, const dualsense::InputState &in)
{
	auto it = pads_.find(device.info().registry_id);
	if(it == pads_.end())
		return;
	Pad &pad = *it->second;
	const uint32_t gen = motion_reset_gen_.load(std::memory_order_acquire);
	if(pad.motion_reset_gen != gen)
	{
		pad.mapper.resetMotion();
		pad.motion_reset_gen = gen;
	}
	pad.mapper.apply(in);
	input_reports_.fetch_add(1, std::memory_order_relaxed);
	const uint32_t step = pad.mapper.lastMotionStepUs();
	uint32_t prev = max_motion_step_us_.load(std::memory_order_relaxed);
	while(step > prev && !max_motion_step_us_.compare_exchange_weak(prev, step, std::memory_order_relaxed))
		;
	if(pads_.size() == 1)
	{
		if(sink_)
			sink_(pad.mapper.state());
		return;
	}
	pushMergedState();
}

void ControllerHub::pushMergedState()
{
	ChiakiControllerState state;
	chiaki_controller_state_set_idle(&state);
	bool first = true;
	for(auto &kv : pads_)
	{
		ChiakiControllerState s = kv.second->mapper.state();
		if(first)
		{
			state = s;
			first = false;
		}
		else
			chiaki_controller_state_or(&state, &state, &s);
	}
	merged_ = state;
	if(sink_)
		sink_(merged_);
}

void ControllerHub::applyInitialEffects(Out &out)
{
	dualsense::Device &d = *out.device;
	// The Qt app opens a DualSense muted: mic LED on, mute bits 0x08.
	d.SetMic(dualsense::MicLed::On, 0x08);
	d.SetMotorPowerLevel(encodeMotorPower(trigger_intensity_, haptic_intensity_));
	if(have_led_)
		d.SetLightbar(led_[0], led_[1], led_[2]);
	if(player_index_ >= 0)
		d.SetPlayerLeds(dualsense::Device::PlayerLedsForIndex(player_index_));
}

void ControllerHub::startHapticsLocked(Out &out)
{
	if(!config_.output || !config_.bt_haptics || !out.bluetooth || out.device->haptics_running())
		return;
	const dualsense::HapticsConfig hc = dualsense::HapticsConfig::FromEnvironment();
	const bool ok = out.device->StartHaptics(hc);
	out.rumble_suppressed = false;
	if(log_)
	{
		if(ok)
			log_('I', std::string("DualSense BT haptics: started; console rumble ")
					+ (config_.keep_rumble_with_haptics ? "kept" : "suppressed") + " while haptics play");
		else
			log_('W', "DualSense BT haptics: could not start, console rumble only");
	}
}

void ControllerHub::setSessionActive(bool active)
{
	std::lock_guard<std::mutex> lk(out_mutex_);
	if(session_active_ == active)
		return;
	session_active_ = active;
	for(auto &o : outs_)
	{
		if(!o->device->output_enabled())
			continue;
		if(active)
			startHapticsLocked(*o);
		else
		{
			if(o->device->haptics_running())
				o->device->StopHaptics();
			// Nothing updates the effects while no session runs (a restart for HDR takes
			// seconds): a rumble or trigger effect that was on would stay on.
			o->device->ClearEffects();
			o->rumble_suppressed = false;
		}
	}
	updateUsbHapticsLocked();
}

void ControllerHub::updateUsbHapticsLocked()
{
	bool usb_pad = false;
	for(auto &o : outs_)
		if(!o->bluetooth && o->device->output_enabled())
			usb_pad = true;
	const bool want = config_.output && config_.usb_haptics && session_active_ && usb_pad;
	if(want == usb_wanted_)
		return;
	usb_wanted_ = want;
	const uint64_t generation = ++usb_generation_;
	if(!want)
	{
		// Close off this lock: stopping an AUHAL waits for its IO thread.
		std::shared_ptr<UsbHaptics> closing(usb_haptics_.release());
		if(closing)
			dispatch_async(usb_queue_, ^{
				closing->close();
				if(log_)
					log_('I', "DualSense USB haptics: closed");
			});
		return;
	}
	dispatch_async(usb_queue_, ^{
		// CoreAudio device lookup and AUHAL start take tens of ms: never on the HID thread.
		const std::vector<AudioDeviceInfo> devices = findDualSenseAudioDevices();
		if(devices.empty())
		{
			if(log_)
				log_('W', "DualSense USB haptics: no DualSense audio device found, console rumble only");
			return;
		}
		const AudioDeviceInfo &dev = devices.front();
		auto haptics = std::make_unique<UsbHaptics>();
		std::string err;
		if(!haptics->open(dev, &err))
		{
			if(log_)
				log_('W', "DualSense USB haptics: cannot open \"" + dev.name + "\": " + err);
			return;
		}
		if(log_)
		{
			log_('I', "DualSense USB haptics: \"" + dev.name + "\", " + std::to_string(dev.output_channels)
					+ " output channels");
			if(dev.output_channels < 4)
				log_('W', "DualSense USB haptics: the device has fewer than 4 channels, so the actuators get "
						"nothing; set it to 4 channels in Audio MIDI Setup");
		}
		std::unique_ptr<UsbHaptics> stale;
		{
			std::lock_guard<std::mutex> lk(out_mutex_);
			if(usb_generation_ == generation && usb_wanted_)
				usb_haptics_ = std::move(haptics);
			else
				stale = std::move(haptics); // the session ended meanwhile
		}
		if(stale)
			stale->close();
	});
}

void ControllerHub::onRumble(uint8_t left, uint8_t right)
{
	rumble_events_.fetch_add(1, std::memory_order_relaxed);
	std::lock_guard<std::mutex> lk(out_mutex_);
	if(haptic_intensity_ == Off)
	{
		rumble_dropped_.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	for(auto &o : outs_)
	{
		dualsense::Device &d = *o->device;
		if(!d.output_enabled())
			continue;
		const bool suppress = !config_.keep_rumble_with_haptics && o->bluetooth && d.haptics_running()
				&& d.HapticsRecentlyNonSilent(250);
		if(suppress)
		{
			if(!o->rumble_suppressed)
				d.SetRumble(0, 0);
			o->rumble_suppressed = true;
			rumble_suppressed_.fetch_add(1, std::memory_order_relaxed);
			continue;
		}
		o->rumble_suppressed = false;
		d.SetRumble(left, right);
	}
}

void ControllerHub::onTriggerEffects(const ChiakiTriggerEffectsEvent &e)
{
	trigger_events_.fetch_add(1, std::memory_order_relaxed);
	uint8_t left[11], right[11];
	left[0] = e.type_left;
	memcpy(left + 1, e.left, 10);
	right[0] = e.type_right;
	memcpy(right + 1, e.right, 10);
	const uint8_t mask = e.mask & 0x03;
	std::lock_guard<std::mutex> lk(out_mutex_);
	const bool drop = trigger_intensity_ == Off || !mask;
	if(config_.trigger_log && log_)
	{
		char buf[160];
		snprintf(buf, sizeof(buf), "DualSense trigger effects %s: mask %#x L2 %#04x R2 %#04x, %zu controller(s)",
				drop ? "dropped" : "applied", e.mask, e.type_left, e.type_right, drop ? (size_t)0 : outs_.size());
		log_('I', buf);
	}
	if(drop)
	{
		trigger_dropped_.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	for(auto &o : outs_)
		if(o->device->output_enabled())
			o->device->SetTriggerEffects(left, right, mask);
}

void ControllerHub::onHapticIntensity(ChiakiDualSenseEffectIntensity intensity)
{
	std::lock_guard<std::mutex> lk(out_mutex_);
	haptic_intensity_ = intensity;
	const uint8_t power = encodeMotorPower(trigger_intensity_, haptic_intensity_);
	for(auto &o : outs_)
		if(o->device->output_enabled())
		{
			o->device->SetMotorPowerLevel(power);
			if(intensity == Off)
				o->device->SetRumble(0, 0);
		}
	if(log_)
	{
		char buf[96];
		snprintf(buf, sizeof(buf), "DualSense: haptic intensity %s, motor power %#04x", intensityName(intensity), power);
		log_('I', buf);
	}
}

void ControllerHub::onTriggerIntensity(ChiakiDualSenseEffectIntensity intensity)
{
	std::lock_guard<std::mutex> lk(out_mutex_);
	trigger_intensity_ = intensity;
	const uint8_t power = encodeMotorPower(trigger_intensity_, haptic_intensity_);
	for(auto &o : outs_)
		if(o->device->output_enabled())
			o->device->SetMotorPowerLevel(power);
	if(log_)
	{
		char buf[96];
		snprintf(buf, sizeof(buf), "DualSense: trigger intensity %s, motor power %#04x", intensityName(intensity), power);
		log_('I', buf);
	}
}

void ControllerHub::onLedColor(const uint8_t rgb[3])
{
	std::lock_guard<std::mutex> lk(out_mutex_);
	memcpy(led_, rgb, 3);
	have_led_ = true;
	for(auto &o : outs_)
		if(o->device->output_enabled())
			o->device->SetLightbar(rgb[0], rgb[1], rgb[2]);
}

void ControllerHub::onPlayerIndex(uint8_t index)
{
	std::lock_guard<std::mutex> lk(out_mutex_);
	player_index_ = index;
	const uint8_t leds = dualsense::Device::PlayerLedsForIndex(index);
	for(auto &o : outs_)
		if(o->device->output_enabled())
			o->device->SetPlayerLeds(leds);
}

void ControllerHub::onMotionReset()
{
	motion_reset_gen_.fetch_add(1, std::memory_order_release);
	if(log_)
		log_('I', "DualSense: motion reset requested by the console");
}

void ControllerHub::onHapticsFrame(const uint8_t *buf, size_t size)
{
	if(!size || size % (2 * sizeof(int16_t)))
		return;
	haptics_frames_.fetch_add(1, std::memory_order_relaxed);
	// The lib's buffer has no alignment guarantee for int16.
	int16_t stack[256];
	std::vector<int16_t> heap;
	const size_t samples = size / sizeof(int16_t);
	int16_t *pcm = stack;
	if(samples > sizeof(stack) / sizeof(stack[0]))
	{
		heap.resize(samples);
		pcm = heap.data();
	}
	memcpy(pcm, buf, samples * sizeof(int16_t));
	std::lock_guard<std::mutex> lk(out_mutex_);
	if(haptic_intensity_ == Off)
		return;
	if(usb_haptics_)
		usb_haptics_->push(pcm, samples, config_.haptic_gain);
	for(auto &o : outs_)
	{
		dualsense::Device &d = *o->device;
		if(!d.haptics_running())
		{
			if(o->bluetooth && !o->haptics_failed_logged && d.haptics_stats().failed && log_)
			{
				log_('W', "DualSense BT haptics: the controller stopped accepting reports");
				o->haptics_failed_logged = true;
			}
			continue;
		}
		d.PushHapticsPcm16(pcm, samples, config_.haptic_gain);
	}
}

std::string ControllerHub::statsLine()
{
	const double now = steadySeconds();
	const uint64_t reports = input_reports_.load();
	const double dt = now - last_stats_time_;
	const double rate = dt > 0 ? (double)(reports - last_stats_reports_) / dt : 0;
	last_stats_reports_ = reports;
	last_stats_time_ = now;
	const uint32_t max_step = max_motion_step_us_.exchange(0);

	std::string line;
	char buf[512];
	snprintf(buf, sizeof(buf),
			"controller: devices=%zu reports=%llu rate=%.0f/s max_motion_step_ms=%.2f rumble=%llu (dropped %llu, "
			"suppressed %llu) triggers=%llu (dropped %llu) haptics_frames=%llu",
			deviceCount(), (unsigned long long)reports, rate, max_step / 1000.0,
			(unsigned long long)rumble_events_.load(), (unsigned long long)rumble_dropped_.load(),
			(unsigned long long)rumble_suppressed_.load(), (unsigned long long)trigger_events_.load(),
			(unsigned long long)trigger_dropped_.load(), (unsigned long long)haptics_frames_.load());
	line = buf;
	std::lock_guard<std::mutex> lk(out_mutex_);
	if(usb_haptics_)
		line += usb_haptics_->statsLine();
	for(auto &o : outs_)
	{
		const dualsense::Device &d = *o->device;
		const dualsense::InputStats is = d.input_stats();
		snprintf(buf, sizeof(buf), " | %s: in=%llu crc_err=%llu", o->bluetooth ? "BT" : "USB",
				(unsigned long long)is.reports, (unsigned long long)is.crc_errors);
		line += buf;
		if(d.output_enabled())
		{
			const dualsense::WriterStats ws = d.writer_stats();
			snprintf(buf, sizeof(buf), " out=%llu fail=%llu merged=%llu write_max_ms=%.2f write_avg_ms=%.2f",
					(unsigned long long)ws.writes, (unsigned long long)ws.write_failures,
					(unsigned long long)ws.window_waits, ws.max_write_ns / 1e6,
					ws.writes ? ws.total_write_ns / 1e6 / (double)ws.writes : 0.0);
			line += buf;
		}
		if(d.haptics_running())
		{
			const dualsense::HapticsStats hs = d.haptics_stats();
			snprintf(buf, sizeof(buf), " haptics=%llu underruns=%llu dropped_bytes=%llu", (unsigned long long)hs.reports,
					(unsigned long long)hs.underruns, (unsigned long long)hs.dropped_bytes);
			line += buf;
		}
	}
	return line;
}

} // namespace plk::app
