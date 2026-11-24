// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "EffectsWriter.h"
#include "ThreadUtil.h"

#include <chrono>
#include <cstring>

namespace plk::dualsense {

using namespace protocol;

EffectsWriter::EffectsWriter(OutputSink *sink, Config config)
	: sink_(sink), config_(config), led_phase_(config.bluetooth ? LedPhase::Hold : LedPhase::Ready)
{
}

EffectsWriter::~EffectsWriter()
{
	Stop(false);
}

void EffectsWriter::Start()
{
	if(thread_.joinable())
		return;
	thread_ = std::thread([this] { Run(); });
}

void EffectsWriter::Stop(bool send_off)
{
	{
		std::lock_guard<std::mutex> lk(mutex_);
		if(stop_ || !thread_.joinable())
			return;
		stop_ = true;
		send_off_ = send_off;
	}
	cond_.notify_all();
	thread_.join();
}

// `change` edits the state and returns the groups it changed (0 = no change).
template <class F> void EffectsWriter::Modify(F &&change)
{
	{
		std::lock_guard<std::mutex> lk(mutex_);
		if(stop_)
			return;
		stats_.updates++;
		uint32_t changed = change(state_);
		if(!changed)
			return;
		dirty_ |= changed;
	}
	cond_.notify_one();
}

void EffectsWriter::SetRumble(uint8_t left, uint8_t right)
{
	Modify([&](EffectsState &s) -> uint32_t {
		if((s.active & kGroupRumble) && s.rumble_left == left && s.rumble_right == right)
			return 0;
		s.active |= kGroupRumble;
		s.rumble_left = left;
		s.rumble_right = right;
		return kGroupRumble;
	});
}

void EffectsWriter::SetTriggerEffects(const uint8_t left[11], const uint8_t right[11])
{
	Modify([&](EffectsState &s) -> uint32_t {
		if((s.active & kGroupTriggers) && !memcmp(s.trigger_left, left, 11) && !memcmp(s.trigger_right, right, 11))
			return 0;
		s.active |= kGroupTriggers;
		memcpy(s.trigger_left, left, 11);
		memcpy(s.trigger_right, right, 11);
		return kGroupTriggers;
	});
}

void EffectsWriter::SetMotorPowerLevel(uint8_t value)
{
	// Like the GUI: stored, and sent with the next report carrying rumble or triggers.
	// If either is active already, that report goes out now.
	Modify([&](EffectsState &s) -> uint32_t {
		if(s.motor_power == value)
			return 0;
		s.motor_power = value;
		return kGroupMotorPower;
	});
}

void EffectsWriter::SetLightbar(uint8_t r, uint8_t g, uint8_t b)
{
	Modify([&](EffectsState &s) -> uint32_t {
		if((s.active & kGroupLightbar) && s.lightbar[0] == r && s.lightbar[1] == g && s.lightbar[2] == b)
			return 0;
		s.active |= kGroupLightbar;
		s.lightbar[0] = r;
		s.lightbar[1] = g;
		s.lightbar[2] = b;
		return kGroupLightbar;
	});
}

void EffectsWriter::SetPlayerLeds(uint8_t mask)
{
	Modify([&](EffectsState &s) -> uint32_t {
		if((s.active & kGroupPlayerLeds) && s.player_leds == mask)
			return 0;
		s.active |= kGroupPlayerLeds;
		s.player_leds = mask;
		return kGroupPlayerLeds;
	});
}

void EffectsWriter::SetMic(uint8_t led_mode, uint8_t power_save)
{
	Modify([&](EffectsState &s) -> uint32_t {
		const uint32_t groups = kGroupMicLed | kGroupPowerSave;
		if((s.active & groups) == groups && s.mic_led == led_mode && s.power_save == power_save)
			return 0;
		s.active |= groups;
		s.mic_led = led_mode;
		s.power_save = power_save;
		return groups;
	});
}

void EffectsWriter::ClearEffects()
{
	Modify([&](EffectsState &s) -> uint32_t {
		const EffectsState off = EffectsOff(s.motor_power);
		uint32_t changed = 0;
		if(!(s.active & kGroupRumble) || s.rumble_left || s.rumble_right)
			changed |= kGroupRumble;
		if(!(s.active & kGroupTriggers) || memcmp(s.trigger_left, off.trigger_left, 11) || memcmp(s.trigger_right, off.trigger_right, 11))
			changed |= kGroupTriggers;
		s.active |= kGroupRumble | kGroupTriggers;
		s.rumble_left = s.rumble_right = 0;
		memcpy(s.trigger_left, off.trigger_left, 11);
		memcpy(s.trigger_right, off.trigger_right, 11);
		return changed;
	});
}

void EffectsWriter::LedsReady()
{
	{
		std::lock_guard<std::mutex> lk(mutex_);
		if(led_phase_ != LedPhase::Hold)
			return;
		led_phase_ = LedPhase::Armed;
	}
	cond_.notify_one();
}

WriterStats EffectsWriter::stats() const
{
	std::lock_guard<std::mutex> lk(mutex_);
	return stats_;
}

EffectsState EffectsWriter::state() const
{
	std::lock_guard<std::mutex> lk(mutex_);
	return state_;
}

bool EffectsWriter::SendableLocked() const
{
	uint32_t sendable = state_.active;
	if(state_.active & (kGroupRumble | kGroupTriggers))
		sendable |= kGroupMotorPower;
	if(led_phase_ == LedPhase::Hold)
		sendable &= ~kGroupLeds;
	return (dirty_ & sendable) != 0;
}

void EffectsWriter::Run()
{
	SetupDriverThread("plk.ds.out", config_.time_constraint, 0, kWriterThreadComputationNs, kWriterThreadConstraintNs);
	uint8_t report[kBtOutputSize];
	std::unique_lock<std::mutex> lk(mutex_);
	for(;;)
	{
		cond_.wait(lk, [this] { return stop_ || SendableLocked(); });
		if(stop_)
			break;

		uint32_t include = state_.active;
		bool release = false;
		if(led_phase_ != LedPhase::Ready)
		{
			include &= ~kGroupLeds;
			if(led_phase_ == LedPhase::Armed && (dirty_ & state_.active & kGroupLeds))
			{
				// First LED report since the animation ended: release the LEDs from the
				// firmware now, send the values with the next report.
				release = true;
				led_phase_ = LedPhase::Ready;
			}
		}
		const size_t size = PackEffectsReport(state_, include, config_.rumble_v2, release,
			config_.bluetooth, seq_, report);
		uint32_t delivered = include;
		if(include & (kGroupRumble | kGroupTriggers))
			delivered |= kGroupMotorPower;
		dirty_ &= ~delivered;
		// A zero rumble has now been sent once: leave rumble out of later reports, so
		// the controller goes back to audio haptics (Controller::SetDualSenseRumble).
		if((include & kGroupRumble) && !state_.rumble_left && !state_.rumble_right)
			state_.active &= ~kGroupRumble;
		seq_ = (seq_ + 1) & 0x0F;
		attempted_ = true;

		lk.unlock();
		const auto t0 = std::chrono::steady_clock::now();
		const bool ok = sink_->Write(report, size);
		const uint64_t ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - t0).count();
		lk.lock();

		stats_.writes++;
		if(!ok)
			stats_.write_failures++;
		stats_.last_write_ns = ns;
		stats_.total_write_ns += ns;
		if(ns > stats_.max_write_ns)
			stats_.max_write_ns = ns;
	}

	if(send_off_ && attempted_)
	{
		const EffectsState off = EffectsOff(state_.motor_power);
		const size_t size = PackEffectsReport(off, off.active, config_.rumble_v2, false,
			config_.bluetooth, seq_, report);
		seq_ = (seq_ + 1) & 0x0F;
		lk.unlock();
		const bool ok = sink_->Write(report, size);
		lk.lock();
		stats_.writes++;
		if(!ok)
			stats_.write_failures++;
	}
}

} // namespace plk::dualsense
