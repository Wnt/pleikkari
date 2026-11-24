// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// plk-dualsense-probe: inspect a connected DualSense through the native driver.
//
// Default mode is read-only: the controller is opened non-exclusively, feature
// reports 0x20 and 0x05 are read, and input reports are timed. Nothing is written.
// The output test modes (--rumble, --trigger, --led, --haptics-tone) write to the
// controller and must only be used when nothing else is using it.

#include "DualSenseHID.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace plk::dualsense;

namespace {

struct Sample {
	uint64_t hid_time;
	uint64_t host_time;
	uint32_t sensor;
	uint8_t counter;
	uint8_t bt_header;
	uint8_t report_id;
	ReportMode mode;
};

enum class Mode { Measure, Rumble, Trigger, Led, HapticsTone };

void Usage()
{
	fprintf(stderr,
		"usage: plk-dualsense-probe [options]\n"
		"  -t, --seconds N       measurement time (read-only mode: 1..10, default 5)\n"
		"  --wait N              seconds to wait for a controller (default 3)\n"
		"  --dump-reports FILE   write raw input reports as hex lines (with HID time in us)\n"
		"  --dump-limit N        reports to dump (default 2000)\n"
		"  --dump-calibration FILE  write feature report 0x05 as hex\n"
		"output test modes (WRITE to the controller, not read-only):\n"
		"  --rumble              ramp both motors up, then off\n"
		"  --trigger             resistance on L2 and R2 for 3 s, then off\n"
		"  --led                 cycle lightbar, player LEDs and mic LED\n"
		"  --haptics-tone        Bluetooth only: 2 s of a 160 Hz tone via report 0x32\n");
}

std::string Hex(const uint8_t *data, size_t size)
{
	static const char digits[] = "0123456789abcdef";
	std::string s;
	s.reserve(size * 2);
	for(size_t i = 0; i < size; i++)
	{
		s.push_back(digits[data[i] >> 4]);
		s.push_back(digits[data[i] & 0xF]);
	}
	return s;
}

struct Dist {
	size_t n = 0;
	double min = 0, mean = 0, p50 = 0, p95 = 0, p99 = 0, max = 0;
};

Dist Distribution(std::vector<double> v)
{
	Dist d;
	d.n = v.size();
	if(v.empty())
		return d;
	std::sort(v.begin(), v.end());
	auto pct = [&](double p) { return v[std::min(v.size() - 1, (size_t)std::llround(p * (double)(v.size() - 1)))]; };
	d.min = v.front();
	d.max = v.back();
	double sum = 0;
	for(double x : v)
		sum += x;
	d.mean = sum / (double)v.size();
	d.p50 = pct(0.50);
	d.p95 = pct(0.95);
	d.p99 = pct(0.99);
	return d;
}

void PrintDist(const char *label, const char *unit, const Dist &d)
{
	printf("  %-34s n=%-6zu min %7.3f  mean %7.3f  p50 %7.3f  p95 %7.3f  p99 %7.3f  max %7.3f %s\n",
		label, d.n, d.min, d.mean, d.p50, d.p95, d.p99, d.max, unit);
}

void PrintHistogram(const char *label, const std::vector<double> &ms)
{
	// 0.25 ms bins up to 8 ms, then one overflow bin.
	const int bins = 32;
	std::vector<size_t> h(bins + 1, 0);
	for(double x : ms)
	{
		int b = (int)(x / 0.25);
		h[(size_t)std::clamp(b, 0, bins)]++;
	}
	printf("  %s histogram (0.25 ms bins, only non-empty):\n", label);
	for(int b = 0; b <= bins; b++)
	{
		if(!h[(size_t)b])
			continue;
		if(b == bins)
			printf("    >= %5.2f ms: %zu\n", bins * 0.25, h[(size_t)b]);
		else
			printf("    %5.2f-%5.2f ms: %zu (%.1f%%)\n", b * 0.25, (b + 1) * 0.25, h[(size_t)b],
				100.0 * (double)h[(size_t)b] / (double)ms.size());
	}
}

const char *BatteryName(BatteryStatus s)
{
	switch(s)
	{
		case BatteryStatus::Discharging: return "discharging";
		case BatteryStatus::Charging: return "charging";
		case BatteryStatus::Full: return "full";
		case BatteryStatus::Error: return "error";
		default: return "unknown";
	}
}

void PrintInfo(const DeviceInfo &i)
{
	printf("%s (%04X:%04X, %s), %s\n", i.product_name.c_str(), i.vendor_id, i.product_id,
		i.model == Model::DualSenseEdge ? "DualSense Edge" : "DualSense",
		i.connection == Connection::Bluetooth ? "Bluetooth" : "USB");
	if(i.firmware_valid)
		printf("  firmware: built %s %s, hardware 0x%08x, firmware 0x%08x, update version 0x%04x\n",
			i.build_date.c_str(), i.build_time.c_str(), i.hardware_version, i.firmware_version, i.update_version);
	else
		printf("  firmware: feature report 0x20 unavailable (%zu bytes read)\n", i.raw_firmware_info.size());
	printf("  IMU calibration: %s (feature 0x05, %zu bytes)\n",
		i.calibration_valid ? "valid" : "unavailable, nominal scaling", i.raw_calibration.size());
	printf("  rumble encoding: %s\n", i.rumble_v2 ? "improved (v2, full scale)" : "legacy (halved)");
	printf("  max report sizes: input %zu, output %zu\n", i.max_input_report_size, i.max_output_report_size);
}

void Analyse(const std::vector<Sample> &s, const InputStats &stats, double seconds)
{
	printf("\nInput reports over %.1f s (read-only):\n", seconds);
	printf("  delivered %llu, CRC errors %llu, other/unparsed %llu\n",
		(unsigned long long)stats.reports, (unsigned long long)stats.crc_errors, (unsigned long long)stats.other_reports);
	if(s.size() < 3)
	{
		printf("  too few reports to analyse\n");
		return;
	}
	size_t simple = 0;
	for(auto &x : s)
		simple += x.mode == ReportMode::BluetoothSimple;
	printf("  report id 0x%02x, %zu enhanced, %zu simple\n", s.front().report_id, s.size() - simple, simple);

	const double span_ms = (double)(MachToNanos(s.back().hid_time) - MachToNanos(s.front().hid_time)) / 1e6;
	printf("  rate: %.1f reports/s by HID timestamp\n", (double)(s.size() - 1) / (span_ms / 1000.0));

	std::vector<double> host_ms, hid_ms, sensor_ms, dispatch_us;
	size_t counter_gaps = 0, counter_missing = 0, seq_gaps = 0, sensor_repeats = 0;
	uint64_t sensor_ticks = 0;
	for(size_t i = 0; i < s.size(); i++)
	{
		dispatch_us.push_back((double)(MachToNanos(s[i].host_time) - MachToNanos(s[i].hid_time)) / 1e3);
		if(i == 0)
			continue;
		host_ms.push_back((double)(MachToNanos(s[i].host_time) - MachToNanos(s[i - 1].host_time)) / 1e6);
		hid_ms.push_back((double)(MachToNanos(s[i].hid_time) - MachToNanos(s[i - 1].hid_time)) / 1e6);
		const uint8_t dc = (uint8_t)(s[i].counter - s[i - 1].counter);
		if(s[i].mode == ReportMode::Enhanced && s[i - 1].mode == ReportMode::Enhanced)
		{
			const uint32_t dt = s[i].sensor - s[i - 1].sensor;
			sensor_ticks += dt;
			sensor_ms.push_back((double)dt / 3000.0);
			if(dt == 0)
				sensor_repeats++;
			if(dc != 1)
			{
				counter_gaps++;
				counter_missing += (uint8_t)(dc - 1);
			}
			if(((s[i].bt_header >> 4) - (s[i - 1].bt_header >> 4) & 0x0F) != 1)
				seq_gaps++;
		}
	}
	PrintDist("interval, host callback time", "ms", Distribution(host_ms));
	PrintDist("interval, HID timestamp", "ms", Distribution(hid_ms));
	PrintDist("interval, controller sensor clock", "ms", Distribution(sensor_ms));
	PrintDist("HID timestamp -> callback", "us", Distribution(dispatch_us));
	printf("  counter (byte 6) gaps: %zu, reports missing by counter: %zu\n", counter_gaps, counter_missing);
	if(s.front().report_id == 0x31)
		printf("  Bluetooth header sequence (byte 1 high nibble) gaps: %zu\n", seq_gaps);
	printf("  repeated sensor timestamps: %zu\n", sensor_repeats);
	if(span_ms > 0 && sensor_ticks)
		printf("  sensor clock: %.4f ticks per us of host time (nominal 3)\n", (double)sensor_ticks / (span_ms * 1000.0));
	PrintHistogram("HID-timestamp interval", hid_ms);
}

// ---------------------------------------------------------------------------
// Output test modes. Never used by the read-only measurement.

void SleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

void PrintWriterStats(Device &d)
{
	WriterStats w = d.writer_stats();
	printf("  writer: %llu updates, %llu writes, %llu failures, write time mean %.3f ms, max %.3f ms\n",
		(unsigned long long)w.updates, (unsigned long long)w.writes, (unsigned long long)w.write_failures,
		w.writes ? (double)w.total_write_ns / (double)w.writes / 1e6 : 0.0, (double)w.max_write_ns / 1e6);
}

void RunRumble(Device &d)
{
	printf("Rumble: ramping both motors up over 1 s, hold 0.5 s, off\n");
	for(int i = 0; i <= 20; i++)
	{
		d.SetRumble((uint8_t)(i * 12), (uint8_t)(i * 12));
		SleepMs(50);
	}
	SleepMs(500);
	d.SetRumble(0, 0);
	SleepMs(200);
	PrintWriterStats(d);
}

void RunTrigger(Device &d)
{
	printf("Triggers: continuous resistance (effect 0x01) on L2 and R2 for 3 s, then off (0x05)\n");
	uint8_t effect[11] = { 0x01, 0x40, 0xA0 };
	d.SetTriggerEffects(effect, effect);
	SleepMs(3000);
	d.ClearEffects();
	SleepMs(200);
	PrintWriterStats(d);
}

void RunLed(Device &d)
{
	printf("LEDs: lightbar red, green, blue; player LEDs 1..5; mic LED on, pulse, off\n");
	const uint8_t colours[3][3] = { { 0x40, 0, 0 }, { 0, 0x40, 0 }, { 0, 0, 0x40 } };
	for(auto &c : colours)
	{
		d.SetLightbar(c[0], c[1], c[2]);
		SleepMs(600);
	}
	for(int i = 0; i < 5; i++)
	{
		d.SetPlayerLeds(Device::PlayerLedsForIndex(i));
		SleepMs(300);
	}
	d.SetMic(MicLed::On, 0x00);
	SleepMs(600);
	d.SetMic(MicLed::Pulse, 0x00);
	SleepMs(1200);
	d.SetMic(MicLed::Off, 0x00);
	SleepMs(200);
	PrintWriterStats(d);
}

void RunHapticsTone(Device &d)
{
	if(d.info().connection != Connection::Bluetooth)
	{
		printf("--haptics-tone needs a Bluetooth controller (USB haptics go through the audio device)\n");
		return;
	}
	HapticsConfig config = HapticsConfig::FromEnvironment();
	printf("Haptics: 2 s of a 160 Hz tone, 30 frames every 10 ms (double block %d, prefill %d ms, max %d ms)\n",
		config.double_block, config.prefill_ms, config.max_ms);
	if(!d.StartHaptics(config))
	{
		printf("  StartHaptics failed\n");
		return;
	}
	const double kTwoPi = 6.283185307179586;
	int8_t chunk[60];
	uint64_t frame = 0;
	auto next = std::chrono::steady_clock::now();
	for(int packet = 0; packet < 200; packet++)
	{
		for(int i = 0; i < 30; i++, frame++)
		{
			int8_t v = (int8_t)std::lround(90.0 * std::sin(kTwoPi * 160.0 * (double)frame / 3000.0));
			chunk[2 * i] = v;
			chunk[2 * i + 1] = v;
		}
		d.PushHaptics(chunk, sizeof(chunk));
		next += std::chrono::milliseconds(10);
		std::this_thread::sleep_until(next);
	}
	SleepMs(150);
	HapticsStats h = d.haptics_stats();
	d.StopHaptics();
	printf("  haptics: %llu reports, %llu underruns, %llu bytes dropped, %llu write failures%s\n",
		(unsigned long long)h.reports, (unsigned long long)h.underruns, (unsigned long long)h.dropped_bytes,
		(unsigned long long)h.write_failures, h.failed ? ", FAILED" : "");
}

} // namespace

