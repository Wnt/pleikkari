// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "PadMapper.h"

#include <algorithm>

namespace plk::app {

namespace {

// Bits ChiakiControllerState has (DualSenseHID's Button enum is the same set).
constexpr uint32_t kChiakiButtons = 0xffff;
constexpr uint16_t kTouchMaxX = 1919;
constexpr uint16_t kTouchMaxY = 1079;

} // namespace

PadMapper::PadMapper()
{
	chiaki_controller_state_set_idle(&state_);
	chiaki_orientation_tracker_init(&tracker_);
	chiaki_accel_new_zero_set_inactive(&accel_zero_, false);
	chiaki_accel_new_zero_set_inactive(&real_accel_, true);
}

void PadMapper::reset()
{
	for(auto &t : touch_)
	{
		if(t.down && t.chiaki_id >= 0)
			chiaki_controller_state_stop_touch(&state_, (uint8_t)t.chiaki_id);
		t = TouchSlot();
	}
	state_.buttons = 0;
	state_.l2_state = state_.r2_state = 0;
	state_.left_x = state_.left_y = state_.right_x = state_.right_y = 0;
}

void PadMapper::apply(const dualsense::InputState &in)
{
	state_.buttons = in.buttons & kChiakiButtons;
	state_.l2_state = in.l2_state;
	state_.r2_state = in.r2_state;
	state_.left_x = in.left_x;
	state_.left_y = in.left_y;
	state_.right_x = in.right_x;
	state_.right_y = in.right_y;

	// The simple Bluetooth report has no touchpad: its touches read as lifted.
	for(int i = 0; i < 2; i++)
	{
		const dualsense::Touch &t = in.touches[i];
		TouchSlot &slot = touch_[i];
		const uint16_t x = std::min(t.x, kTouchMaxX);
		const uint16_t y = std::min(t.y, kTouchMaxY);
		if(t.active)
		{
			if(slot.down && slot.contact == t.id)
			{
				if(slot.chiaki_id >= 0)
					chiaki_controller_state_set_touch_pos(&state_, (uint8_t)slot.chiaki_id, x, y);
				continue;
			}
			// A new contact (or one whose lift we never saw): end the old, start the new.
			if(slot.down && slot.chiaki_id >= 0)
				chiaki_controller_state_stop_touch(&state_, (uint8_t)slot.chiaki_id);
			slot.down = true;
			slot.contact = t.id;
			slot.chiaki_id = chiaki_controller_state_start_touch(&state_, x, y);
		}
		else if(slot.down)
		{
			if(slot.chiaki_id >= 0)
				chiaki_controller_state_stop_touch(&state_, (uint8_t)slot.chiaki_id);
			slot = TouchSlot();
		}
	}

	if(!in.motion_valid)
		return;
	// The tracker takes a 32-bit microsecond timestamp and handles its wrap itself.
	const uint32_t ts = (uint32_t)in.sensor_time_us;
	last_step_us_ = have_motion_ ? ts - last_motion_us_ : 0;
	chiaki_accel_new_zero_set_active(&real_accel_, in.accel_x, in.accel_y, in.accel_z, true);
	chiaki_orientation_tracker_update(&tracker_, in.gyro_x, in.gyro_y, in.gyro_z, in.accel_x, in.accel_y, in.accel_z,
			&accel_zero_, false, ts);
	chiaki_orientation_tracker_apply_to_controller_state(&tracker_, &state_);
	last_motion_us_ = ts;
	have_motion_ = true;
}

void PadMapper::resetMotion()
{
	chiaki_accel_new_zero_set_active(&accel_zero_, real_accel_.accel_x, real_accel_.accel_y, real_accel_.accel_z, false);
	chiaki_orientation_tracker_init(&tracker_);
	chiaki_orientation_tracker_update(&tracker_, state_.gyro_x, state_.gyro_y, state_.gyro_z, real_accel_.accel_x,
			real_accel_.accel_y, real_accel_.accel_z, &accel_zero_, false, last_motion_us_);
	chiaki_orientation_tracker_apply_to_controller_state(&tracker_, &state_);
}

} // namespace plk::app
