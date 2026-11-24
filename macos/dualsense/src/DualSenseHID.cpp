// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// IOKit side of the driver: device matching and hotplug through IOHIDManager on a
// dedicated CFRunLoop thread, input reports through
// IOHIDDeviceRegisterInputReportWithTimeStampCallback (delivered on that thread the
// moment the HID system queues them, with the HID system's own timestamp), output
// through IOHIDDeviceSetReport from the writer and haptics threads.

#include "DualSenseHID.h"
#include "EffectsWriter.h"
#include "HapticsSender.h"
#include "Protocol.h"
#include "ThreadUtil.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDManager.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <thread>

namespace plk::dualsense {

using namespace protocol;

uint64_t NowMach()
{
	return mach_absolute_time();
}

uint64_t MachToNanos(uint64_t mach_time)
{
	static const mach_timebase_info_data_t timebase = [] {
		mach_timebase_info_data_t tb;
		mach_timebase_info(&tb);
		return tb;
	}();
	return (uint64_t)((unsigned __int128)mach_time * timebase.numer / timebase.denom);
}

namespace {

int32_t IntProperty(IOHIDDeviceRef device, CFStringRef key, int32_t def = 0)
{
	CFTypeRef ref = IOHIDDeviceGetProperty(device, key);
	int32_t v = def;
	if(ref && CFGetTypeID(ref) == CFNumberGetTypeID())
		CFNumberGetValue((CFNumberRef)ref, kCFNumberSInt32Type, &v);
	return v;
}

std::string StringProperty(IOHIDDeviceRef device, CFStringRef key)
{
	CFTypeRef ref = IOHIDDeviceGetProperty(device, key);
	if(!ref || CFGetTypeID(ref) != CFStringGetTypeID())
		return std::string();
	char buf[256];
	if(!CFStringGetCString((CFStringRef)ref, buf, sizeof(buf), kCFStringEncodingUTF8))
		return std::string();
	return std::string(buf);
}

CFDictionaryRef MatchingDictionary(int vendor, int product)
{
	CFMutableDictionaryRef d = CFDictionaryCreateMutable(kCFAllocatorDefault, 2,
		&kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFNumberRef v = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vendor);
	CFNumberRef p = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &product);
	CFDictionarySetValue(d, CFSTR(kIOHIDVendorIDKey), v);
	CFDictionarySetValue(d, CFSTR(kIOHIDProductIDKey), p);
	CFRelease(v);
	CFRelease(p);
	return d;
}

std::vector<uint8_t> GetFeatureReport(IOHIDDeviceRef device, uint8_t id, size_t size)
{
	std::vector<uint8_t> buf(size, 0);
	buf[0] = id;
	CFIndex length = (CFIndex)size;
	if(IOHIDDeviceGetReport(device, kIOHIDReportTypeFeature, id, buf.data(), &length) != kIOReturnSuccess || length <= 0)
		return {};
	buf.resize((size_t)length);
	return buf;
}

// The only place this driver calls IOHIDDeviceSetReport. One write in flight per
// device; refuses everything unless output was allowed, and after Close().
class IOKitSink : public OutputSink {
public:
	IOKitSink(IOHIDDeviceRef device, bool allow_output) : device_(device), allow_(allow_output) {}

	bool Write(const uint8_t *data, size_t size) override
	{
		std::lock_guard<std::mutex> lk(mutex_);
		if(!allow_ || closed_ || !size)
			return false;
		return IOHIDDeviceSetReport(device_, kIOHIDReportTypeOutput, data[0], data, (CFIndex)size) == kIOReturnSuccess;
	}

	void Close()
	{
		std::lock_guard<std::mutex> lk(mutex_);
		closed_ = true;
	}

private:
	std::mutex mutex_;
	IOHIDDeviceRef device_;
	const bool allow_;
	bool closed_ = false;
};

} // namespace

// ---------------------------------------------------------------------------

struct Device::Impl {
	Impl(IOHIDDeviceRef ref, bool allow_output) : ref(ref), allow_output(allow_output), sink(ref, allow_output)
	{
		CFRetain(ref);
	}
	~Impl()
	{
		if(haptics)
			haptics->Stop();
		if(writer)
			writer->Stop(false);
		CFRelease(ref);
	}

	void HandleReport(const uint8_t *report, size_t size, uint64_t hid_time, uint64_t host_time);
	// HID thread only. send_off: the device is still there (Manager::Stop), so write the
	// effects-off report first; false: it is gone.
	void Shutdown(bool send_off);

