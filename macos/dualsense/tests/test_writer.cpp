// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// The coalescing effects writer against a fake slow device: a sink whose Write()
// blocks until the test lets it finish, like a blocking Bluetooth write.

#include "EffectsWriter.h"
#include "Protocol.h"
#include "test.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

using namespace plk::dualsense;
using namespace plk::dualsense::protocol;
using namespace std::chrono_literals;

namespace {

class SlowSink : public OutputSink {
public:
	bool Write(const uint8_t *data, size_t size) override
	{
		std::unique_lock<std::mutex> lk(m);
		reports.emplace_back(data, data + size);
		cv.notify_all();
		// Block until the test releases this write (or auto mode).
		cv.wait(lk, [&] { return auto_release || released >= reports.size(); });
		return ok;
	}

	// Waits until `n` writes have started.
	bool WaitForWrites(size_t n, std::chrono::milliseconds timeout = 2000ms)
	{
		std::unique_lock<std::mutex> lk(m);
		return cv.wait_for(lk, timeout, [&] { return reports.size() >= n; });
	}
	void Release(size_t n)
	{
		std::lock_guard<std::mutex> lk(m);
		released = n;
		cv.notify_all();
	}
	void AutoRelease()
	{
		std::lock_guard<std::mutex> lk(m);
		auto_release = true;
		cv.notify_all();
	}
	size_t Count()
	{
		std::lock_guard<std::mutex> lk(m);
		return reports.size();
	}
	std::vector<uint8_t> Report(size_t i)
	{
		std::lock_guard<std::mutex> lk(m);
		return reports.at(i);
	}

