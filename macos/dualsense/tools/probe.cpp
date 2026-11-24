// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// plk-dualsense-probe: inspect a connected DualSense through the native driver.
//
// Default mode is read-only: the controller is opened non-exclusively, feature
// reports 0x20 and 0x05 are read, and input reports are timed. Nothing is written.
// The output test modes (--rumble, --trigger, --led, --haptics-tone) write to the
// controller and must only be used when nothing else is using it.

#include "DualSenseHID.h"
#include "ThreadUtil.h"

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

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/task_policy.h>
#include <mach/thread_act.h>
#include <pthread.h>
#include <pthread/qos.h>

using namespace plk::dualsense;

namespace {

struct Sample {
	uint64_t hid_time;
	uint64_t host_time;
	uint32_t sensor;
	uint32_t sequence;
	uint8_t bt_header;
	uint8_t report_id;
	ReportMode mode;
};

enum class Mode { Measure, SchedTest, Rumble, Trigger, Led, HapticsTone };

void Usage()
{
	fprintf(stderr,
		"usage: plk-dualsense-probe [options]\n"
		"  -t, --seconds N       measurement time (read-only mode: 1..10, default 5)\n"
		"  --wait N              seconds to wait for a controller (default 3)\n"
		"  --dump-reports FILE   write raw input reports as hex lines (with HID time in us)\n"
		"  --dump-limit N        reports to dump (default 2000)\n"
		"  --dump-calibration FILE  write feature report 0x05 as hex\n"
		"  --timing-csv FILE     write per-report timing (ns) for offline analysis\n"
		"  --realtime            run the driver threads under the time-constraint policy\n"
		"                        (Manager::Options::realtime_threads)\n"
		"  --sched-test          no controller: timer wake-up lateness of a user-interactive\n"
		"                        thread vs a time-constraint (real-time) thread, -t seconds each\n"
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
	// 0.5 ms bins up to 40 ms (Bluetooth intervals are 7.5 / 15 ms multiples), then
	// one overflow bin.
	const double width = 0.5;
	const int bins = 80;
	std::vector<size_t> h(bins + 1, 0);
	for(double x : ms)
	{
		int b = (int)(x / width);
		h[(size_t)std::clamp(b, 0, bins)]++;
	}
	printf("  %s histogram (%.1f ms bins, only non-empty):\n", label, width);
	for(int b = 0; b <= bins; b++)
	{
		if(!h[(size_t)b])
			continue;
		if(b == bins)
			printf("    >= %5.1f ms: %zu\n", bins * width, h[(size_t)b]);
		else
			printf("    %5.1f-%5.1f ms: %zu (%.1f%%)\n", b * width, (b + 1) * width, h[(size_t)b],
				100.0 * (double)h[(size_t)b] / (double)ms.size());
	}
}

// Scheduling of the calling thread: requested QoS and the kernel's current and base
// priority (user-interactive QoS is base 47 on macOS; a clamped or background task
// shows less).
struct ThreadSched {
	bool valid = false;
	qos_class_t qos = QOS_CLASS_UNSPECIFIED;
	int cur_pri = 0;
	int base_pri = 0;
};

ThreadSched CurrentThreadSched()
{
	ThreadSched t;
	t.qos = qos_class_self();
	thread_extended_info_data_t info;
	mach_msg_type_number_t count = THREAD_EXTENDED_INFO_COUNT;
	mach_port_t self = mach_thread_self();
	if(thread_info(self, THREAD_EXTENDED_INFO, (thread_info_t)&info, &count) == KERN_SUCCESS)
	{
		t.valid = true;
		t.cur_pri = info.pth_curpri;
		t.base_pri = info.pth_priority;
	}
	mach_port_deallocate(mach_task_self(), self);
	return t;
}

const char *QosName(qos_class_t q)
{
	switch(q)
	{
		case QOS_CLASS_USER_INTERACTIVE: return "user-interactive";
		case QOS_CLASS_USER_INITIATED: return "user-initiated";
		case QOS_CLASS_DEFAULT: return "default";
		case QOS_CLASS_UTILITY: return "utility";
		case QOS_CLASS_BACKGROUND: return "background";
		default: return "unspecified";
	}
}

const char *TaskRoleName()
{
	task_category_policy_data_t policy;
	mach_msg_type_number_t count = TASK_CATEGORY_POLICY_COUNT;
	boolean_t get_default = FALSE;
	if(task_policy_get(mach_task_self(), TASK_CATEGORY_POLICY, (task_policy_t)&policy, &count, &get_default) != KERN_SUCCESS)
		return "?";
	switch(policy.role)
	{
		case TASK_UNSPECIFIED: return "unspecified";
		case TASK_FOREGROUND_APPLICATION: return "foreground application";
		case TASK_BACKGROUND_APPLICATION: return "background application";
		case TASK_CONTROL_APPLICATION: return "control application";
		case TASK_GRAPHICS_SERVER: return "graphics server";
		case TASK_THROTTLE_APPLICATION: return "throttle application";
		case TASK_NONUI_APPLICATION: return "non-UI application";
		case TASK_DEFAULT_APPLICATION: return "default application";
		case TASK_DARWINBG_APPLICATION: return "darwin background";
		default: return "other";
	}
}

// Wake-up latency of a user-interactive thread in this process: sleep to an absolute
// deadline every 2 ms and record how late the thread runs. If this is small while
// "HID timestamp -> callback" is large, the delay is upstream of our thread
// (bluetoothd / the HID event path), not our scheduling.
void WakeupLatencyThread(std::atomic<bool> *run, std::vector<double> *late_us, bool time_constraint = false,
	ThreadSched *sched = nullptr)
{
	pthread_setname_np("plk.probe.wakeup");
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
	if(time_constraint && !SetCurrentThreadTimeConstraint(2000000, 200000, 1000000))
		fprintf(stderr, "time-constraint policy refused\n");
	if(sched)
		*sched = CurrentThreadSched();
	const uint64_t period = (uint64_t)((double)2000000 / ((double)MachToNanos(1000000) / 1000000.0));
	uint64_t deadline = mach_absolute_time() + period;
	while(*run)
	{
		mach_wait_until(deadline);
		const uint64_t now = mach_absolute_time();
		late_us->push_back((double)MachToNanos(now - deadline) / 1e3);
		deadline += period;
		if(deadline < now)
			deadline = now + period;
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
	std::vector<double> sensor_us, hid_us, host_us; // for the delay fit, enhanced only
	size_t seq_gaps = 0, seq_missing = 0, nibble_gaps = 0, sensor_repeats = 0;
	uint64_t sensor_ticks = 0;
	for(size_t i = 0; i < s.size(); i++)
	{
		dispatch_us.push_back((double)(MachToNanos(s[i].host_time) - MachToNanos(s[i].hid_time)) / 1e3);
		if(s[i].mode == ReportMode::Enhanced)
		{
			if(i > 0 && s[i - 1].mode == ReportMode::Enhanced)
				sensor_ticks += (uint32_t)(s[i].sensor - s[i - 1].sensor);
			sensor_us.push_back((double)sensor_ticks / 3.0);
			hid_us.push_back((double)MachToNanos(s[i].hid_time) / 1e3);
			host_us.push_back((double)MachToNanos(s[i].host_time) / 1e3);
		}
		if(i == 0)
			continue;
		host_ms.push_back((double)(MachToNanos(s[i].host_time) - MachToNanos(s[i - 1].host_time)) / 1e6);
		hid_ms.push_back((double)(MachToNanos(s[i].hid_time) - MachToNanos(s[i - 1].hid_time)) / 1e6);
		if(s[i].mode == ReportMode::Enhanced && s[i - 1].mode == ReportMode::Enhanced)
		{
			const uint32_t dt = s[i].sensor - s[i - 1].sensor;
			sensor_ms.push_back((double)dt / 3000.0);
			if(dt == 0)
				sensor_repeats++;
			const uint32_t ds = s[i].sequence - s[i - 1].sequence;
			if(ds != 1)
			{
				seq_gaps++;
				seq_missing += ds - 1;
			}
			if((((s[i].bt_header >> 4) - (s[i - 1].bt_header >> 4)) & 0x0F) != 1)
				nibble_gaps++;
		}
	}
	PrintDist("interval, host callback time", "ms", Distribution(host_ms));
	PrintDist("interval, HID timestamp", "ms", Distribution(hid_ms));
	PrintDist("interval, controller sensor clock", "ms", Distribution(sensor_ms));
	PrintDist("HID timestamp -> callback", "us", Distribution(dispatch_us));

	// How much later than its best case each report arrived, measured against the
	// controller's own clock: fit host time = a + b * sensor time (least squares,
	// removes clock offset and drift), then take residuals above the minimum. This
	// is the jitter of the whole path (radio, retransmissions, bluetoothd, kernel,
	// our wake-up); the constant part of the delay is not observable this way.
	auto extra_delay = [](const std::vector<double> &x, const std::vector<double> &y) {
		const size_t n = x.size();
		double mx = 0, my = 0;
		for(size_t i = 0; i < n; i++)
		{
			mx += x[i];
			my += y[i];
		}
		mx /= (double)n;
		my /= (double)n;
		double sxy = 0, sxx = 0;
		for(size_t i = 0; i < n; i++)
		{
			sxy += (x[i] - mx) * (y[i] - my);
			sxx += (x[i] - mx) * (x[i] - mx);
		}
		const double b = sxx > 0 ? sxy / sxx : 1.0;
		std::vector<double> r(n);
		for(size_t i = 0; i < n; i++)
			r[i] = y[i] - (my + b * (x[i] - mx));
		const double lo = *std::min_element(r.begin(), r.end());
		for(double &v : r)
			v = (v - lo) / 1000.0;
		return r;
	};
	if(sensor_us.size() > 10)
	{
		PrintDist("delay above best, HID timestamp", "ms", Distribution(extra_delay(sensor_us, hid_us)));
		PrintDist("delay above best, callback", "ms", Distribution(extra_delay(sensor_us, host_us)));
	}
	printf("  report sequence (payload 11..14) gaps: %zu, reports lost: %zu\n", seq_gaps, seq_missing);
	if(s.front().report_id == 0x31)
		printf("  Bluetooth header sequence (byte 1 high nibble) gaps: %zu\n", nibble_gaps);
	printf("  repeated sensor timestamps: %zu\n", sensor_repeats);
	if(span_ms > 0 && sensor_ticks)
		printf("  sensor clock: %.4f ticks per us of host time (nominal 3)\n", (double)sensor_ticks / (span_ms * 1000.0));
	PrintHistogram("HID-timestamp interval", hid_ms);
}

// Opens no device. Same wake-up test as during a measurement, once per scheduling
// policy, so the effect of the process context on our threads can be seen on its own.
int RunSchedTest(double seconds)
{
	printf("Scheduling test (no controller), task role %s\n", TaskRoleName());
	for(int rt = 0; rt < 2; rt++)
	{
		std::atomic<bool> run{true};
		std::vector<double> late;
		late.reserve((size_t)(seconds * 600) + 16);
		ThreadSched sched;
		std::thread t(WakeupLatencyThread, &run, &late, rt == 1, &sched);
		std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
		run = false;
		t.join();
		printf("  %s: priority %d (base %d)\n", rt ? "time-constraint policy" : "QoS user-interactive", sched.cur_pri,
			sched.base_pri);
		PrintDist("timer wake-up lateness, 2 ms period", "us", Distribution(late));
	}
	return 0;
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
	std::string dump_path, calibration_path, csv_path;
	bool realtime = false;
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
		else if(a == "--timing-csv")
			csv_path = next_arg();
		else if(a == "--rumble")
			mode = Mode::Rumble;
		else if(a == "--trigger")
			mode = Mode::Trigger;
		else if(a == "--led")
			mode = Mode::Led;
		else if(a == "--haptics-tone")
			mode = Mode::HapticsTone;
		else if(a == "--sched-test")
			mode = Mode::SchedTest;
		else if(a == "--realtime")
			realtime = true;
		else
		{
			Usage();
			return a == "-h" || a == "--help" ? 0 : 2;
		}
	}
	if(mode == Mode::SchedTest)
		return RunSchedTest(std::clamp(seconds, 1.0, 10.0));
	const bool write_mode = mode != Mode::Measure;
	if(!write_mode)
		seconds = std::clamp(seconds, 1.0, 10.0);

	std::mutex mutex;
	std::condition_variable cond;
	std::shared_ptr<Device> device;
	std::atomic<bool> recording{false};
	std::atomic<uint64_t> start_mach{0};
	std::vector<Sample> samples;
	samples.reserve((size_t)(seconds * 4000) + 16);
	InputState last_state;
	ThreadSched hid_sched;
	std::atomic<bool> wakeup_run{false};
	std::vector<double> wakeup_late_us;
	wakeup_late_us.reserve((size_t)(seconds * 600) + 16);
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
	options.realtime_threads = realtime;
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
		if(!dump || !recording || hid_time < start_mach || dumped >= dump_limit || &d != device.get())
			return;
		if(!dumped)
			dump_t0 = hid_time;
		fprintf(dump, "%llu %s\n", (unsigned long long)(MachToNanos(hid_time - dump_t0) / 1000), Hex(report, size).c_str());
		dumped++;
	};
	cb.input = [&](Device &d, const InputState &s) {
		// Reports queued before the run started (e.g. during the feature reads at open)
		// would show up as late deliveries: skip them.
		if(!recording || s.hid_time < start_mach || &d != device.get())
			return;
		last_state = s;
		if(!hid_sched.valid)
			hid_sched = CurrentThreadSched(); // callbacks run on the HID thread
		if(samples.size() == samples.capacity())
			return;
		samples.push_back({ s.hid_time, s.host_time, s.sensor_timestamp, s.sequence, s.bt_header, s.report_id, s.mode });
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

	wakeup_run = true;
	std::thread wakeup(WakeupLatencyThread, &wakeup_run, &wakeup_late_us, false, nullptr);
	start_mach = NowMach();
	recording = true;
	std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
	recording = false;
	wakeup_run = false;
	wakeup.join();
	InputStats stats = device->input_stats();
	manager.Stop(); // joins the HID thread: `samples` is ours from here
	if(dump)
		fclose(dump);
	Analyse(samples, stats, seconds);
	printf("  process: task role %s\n", TaskRoleName());
	if(hid_sched.valid)
		printf("  HID thread: QoS %s, priority %d (base %d)\n", QosName(hid_sched.qos), hid_sched.cur_pri, hid_sched.base_pri);
	PrintDist("timer wake-up lateness, 2 ms period", "us", Distribution(wakeup_late_us));
	if(!csv_path.empty())
	{
		FILE *f = fopen(csv_path.c_str(), "w");
		if(f)
		{
			fprintf(f, "hid_ns,callback_ns,sensor_ticks,sequence,bt_header\n");
			for(const Sample &x : samples)
				fprintf(f, "%llu,%llu,%u,%u,%u\n", (unsigned long long)MachToNanos(x.hid_time),
					(unsigned long long)MachToNanos(x.host_time), x.sensor, x.sequence, x.bt_header);
			fclose(f);
			printf("  wrote per-report timing to %s\n", csv_path.c_str());
		}
	}
	if(last_state.battery_valid)
		printf("  battery: %u%% (%s)%s\n", last_state.battery_percent, BatteryName(last_state.battery_status),
			last_state.headphones ? ", headphones plugged in" : "");
	if(dump)
		printf("  dumped %zu raw reports to %s\n", dumped, dump_path.c_str());
	return 0;
}