	Device *owner = nullptr;
	Manager::Callbacks *callbacks = nullptr;
	CFRunLoopRef run_loop = nullptr;

	DeviceInfo info;
	IOHIDDeviceRef ref;
	const bool allow_output;
	bool realtime_threads = false;
	bool bluetooth = false;

	// HID thread only
	Calibration calibration;
	InputParser parser;
	std::vector<uint8_t> input_buffer;
	bool leds_ready_sent = false;
	bool scheduled = false;

	std::atomic<bool> connected{true};
	std::atomic<uint64_t> reports{0}, crc_errors{0}, other_reports{0};

	IOKitSink sink;
	std::unique_ptr<EffectsWriter> writer;
	mutable std::mutex haptics_mutex;
	std::unique_ptr<HapticsSender> haptics;
};

void Device::Impl::HandleReport(const uint8_t *report, size_t size, uint64_t hid_time, uint64_t host_time)
{
	if(callbacks->raw_input)
		callbacks->raw_input(*owner, report, size, hid_time);
	InputState state;
	switch(parser.Parse(report, size, bluetooth, calibration, &state))
	{
		case ParseResult::Ok:
			break;
		case ParseResult::BadCrc:
			crc_errors.fetch_add(1, std::memory_order_relaxed);
			return;
		case ParseResult::Unsupported:
			other_reports.fetch_add(1, std::memory_order_relaxed);
			return;
	}
	state.hid_time = hid_time;
	state.host_time = host_time;
	reports.fetch_add(1, std::memory_order_relaxed);
	if(writer && !leds_ready_sent
		&& (state.mode == ReportMode::BluetoothSimple || state.sensor_timestamp >= kLedAnimationDoneTicks))
	{
		writer->LedsReady();
		leds_ready_sent = true;
	}
	if(callbacks->input)
		callbacks->input(*owner, state);
}

void Device::Impl::Shutdown(bool send_off)
{
	connected = false;
	std::unique_ptr<HapticsSender> h;
	{
		std::lock_guard<std::mutex> lk(haptics_mutex);
		h = std::move(haptics);
	}
	if(h)
		h->Stop();
	if(!send_off)
		sink.Close();
	if(writer)
		writer->Stop(send_off);
	sink.Close();
	if(scheduled)
	{
		IOHIDDeviceRegisterInputReportWithTimeStampCallback(ref, input_buffer.data(), (CFIndex)input_buffer.size(), nullptr, nullptr);
		IOHIDDeviceUnscheduleFromRunLoop(ref, run_loop, kCFRunLoopDefaultMode);
		IOHIDDeviceClose(ref, kIOHIDOptionsTypeNone);
		scheduled = false;
	}
}

Device::Device(std::unique_ptr<Impl> impl) : impl_(std::move(impl))
{
	impl_->owner = this;
}

Device::~Device() = default;

const DeviceInfo &Device::info() const { return impl_->info; }
bool Device::connected() const { return impl_->connected; }
bool Device::output_enabled() const { return impl_->writer != nullptr; }

InputStats Device::input_stats() const
{
	InputStats s;
	s.reports = impl_->reports;
	s.crc_errors = impl_->crc_errors;
	s.other_reports = impl_->other_reports;
	return s;
}

#define PLK_WRITER_OR_RETURN \
	if(!impl_->writer || !impl_->connected) \
		return; \
	EffectsWriter &w = *impl_->writer

void Device::SetRumble(uint8_t left, uint8_t right) { PLK_WRITER_OR_RETURN; w.SetRumble(left, right); }
void Device::SetTriggerEffects(const uint8_t left[11], const uint8_t right[11], uint8_t mask) { PLK_WRITER_OR_RETURN; w.SetTriggerEffects(left, right, mask); }
void Device::SetMotorPowerLevel(uint8_t value) { PLK_WRITER_OR_RETURN; w.SetMotorPowerLevel(value); }
void Device::SetLightbar(uint8_t r, uint8_t g, uint8_t b) { PLK_WRITER_OR_RETURN; w.SetLightbar(r, g, b); }
void Device::SetPlayerLeds(uint8_t mask) { PLK_WRITER_OR_RETURN; w.SetPlayerLeds(mask); }
void Device::SetMic(MicLed led, uint8_t power_save_bits) { PLK_WRITER_OR_RETURN; w.SetMic((uint8_t)led, power_save_bits); }
void Device::ClearEffects() { PLK_WRITER_OR_RETURN; w.ClearEffects(); }
uint8_t Device::PlayerLedsForIndex(int player_index) { return protocol::PlayerLedsForIndex(player_index); }

