// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Bluetooth haptics: the pacer against a fake clock (exact timestamps), and the
// sender thread against a recording sink.

#include "HapticsSender.h"
#include "Protocol.h"
#include "test.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

using namespace plk::dualsense;
using namespace plk::dualsense::protocol;
using namespace std::chrono_literals;

namespace {

constexpr int64_t kMs = 1000000;

struct Push {
	int64_t time;
	std::vector<uint8_t> bytes;
};

struct Send {
	int64_t time;
	std::vector<uint8_t> block;
};

// Drives a pacer with a fake clock, the way HapticsSender::Run() does: waits for data,
// waits until deadlines (waking early on pushes only while prefilling), and models a
// write that takes `write_ns` (or a per-send override).
struct Sim {
	explicit Sim(const HapticsConfig &c) : pacer(c), block(pacer.report_bytes()) {}

	void Run(std::vector<Push> pushes, int64_t end, int64_t write_ns = 0,
		int64_t stall_at_send = -1, int64_t stall_ns = 0)
	{
		std::sort(pushes.begin(), pushes.end(), [](const Push &a, const Push &b) { return a.time < b.time; });
		size_t idx = 0;
		int64_t now = 0;
		const int64_t never = std::numeric_limits<int64_t>::max();
		while(now <= end)
		{
			while(idx < pushes.size() && pushes[idx].time <= now)
			{
				pacer.Push(pushes[idx].bytes.data(), pushes[idx].bytes.size());
				idx++;
			}
			const int64_t next_push = idx < pushes.size() ? pushes[idx].time : never;
			HapticsPacer::Step step = pacer.Next(now, block.data());
			if(step.kind == HapticsPacer::Step::Send)
			{
				sends.push_back({ now, block });
				int64_t w = write_ns;
				if((int64_t)sends.size() - 1 == stall_at_send)
					w = stall_ns;
				now += w;
				pacer.Sent(now);
				continue;
			}
			int64_t wake;
			if(step.kind == HapticsPacer::Step::WaitForData)
				wake = next_push;
			else if(pacer.Prefilling())
				wake = std::min(step.deadline, next_push);
			else
				wake = step.deadline;
			if(wake == never)
				break;
			now = std::max(now, wake);
		}
	}

	HapticsPacer pacer;
	std::vector<uint8_t> block;
	std::vector<Send> sends;
};

// The console's stream: 30 stereo frames (60 bytes) every 10 ms, bytes counting up.
std::vector<Push> ConsoleStream(int64_t start, int64_t end, uint8_t *counter)
{
	std::vector<Push> v;
	for(int64_t t = start; t <= end; t += 10 * kMs)
	{
		Push p{ t, std::vector<uint8_t>(60) };
		for(auto &b : p.bytes)
			b = (*counter)++;
		v.push_back(std::move(p));
	}
	return v;
}

} // namespace

TEST(haptics_pacer_default_config_matches_gui)
{
	HapticsPacer p{ HapticsConfig() };
	CHECK_EQ(p.report_bytes(), (size_t)128);
	CHECK_EQ(p.prefill_bytes(), (size_t)192);
	CHECK_EQ(p.max_bytes(), (size_t)576);
	CHECK_EQ(p.period(), (int64_t)21333333);
	CHECK_EQ(p.prefill_wait(), (int64_t)21333333 * 3);

	HapticsConfig single;
	single.double_block = false;
	HapticsPacer s{ single };
	CHECK_EQ(s.report_bytes(), (size_t)64);
	CHECK_EQ(s.period(), (int64_t)10666666);
	CHECK_EQ(s.prefill_wait(), (int64_t)10666666 * 4);

	HapticsConfig small;
	small.max_ms = 0;
	HapticsPacer m{ small };
	CHECK_EQ(m.max_bytes(), (size_t)(192 + 128)); // never below prefill + one report
}

TEST(haptics_pacer_steady_stream_is_paced_exactly)
{
	Sim sim{ HapticsConfig() };
	uint8_t counter = 0;
	sim.Run(ConsoleStream(0, 1000 * kMs, &counter), 2000 * kMs);
	CHECK(!sim.sends.empty());
	// Playback starts with the 4th packet (240 bytes >= 192 prefill), at 30 ms.
	CHECK_EQ(sim.sends.front().time, 30 * kMs);
	// Every following report exactly one period later, no gaps while data flows.
	const int64_t period = sim.pacer.period();
	for(size_t i = 1; i < sim.sends.size(); i++)
		CHECK_EQ(sim.sends[i].time - sim.sends[i - 1].time, period);
	// 101 packets * 60 bytes = 6060 bytes = 47 full reports + one partial.
	CHECK_EQ(sim.sends.size(), (size_t)48);
	CHECK_EQ(sim.pacer.dropped_bytes(), (uint64_t)0);
	CHECK_EQ(sim.pacer.underruns(), (uint64_t)1); // only when the stream ended
	// Samples leave in order, unmodified; the tail is zero padded.
	uint8_t expect = 0;
	size_t total = 0;
	for(size_t i = 0; i < sim.sends.size(); i++)
		for(size_t k = 0; k < sim.sends[i].block.size() && total < 6060; k++, total++)
			CHECK_EQ(sim.sends[i].block[k], expect++);
	const auto &last = sim.sends.back().block;
	CHECK_EQ(last[6060 - 47 * 128], 0);
	CHECK_EQ(last.back(), 0);
}