	std::mutex m;
	std::condition_variable cv;
	std::vector<std::vector<uint8_t>> reports;
	size_t released = 0;
	bool auto_release = false;
	bool ok = true;
};

const uint8_t kTL[11] = { 0x21, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
const uint8_t kTR[11] = { 0x26, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 };

// USB report: block starts at byte 1.
const uint8_t *Block(const std::vector<uint8_t> &r) { return r.data() + (r[0] == kReportIdBtOutput ? 3 : 1); }

} // namespace

TEST(writer_coalesces_updates_during_blocking_write)
{
	SlowSink sink;
	EffectsWriter w(&sink, { /*bluetooth*/ false, /*rumble_v2*/ true });
	w.Start();
	w.SetRumble(10, 20);
	CHECK(sink.WaitForWrites(1)); // write #1 is now blocked inside the sink
	// Three updates while the write is in flight.
	w.SetTriggerEffects(kTL, kTR);
	w.SetLightbar(1, 2, 3);
	w.SetRumble(30, 40);
	std::this_thread::sleep_for(20ms);
	CHECK_EQ(sink.Count(), (size_t)1); // nothing queued behind the blocked write
	sink.Release(1);
	CHECK(sink.WaitForWrites(2));
	sink.Release(2);
	std::this_thread::sleep_for(50ms);
	CHECK_EQ(sink.Count(), (size_t)2); // exactly one follow-up write

	const auto first = sink.Report(0);
	CHECK_EQ(Block(first)[3], 10);
	CHECK_EQ(Block(first)[2], 20);
	const auto second = sink.Report(1);
	const uint8_t *b = Block(second);
	CHECK_EQ(b[3], 30);           // latest rumble
	CHECK_EQ(b[2], 40);
	CHECK_EQ(b[0], 0x02 | 0x04 | 0x08); // rumble + both triggers
	CHECK_EQ(b[1], 0x40 | 0x04);  // motor power + lightbar
	CHECK(!memcmp(b + 10, kTR, 11));
	CHECK(!memcmp(b + 21, kTL, 11));
	CHECK_EQ(b[44], 1);
	CHECK_EQ(b[45], 2);
	CHECK_EQ(b[46], 3);
	sink.AutoRelease();
	w.Stop(false);
}

TEST(writer_unchanged_state_writes_nothing)
{
	SlowSink sink;
	sink.AutoRelease();
	EffectsWriter w(&sink, { false, true });
	w.Start();
	w.SetRumble(5, 6);
	CHECK(sink.WaitForWrites(1));
	w.SetRumble(5, 6);
	w.SetTriggerEffects(kTL, kTR);
	CHECK(sink.WaitForWrites(2));
	w.SetTriggerEffects(kTL, kTR);
	std::this_thread::sleep_for(30ms);
	CHECK_EQ(sink.Count(), (size_t)2);
	w.Stop(false);
	CHECK_EQ(sink.Count(), (size_t)2);
}

TEST(writer_zero_rumble_is_sent_once_then_released)
{
	SlowSink sink;
	sink.AutoRelease();
	EffectsWriter w(&sink, { false, true });
	w.Start();
	w.SetRumble(100, 100);
	CHECK(sink.WaitForWrites(1));
	w.SetRumble(0, 0);
	CHECK(sink.WaitForWrites(2));
	w.SetLightbar(9, 9, 9);
	CHECK(sink.WaitForWrites(3));
	w.Stop(false);
	const auto zero = sink.Report(1);
	CHECK_EQ(Block(zero)[0] & 0x02, 0x02); // rumble flags with zero values
	CHECK_EQ(Block(zero)[38] & 0x04, 0x04);
	CHECK_EQ(Block(zero)[3], 0);
	const auto after = sink.Report(2);
	CHECK_EQ(Block(after)[0], 0);          // rumble left the report: audio haptics again
	CHECK_EQ(Block(after)[38], 0);
	CHECK_EQ(Block(after)[1], 0x04);
}

TEST(writer_motor_power_rides_with_rumble_or_triggers)
{
	SlowSink sink;
	sink.AutoRelease();
	EffectsWriter w(&sink, { false, true });
	w.Start();
	w.SetMotorPowerLevel(0x62); // nothing to carry it yet: no write
	std::this_thread::sleep_for(20ms);
	CHECK_EQ(sink.Count(), (size_t)0);
	w.SetTriggerEffects(kTL, kTR);
	CHECK(sink.WaitForWrites(1));
	CHECK_EQ(Block(sink.Report(0))[36], 0x62);
	w.SetMotorPowerLevel(0x00); // triggers active: goes out at once
	CHECK(sink.WaitForWrites(2));
	CHECK_EQ(Block(sink.Report(1))[36], 0x00);
	CHECK_EQ(Block(sink.Report(1))[1] & 0x40, 0x40);
	w.Stop(false);
}

TEST(writer_bluetooth_holds_leds_until_ready_then_releases)
{
	SlowSink sink;
	sink.AutoRelease();
	EffectsWriter w(&sink, { /*bluetooth*/ true, true });
	w.Start();
	w.SetLightbar(0x11, 0x22, 0x33);
	w.SetPlayerLeds(0x24);
	std::this_thread::sleep_for(20ms);
	CHECK_EQ(sink.Count(), (size_t)0); // connection animation still running
	w.SetRumble(1, 2);                 // other effects are not held
	CHECK(sink.WaitForWrites(1));
	CHECK_EQ(Block(sink.Report(0))[1] & (0x04 | 0x08 | 0x10), 0);
	w.LedsReady();
	CHECK(sink.WaitForWrites(3));
	std::this_thread::sleep_for(20ms);
	CHECK_EQ(sink.Count(), (size_t)3);
	const auto release = sink.Report(1);
	CHECK_EQ(Block(release)[1] & 0x08, 0x08);
	CHECK_EQ(Block(release)[1] & (0x04 | 0x10), 0);
	const auto leds = sink.Report(2);
	CHECK_EQ(Block(leds)[1] & (0x04 | 0x08 | 0x10), 0x04 | 0x10);
	CHECK_EQ(Block(leds)[44], 0x11);
	CHECK_EQ(Block(leds)[43], 0x24);
	// Bluetooth framing on every report: sequence nibble increments, CRC valid.
	for(size_t i = 0; i < 3; i++)
	{
		const auto r = sink.Report(i);
		CHECK_EQ(r.size(), kBtOutputSize);
		CHECK_EQ(r[1], (uint8_t)(i << 4));
		CHECK(BtCrcValid(kCrcSeedOutput, r.data(), r.size()));
	}
	w.Stop(false);
}

TEST(writer_stop_sends_effects_off_only_after_writing)
{
	{
		SlowSink sink;
		sink.AutoRelease();
		EffectsWriter w(&sink, { false, true });
		w.Start();
		w.Stop(true);
		CHECK_EQ(sink.Count(), (size_t)0); // never wrote: stays silent
	}
	{
		SlowSink sink;
		sink.AutoRelease();
		EffectsWriter w(&sink, { false, true });
		w.Start();
		w.SetMotorPowerLevel(0x60);
		w.SetRumble(200, 200);
		w.SetLightbar(5, 5, 5);
		CHECK(sink.WaitForWrites(1));
		w.Stop(true);
		const size_t n = sink.Count();
		CHECK(n >= 2);
		const auto off = sink.Report(n - 1);
		const uint8_t *b = Block(off);
		CHECK_EQ(b[0], 0x02 | 0x04 | 0x08);
		CHECK_EQ(b[2], 0);
		CHECK_EQ(b[3], 0);
		CHECK_EQ(b[10], 0x05);
		CHECK_EQ(b[21], 0x05);
		CHECK_EQ(b[36], 0x60);
		CHECK_EQ(b[1], 0x40); // lightbar left as it is
	}
}

TEST(writer_clear_effects)
{
	SlowSink sink;
	sink.AutoRelease();
	EffectsWriter w(&sink, { false, false });
	w.Start();
	w.SetTriggerEffects(kTL, kTR);
	w.SetRumble(50, 60);
	CHECK(sink.WaitForWrites(1));
	w.ClearEffects();
	std::this_thread::sleep_for(30ms);
	const auto r = sink.Report(sink.Count() - 1);
	const uint8_t *b = Block(r);
	CHECK_EQ(b[0], 0x01 | 0x02 | 0x04 | 0x08); // legacy rumble flag (rumble_v2 = false)
	CHECK_EQ(b[3], 0);
	CHECK_EQ(b[10], 0x05);
	CHECK_EQ(b[21], 0x05);
	w.Stop(false);
}

TEST(writer_stats_count_writes_and_failures)
{
	SlowSink sink;
	sink.ok = false;
	sink.AutoRelease();
	EffectsWriter w(&sink, { false, true });
	w.Start();
	w.SetRumble(1, 1);
	CHECK(sink.WaitForWrites(1));
	std::this_thread::sleep_for(20ms);
	WriterStats s = w.stats();
	CHECK_EQ(s.writes, (uint64_t)1);
	CHECK_EQ(s.write_failures, (uint64_t)1);
	CHECK_EQ(s.updates, (uint64_t)1);
	w.Stop(false);
}