#undef PLK_WRITER_OR_RETURN

WriterStats Device::writer_stats() const
{
	return impl_->writer ? impl_->writer->stats() : WriterStats();
}

bool Device::StartHaptics(const HapticsConfig &config)
{
	if(!impl_->allow_output || !impl_->bluetooth || !impl_->connected)
		return false;
	std::lock_guard<std::mutex> lk(impl_->haptics_mutex);
	if(impl_->haptics && impl_->haptics->running())
		return true;
	if(impl_->haptics)
		impl_->haptics->Stop();
	impl_->haptics = std::make_unique<HapticsSender>(&impl_->sink, config, impl_->realtime_threads);
	impl_->haptics->Start();
	return true;
}

void Device::StopHaptics()
{
	std::unique_ptr<HapticsSender> h;
	{
		std::lock_guard<std::mutex> lk(impl_->haptics_mutex);
		h = std::move(impl_->haptics);
	}
	if(h)
		h->Stop();
}

bool Device::haptics_running() const
{
	std::lock_guard<std::mutex> lk(impl_->haptics_mutex);
	return impl_->haptics && impl_->haptics->running();
}

void Device::PushHaptics(const int8_t *interleaved, size_t bytes)
{
	std::lock_guard<std::mutex> lk(impl_->haptics_mutex);
	if(impl_->haptics)
		impl_->haptics->Push(interleaved, bytes);
}

void Device::PushHapticsPcm16(const int16_t *interleaved, size_t samples, float gain)
{
	std::lock_guard<std::mutex> lk(impl_->haptics_mutex);
	if(impl_->haptics)
		impl_->haptics->PushPcm16(interleaved, samples, gain);
}

bool Device::HapticsRecentlyNonSilent(uint32_t window_ms) const
{
	std::lock_guard<std::mutex> lk(impl_->haptics_mutex);
	return impl_->haptics && impl_->haptics->RecentlyNonSilent(window_ms);
}

HapticsStats Device::haptics_stats() const
{
	std::lock_guard<std::mutex> lk(impl_->haptics_mutex);
	return impl_->haptics ? impl_->haptics->stats() : HapticsStats();
}

// ---------------------------------------------------------------------------