TEST(haptics_pacer_prefill_times_out)
{
	Sim sim{ HapticsConfig() };
	std::vector<Push> pushes = { { 0, std::vector<uint8_t>(60, 0x11) } };
	sim.Run(pushes, 500 * kMs);
	CHECK_EQ(sim.sends.size(), (size_t)1);
	CHECK_EQ(sim.sends[0].time, sim.pacer.prefill_wait()); // gave up waiting at 64 ms
	CHECK_EQ(sim.sends[0].block[59], 0x11);
	CHECK_EQ(sim.sends[0].block[60], 0x00);
	CHECK_EQ(sim.pacer.underruns(), (uint64_t)1);
}

TEST(haptics_pacer_underrun_rebuilds_prefill)
{
	Sim sim{ HapticsConfig() };
	uint8_t counter = 0;
	auto pushes = ConsoleStream(0, 200 * kMs, &counter);
	auto later = ConsoleStream(500 * kMs, 700 * kMs, &counter);
	pushes.insert(pushes.end(), later.begin(), later.end());
	sim.Run(pushes, 1500 * kMs);
	// The second burst starts after its own prefill: 4 packets in, at 530 ms.
	auto it = std::find_if(sim.sends.begin(), sim.sends.end(), [](const Send &s) { return s.time >= 500 * kMs; });
	CHECK(it != sim.sends.end());
	if(it != sim.sends.end())
		CHECK_EQ(it->time, 530 * kMs);
	CHECK_EQ(sim.pacer.underruns(), (uint64_t)2);
}

TEST(haptics_pacer_drops_backlog_above_cap)
{
	Sim sim{ HapticsConfig() };
	uint8_t counter = 0;
	auto pushes = ConsoleStream(0, 400 * kMs, &counter);
	pushes.push_back({ 205 * kMs, std::vector<uint8_t>(1001, 0x7F) }); // a burst
	sim.Run(pushes, 1000 * kMs);
	CHECK(sim.pacer.dropped_bytes() > 0);
	CHECK_EQ(sim.pacer.dropped_bytes() % 2, (uint64_t)0); // whole stereo frames only
	// Pacing is unaffected.
	for(size_t i = 1; i < sim.sends.size(); i++)
		CHECK_EQ(sim.sends[i].time - sim.sends[i - 1].time, sim.pacer.period());
}

TEST(haptics_pacer_resyncs_after_stalled_write)
{
	Sim sim{ HapticsConfig() };
	uint8_t counter = 0;
	// A long stream with enough backlog; the 3rd write stalls for 200 ms.
	sim.Run(ConsoleStream(0, 2000 * kMs, &counter), 3000 * kMs, 0, 2, 200 * kMs);
	CHECK(sim.sends.size() > 6);
	const int64_t period = sim.pacer.period();
	const int64_t stall_end = sim.sends[2].time + 200 * kMs;
	// One report right after the stall, then the normal period: no catch-up burst.
	CHECK_EQ(sim.sends[3].time, stall_end);
	CHECK_EQ(sim.sends[4].time, stall_end + period);
	CHECK_EQ(sim.sends[5].time, stall_end + 2 * period);
}

TEST(haptics_pacer_small_delay_keeps_schedule)
{
	// A write that is slow but within 4 periods does not move the schedule.
	Sim sim{ HapticsConfig() };
	uint8_t counter = 0;
	sim.Run(ConsoleStream(0, 1000 * kMs, &counter), 2000 * kMs, 3 * kMs);
	for(size_t i = 1; i + 1 < sim.sends.size(); i++)
		CHECK_EQ(sim.sends[i].time - sim.sends[i - 1].time, sim.pacer.period());
}

TEST(haptics_pacer_single_block)
{
	HapticsConfig c;
	c.double_block = false;
	Sim sim{ c };
	uint8_t counter = 0;
	sim.Run(ConsoleStream(0, 500 * kMs, &counter), 1000 * kMs);
	CHECK_EQ(sim.sends.front().time, 30 * kMs);
	for(size_t i = 1; i < sim.sends.size(); i++)
		CHECK_EQ(sim.sends[i].time - sim.sends[i - 1].time, (int64_t)10666666);
}

// ---------------------------------------------------------------------------

namespace {

class RecordingSink : public OutputSink {
public:
	bool Write(const uint8_t *data, size_t size) override
	{
		std::lock_guard<std::mutex> lk(m);
		reports.emplace_back(data, data + size);
		times.push_back(std::chrono::steady_clock::now());
		return ok;
	}
	size_t Count()
	{
		std::lock_guard<std::mutex> lk(m);
		return reports.size();
	}
	std::mutex m;
	std::vector<std::vector<uint8_t>> reports;
	std::vector<std::chrono::steady_clock::time_point> times;
	bool ok = true;
};

} // namespace