int main(int argc, char **argv)
{
	double seconds = 5;
	double wait_s = 3;
	std::string dump_path, calibration_path;
	size_t dump_limit = 2000;
	Mode mode = Mode::Measure;

	for(int i = 1; i < argc; i++)
	{
		std::string a = argv[i];
		auto next_arg = [&]() -> const char * {
			if(i + 1 >= argc)
			{
				Usage();
				exit(2);
			}
			return argv[++i];
		};
		if(a == "-t" || a == "--seconds")
			seconds = atof(next_arg());
		else if(a == "--wait")
			wait_s = atof(next_arg());
		else if(a == "--dump-reports")
			dump_path = next_arg();
		else if(a == "--dump-limit")
			dump_limit = (size_t)atol(next_arg());
		else if(a == "--dump-calibration")
			calibration_path = next_arg();
		else if(a == "--rumble")
			mode = Mode::Rumble;
		else if(a == "--trigger")
			mode = Mode::Trigger;
		else if(a == "--led")
			mode = Mode::Led;
		else if(a == "--haptics-tone")
			mode = Mode::HapticsTone;
		else
		{
			Usage();
			return a == "-h" || a == "--help" ? 0 : 2;
		}
	}
	const bool write_mode = mode != Mode::Measure;
	if(!write_mode)
		seconds = std::clamp(seconds, 1.0, 10.0);

	std::mutex mutex;
	std::condition_variable cond;
	std::shared_ptr<Device> device;
	std::atomic<bool> recording{false};
	std::vector<Sample> samples;
	samples.reserve((size_t)(seconds * 4000) + 16);
	InputState last_state;
	FILE *dump = nullptr;
	size_t dumped = 0;
	uint64_t dump_t0 = 0;
	if(!dump_path.empty())
	{
		dump = fopen(dump_path.c_str(), "w");
		if(!dump)
		{
			perror(dump_path.c_str());
			return 1;
		}
	}

	Manager manager;
	Manager::Options options;
	options.allow_output = write_mode;
	Manager::Callbacks cb;
	cb.log = [](const char *m) { fprintf(stderr, "[plk.ds] %s\n", m); };
	cb.connected = [&](const std::shared_ptr<Device> &d) {
		std::lock_guard<std::mutex> lk(mutex);
		if(!device)
			device = d;
		cond.notify_all();
	};
	cb.disconnected = [&](const std::shared_ptr<Device> &d) {
		fprintf(stderr, "[plk.ds] controller disconnected\n");
	};
	cb.raw_input = [&](Device &d, const uint8_t *report, size_t size, uint64_t hid_time) {
		if(!dump || !recording || dumped >= dump_limit || &d != device.get())
			return;
		if(!dumped)
			dump_t0 = hid_time;
		fprintf(dump, "%llu %s\n", (unsigned long long)(MachToNanos(hid_time - dump_t0) / 1000), Hex(report, size).c_str());
		dumped++;
	};
	cb.input = [&](Device &d, const InputState &s) {
		if(!recording || &d != device.get())
			return;
		last_state = s;
		if(samples.size() == samples.capacity())
			return;
		samples.push_back({ s.hid_time, s.host_time, s.sensor_timestamp, s.counter, s.bt_header, s.report_id, s.mode });
	};

	if(!manager.Start(options, cb))
	{
		fprintf(stderr, "could not start the IOHIDManager\n");
		return 1;
	}
	{
		std::unique_lock<std::mutex> lk(mutex);
		cond.wait_for(lk, std::chrono::duration<double>(wait_s), [&] { return device != nullptr; });
	}
	if(!device)
	{
		fprintf(stderr, "no DualSense found within %.1f s\n", wait_s);
		return 1;
	}
	PrintInfo(device->info());
	if(!calibration_path.empty())
	{
		FILE *f = fopen(calibration_path.c_str(), "w");
		if(f)
		{
			fprintf(f, "%s\n", Hex(device->info().raw_calibration.data(), device->info().raw_calibration.size()).c_str());
			fclose(f);
		}
	}

	if(write_mode)
	{
		printf("WRITING to the controller (%s)\n", device->info().connection == Connection::Bluetooth ? "Bluetooth" : "USB");
		switch(mode)
		{
			case Mode::Rumble: RunRumble(*device); break;
			case Mode::Trigger: RunTrigger(*device); break;
			case Mode::Led: RunLed(*device); break;
			case Mode::HapticsTone: RunHapticsTone(*device); break;
			default: break;
		}
		manager.Stop(); // writes the effects-off report
		return 0;
	}

	recording = true;
	std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
	recording = false;
	InputStats stats = device->input_stats();
	manager.Stop(); // joins the HID thread: `samples` is ours from here
	if(dump)
		fclose(dump);
	Analyse(samples, stats, seconds);
	if(last_state.battery_valid)
		printf("  battery: %u%% (%s)%s\n", last_state.battery_percent, BatteryName(last_state.battery_status),
			last_state.headphones ? ", headphones plugged in" : "");
	if(dump)
		printf("  dumped %zu raw reports to %s\n", dumped, dump_path.c_str());
	return 0;
}
