// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// The effects writer: one thread per device that always writes the newest complete
// effects state, with at most that one state pending.
//
// Why not a queue: SDL (which the Qt GUI uses today) queues effect reports for a
// single global writer thread, merges a new report into a queued one only when their
// enable bits match, and sleeps 10 ms after every write (SDL_hidapi_rumble.c,
// "Make sure we're not starving report reads"). Over Bluetooth every write blocks, so
// a burst of rumble/trigger/LED changes is delivered late and one write apiece. Here
// the pending "queue" is the state itself: Set*() overwrites fields and marks their
// group dirty; the writer packs whatever is current when it is free. Three updates
// that land during one blocking write become exactly one following write.

#pragma once

#include "Protocol.h"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace plk::dualsense {

// Destination of output reports. Write() is blocking and may be called from several
// threads; implementations serialise as needed.
class OutputSink {
public:
	virtual ~OutputSink() = default;
	// One complete output report, report id in data[0].
	virtual bool Write(const uint8_t *data, size_t size) = 0;
};

class EffectsWriter {
public:
	struct Config {
		bool bluetooth = false;
		bool rumble_v2 = true;
		bool time_constraint = false; // Manager::Options::realtime_threads
	};

	EffectsWriter(OutputSink *sink, Config config);
	~EffectsWriter();

	void Start();
	// Stops the thread. With send_off, and if this writer has written anything, it
	// first writes protocol::EffectsOff() (rumble 0, both triggers off).
	void Stop(bool send_off);

	// Thread-safe and non-blocking. A call that leaves the state unchanged writes
	// nothing.
	void SetRumble(uint8_t left, uint8_t right);
	void SetTriggerEffects(const uint8_t left[11], const uint8_t right[11]);
	void SetMotorPowerLevel(uint8_t value);
	void SetLightbar(uint8_t r, uint8_t g, uint8_t b);
	void SetPlayerLeds(uint8_t mask);
	void SetMic(uint8_t led_mode, uint8_t power_save);
	void ClearEffects();

	// Bluetooth only: the controller's connection LED animation has finished. Until
	// then lightbar and player LEDs are held back; the first report after it carries
	// "release LEDs" (block[1] bit 3) and the next the LED values, which is what SDL
	// does (HIDAPI_DriverPS5_CheckPendingLEDReset). USB writers start ready.
	void LedsReady();

	WriterStats stats() const;
	protocol::EffectsState state() const;

private:
	enum class LedPhase { Hold, Armed, Ready };

	template <class F> void Modify(F &&change);
	bool SendableLocked() const;
	void Run();

	OutputSink *const sink_;
	const Config config_;

	mutable std::mutex mutex_;
	std::condition_variable cond_;
	protocol::EffectsState state_;
	uint32_t dirty_ = 0;
	LedPhase led_phase_;
	uint8_t seq_ = 0;
	bool stop_ = false;
	bool send_off_ = false;
	bool attempted_ = false;
	WriterStats stats_;
	std::thread thread_;
};

} // namespace plk::dualsense