struct Manager::Impl {
	void ThreadMain();
	void Log(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
	void OnMatched(IOHIDDeviceRef ref);
	void OnRemoved(IOHIDDeviceRef ref);

	static void MatchCallback(void *ctx, IOReturn, void *, IOHIDDeviceRef ref)
	{
		static_cast<Impl *>(ctx)->OnMatched(ref);
	}
	static void RemoveCallback(void *ctx, IOReturn, void *, IOHIDDeviceRef ref)
	{
		static_cast<Impl *>(ctx)->OnRemoved(ref);
	}
	static void ReportCallback(void *ctx, IOReturn result, void *, IOHIDReportType type,
		uint32_t, uint8_t *report, CFIndex length, uint64_t timestamp)
	{
		const uint64_t now = mach_absolute_time();
		if(result != kIOReturnSuccess || type != kIOHIDReportTypeInput || length <= 0)
			return;
		static_cast<Device::Impl *>(ctx)->HandleReport(report, (size_t)length, timestamp, now);
	}
	static void StopPerform(void *)
	{
		CFRunLoopStop(CFRunLoopGetCurrent());
	}

	Options options;
	Callbacks callbacks;
	std::thread thread;
	CFRunLoopRef run_loop = nullptr;
	CFRunLoopSourceRef stop_source = nullptr;
	IOHIDManagerRef hid_manager = nullptr;

	mutable std::mutex devices_mutex;
	std::vector<std::shared_ptr<Device>> devices;

	std::mutex start_mutex;
	std::condition_variable start_cond;
	bool start_done = false;
	bool start_ok = false;
};

void Manager::Impl::Log(const char *fmt, ...)
{
	if(!callbacks.log)
		return;
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	callbacks.log(buf);
}

void Manager::Impl::ThreadMain()
{
	SetupDriverThread("plk.ds.hid", options.realtime_threads, 0, kHidThreadComputationNs, kHidThreadConstraintNs);
	run_loop = CFRunLoopGetCurrent();
	CFRetain(run_loop);

	CFRunLoopSourceContext source_context = {};
	source_context.perform = StopPerform;
	stop_source = CFRunLoopSourceCreate(kCFAllocatorDefault, 0, &source_context);
	CFRunLoopAddSource(run_loop, stop_source, kCFRunLoopDefaultMode);

	hid_manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
	bool ok = hid_manager != nullptr;
	if(ok)
	{
		CFDictionaryRef dicts[2] = {
			MatchingDictionary(kVendorSony, kProductDualSense),
			MatchingDictionary(kVendorSony, kProductDualSenseEdge),
		};
		CFArrayRef matching = CFArrayCreate(kCFAllocatorDefault, (const void **)dicts, 2, &kCFTypeArrayCallBacks);
		IOHIDManagerSetDeviceMatchingMultiple(hid_manager, matching);
		CFRelease(matching);
		CFRelease(dicts[0]);
		CFRelease(dicts[1]);
		IOHIDManagerRegisterDeviceMatchingCallback(hid_manager, MatchCallback, this);
		IOHIDManagerRegisterDeviceRemovalCallback(hid_manager, RemoveCallback, this);
		// The manager itself is never opened: IOHIDManagerOpen would open every matched
		// device with its options. Devices are opened one by one, non-exclusively.
		IOHIDManagerScheduleWithRunLoop(hid_manager, run_loop, kCFRunLoopDefaultMode);
	}
	{
		std::lock_guard<std::mutex> lk(start_mutex);
		start_done = true;
		start_ok = ok;
	}
	start_cond.notify_all();

	if(ok)
		CFRunLoopRun(); // until StopPerform

	std::vector<std::shared_ptr<Device>> devs;
	{
		std::lock_guard<std::mutex> lk(devices_mutex);
		devs.swap(devices);
	}
	for(auto &d : devs)
		d->impl()->Shutdown(true);
	if(hid_manager)
	{
		IOHIDManagerRegisterDeviceMatchingCallback(hid_manager, nullptr, nullptr);
		IOHIDManagerRegisterDeviceRemovalCallback(hid_manager, nullptr, nullptr);
		IOHIDManagerUnscheduleFromRunLoop(hid_manager, run_loop, kCFRunLoopDefaultMode);
		CFRelease(hid_manager);
		hid_manager = nullptr;
	}
	CFRunLoopRemoveSource(run_loop, stop_source, kCFRunLoopDefaultMode);
	CFRelease(stop_source);
	stop_source = nullptr;
	CFRelease(run_loop);
	run_loop = nullptr;
}

void Manager::Impl::OnMatched(IOHIDDeviceRef ref)
{
	const int vendor = IntProperty(ref, CFSTR(kIOHIDVendorIDKey));
	const int product = IntProperty(ref, CFSTR(kIOHIDProductIDKey));
	if(vendor != kVendorSony || (product != kProductDualSense && product != kProductDualSenseEdge))
		return;
	{
		std::lock_guard<std::mutex> lk(devices_mutex);
		for(auto &d : devices)
			if(d->impl()->ref == ref)
				return;
	}

	auto impl = std::make_unique<Device::Impl>(ref, options.allow_output);
	Device::Impl &d = *impl;
	d.realtime_threads = options.realtime_threads;
	d.callbacks = &callbacks;
	d.run_loop = run_loop;
	DeviceInfo &info = d.info;
	info.vendor_id = (uint16_t)vendor;
	info.product_id = (uint16_t)product;
	info.model = product == kProductDualSenseEdge ? Model::DualSenseEdge : Model::DualSense;
	const std::string transport = StringProperty(ref, CFSTR(kIOHIDTransportKey));
	d.bluetooth = transport.find("Bluetooth") != std::string::npos;
	info.connection = d.bluetooth ? Connection::Bluetooth : Connection::USB;
	info.product_name = StringProperty(ref, CFSTR(kIOHIDProductKey));
	info.max_input_report_size = (size_t)std::max(0, IntProperty(ref, CFSTR(kIOHIDMaxInputReportSizeKey)));
	info.max_output_report_size = (size_t)std::max(0, IntProperty(ref, CFSTR(kIOHIDMaxOutputReportSizeKey)));
	io_service_t service = IOHIDDeviceGetService(ref);
	if(service)
		IORegistryEntryGetRegistryEntryID(service, &info.registry_id);

	const IOReturn open_result = IOHIDDeviceOpen(ref, kIOHIDOptionsTypeNone);
	if(open_result != kIOReturnSuccess)
	{
		Log("DualSense %s: IOHIDDeviceOpen failed: 0x%08x", transport.c_str(), open_result);
		return;
	}

	// Feature reads: firmware and IMU calibration. Over Bluetooth this also switches a
	// controller that is still sending the simple 0x01 report to the enhanced 0x31
	// report. Feature 0x09 (pairing info, contains the MAC address) is not read.
	info.raw_firmware_info = GetFeatureReport(ref, kFeatureIdFirmwareInfo, kFirmwareInfoSize);
	FirmwareInfo fw;
	if(ParseFirmwareInfo(info.raw_firmware_info.data(), info.raw_firmware_info.size(), d.bluetooth, &fw))
	{
		info.firmware_valid = true;
		info.update_version = fw.update_version;
		info.hardware_version = fw.hardware_version;
		info.firmware_version = fw.firmware_version;
		info.build_date.assign(fw.build_date, strnlen(fw.build_date, sizeof(fw.build_date)));
		info.build_time.assign(fw.build_time, strnlen(fw.build_time, sizeof(fw.build_time)));
	}
	else
		Log("DualSense: firmware info unavailable (%zu bytes)", info.raw_firmware_info.size());
	info.raw_calibration = GetFeatureReport(ref, kFeatureIdCalibration, kCalibrationSize);
	info.calibration_valid = ParseCalibration(info.raw_calibration.data(), info.raw_calibration.size(), d.bluetooth, &d.calibration);
	if(!info.calibration_valid)
		Log("DualSense: IMU calibration unavailable or rejected (%zu bytes), using nominal scaling", info.raw_calibration.size());
	info.rumble_v2 = UseRumbleV2(info.product_id, info.firmware_valid ? info.update_version : 0);

	d.input_buffer.resize(std::max<size_t>(info.max_input_report_size, 128));
	IOHIDDeviceRegisterInputReportWithTimeStampCallback(ref, d.input_buffer.data(), (CFIndex)d.input_buffer.size(), ReportCallback, &d);
	IOHIDDeviceScheduleWithRunLoop(ref, run_loop, kCFRunLoopDefaultMode);
	d.scheduled = true;

	if(options.allow_output)
	{
		EffectsWriter::Config wc;
		wc.bluetooth = d.bluetooth;
		wc.rumble_v2 = info.rumble_v2;
		wc.time_constraint = options.realtime_threads;
		wc.min_interval_us = options.effects_min_interval_us;
		d.writer = std::make_unique<EffectsWriter>(&d.sink, wc);
		d.writer->Start();
	}

	auto device = std::make_shared<Device>(std::move(impl));
	{
		std::lock_guard<std::mutex> lk(devices_mutex);
		devices.push_back(device);
	}
	if(callbacks.connected)
		callbacks.connected(device);
}

void Manager::Impl::OnRemoved(IOHIDDeviceRef ref)
{
	std::shared_ptr<Device> device;
	{
		std::lock_guard<std::mutex> lk(devices_mutex);
		auto it = std::find_if(devices.begin(), devices.end(), [ref](const std::shared_ptr<Device> &d) { return d->impl()->ref == ref; });
		if(it == devices.end())
			return;
		device = *it;
		devices.erase(it);
	}
	device->impl()->Shutdown(false);
	if(callbacks.disconnected)
		callbacks.disconnected(device);
}

Manager::Manager() : impl_(std::make_unique<Impl>()) {}

Manager::~Manager()
{
	Stop();
}

bool Manager::Start(const Options &options, Callbacks callbacks)
{
	if(impl_->thread.joinable())
		return false;
	impl_->options = options;
	impl_->callbacks = std::move(callbacks);
	impl_->start_done = false;
	impl_->thread = std::thread([this] { impl_->ThreadMain(); });
	std::unique_lock<std::mutex> lk(impl_->start_mutex);
	impl_->start_cond.wait(lk, [this] { return impl_->start_done; });
	if(!impl_->start_ok)
	{
		lk.unlock();
		impl_->thread.join();
		return false;
	}
	return true;
}

void Manager::Stop()
{
	if(!impl_->thread.joinable())
		return;
	CFRunLoopSourceSignal(impl_->stop_source);
	CFRunLoopWakeUp(impl_->run_loop);
	impl_->thread.join();
}

std::vector<std::shared_ptr<Device>> Manager::devices() const
{
	std::lock_guard<std::mutex> lk(impl_->devices_mutex);
	return impl_->devices;
}

} // namespace plk::dualsense