TEST(haptics_sender_writes_valid_reports_in_order)
{
	RecordingSink sink;
	HapticsSender s(&sink, HapticsConfig());
	s.Start();
	std::vector<int8_t> data(512);
	for(size_t i = 0; i < data.size(); i++)
		data[i] = (int8_t)i;
	s.Push(data.data(), data.size());
	std::this_thread::sleep_for(150ms); // 4 reports at 21.33 ms
	s.Stop();
	CHECK(sink.Count() >= 4);
	for(size_t i = 0; i < 4 && i < sink.reports.size(); i++)
	{
		const auto &r = sink.reports[i];
		CHECK_EQ(r.size(), kBtHapticsSize);
		CHECK_EQ(r[0], 0x32);
		CHECK_EQ(r[1], (uint8_t)(i << 4));
		CHECK_EQ(r[2], 0x91);
		CHECK_EQ(r[5], 48);
		CHECK_EQ(r[7], (uint8_t)(2 * i));
		CHECK_EQ(r[8], 0xD2);
		CHECK(BtCrcValid(kCrcSeedOutput, r.data(), r.size()));
		CHECK(!memcmp(r.data() + 10, data.data() + 128 * i, 128));
	}
	if(sink.times.size() >= 4)
	{
		const double ms = std::chrono::duration<double, std::milli>(sink.times[3] - sink.times[0]).count();
		CHECK(ms > 3 * 21.333 - 2 && ms < 3 * 21.333 + 15); // paced, not burst
	}
	HapticsStats st = s.stats();
	CHECK_EQ(st.reports, (uint64_t)sink.Count());
	CHECK_EQ(st.underruns, (uint64_t)1);
}

TEST(haptics_sender_gives_up_after_50_failures)
{
	RecordingSink sink;
	sink.ok = false;
	HapticsConfig c;
	c.double_block = false;
	c.prefill_ms = 0;
	c.max_ms = 1000;
	HapticsSender s(&sink, c);
	s.Start();
	std::vector<int8_t> data(64 * 60, 1);
	s.Push(data.data(), data.size());
	for(int i = 0; i < 300 && s.running(); i++)
		std::this_thread::sleep_for(10ms);
	CHECK(!s.running());
	HapticsStats st = s.stats();
	CHECK(st.failed);
	CHECK_EQ(st.write_failures, (uint64_t)HapticsSender::kMaxConsecutiveFailures);
	CHECK_EQ(st.reports, (uint64_t)0);
	s.Push(data.data(), 2); // ignored once failed
	s.Stop();
}

TEST(haptics_sender_pcm16_conversion_and_silence_tracking)
{
	RecordingSink sink;
	HapticsSender s(&sink, HapticsConfig()); // both pushes land inside the 64 ms prefill wait
	s.Start();
	const int16_t zeros[4] = { 0, 0, 255, -255 }; // all truncate to 0 after / 256
	s.PushPcm16(zeros, 4, 1.0f);
	CHECK(!s.RecentlyNonSilent(250));
	const int16_t pcm[8] = { 256, -256, 32767, -32768, 512, 100, 1000, 1000 };
	s.PushPcm16(pcm, 8, 0.5f);
	CHECK(s.RecentlyNonSilent(250));
	const int16_t loud[2] = { 32767, -32768 };
	s.PushPcm16(loud, 2, 4.0f);
	std::this_thread::sleep_for(120ms);
	s.Stop();
	CHECK_EQ(sink.Count(), (size_t)1);
	if(sink.Count())
	{
		const auto &r = sink.reports[0];
		// (int)(s16 * gain) / 256, clamped to s8
		const int8_t expect[16] = { 0, 0, 0, 0, 0, 0, 63, -64, 1, 0, 1, 1, 127, -128, 0, 0 };
		CHECK(!memcmp(r.data() + 10, expect, sizeof(expect)));
	}
}

TEST(haptics_config_from_environment)
{
	setenv("CHIAKI_BT_HAPTICS_SINGLE", "1", 1);
	setenv("CHIAKI_BT_HAPTICS_CTRL_BUFFER", "500", 1);
	setenv("CHIAKI_BT_HAPTICS_PREFILL_MS", "abc", 1);
	setenv("CHIAKI_BT_HAPTICS_MAX_MS", "0x40", 1);
	HapticsConfig c = HapticsConfig::FromEnvironment();
	CHECK(!c.double_block);
	CHECK_EQ(c.controller_buffer, 128);
	CHECK_EQ(c.prefill_ms, 32);
	CHECK_EQ(c.max_ms, 64);
	unsetenv("CHIAKI_BT_HAPTICS_SINGLE");
	unsetenv("CHIAKI_BT_HAPTICS_CTRL_BUFFER");
	unsetenv("CHIAKI_BT_HAPTICS_PREFILL_MS");
	unsetenv("CHIAKI_BT_HAPTICS_MAX_MS");
	HapticsConfig d = HapticsConfig::FromEnvironment();
	CHECK(d.double_block);
	CHECK_EQ(d.controller_buffer, 48);
	CHECK_EQ(d.prefill_ms, 32);
	CHECK_EQ(d.max_ms, 96);
}
