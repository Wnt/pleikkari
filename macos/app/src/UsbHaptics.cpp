// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "UsbHaptics.h"

#include "AudioOutput.h"

#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace plk::app {

namespace {

constexpr size_t kUpsample = 16; // 3000 Hz -> 48000 Hz
constexpr uint32_t kChannels = 4;

std::string deviceName(AudioObjectID id)
{
	AudioObjectPropertyAddress addr = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMain };
	CFStringRef name = nullptr;
	UInt32 size = sizeof(name);
	if(AudioObjectGetPropertyData(id, &addr, 0, nullptr, &size, &name) != noErr || !name)
		return std::string();
	char buf[256];
	std::string out;
	if(CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8))
		out = buf;
	CFRelease(name);
	return out;
}

uint32_t outputChannels(AudioObjectID id)
{
	AudioObjectPropertyAddress addr = { kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput,
		kAudioObjectPropertyElementMain };
	UInt32 size = 0;
	if(AudioObjectGetPropertyDataSize(id, &addr, 0, nullptr, &size) != noErr || !size)
		return 0;
	std::vector<uint8_t> storage(size);
	auto *list = reinterpret_cast<AudioBufferList *>(storage.data());
	if(AudioObjectGetPropertyData(id, &addr, 0, nullptr, &size, list) != noErr)
		return 0;
	uint32_t channels = 0;
	for(UInt32 i = 0; i < list->mNumberBuffers; i++)
		channels += list->mBuffers[i].mNumberChannels;
	return channels;
}

int16_t clampS16(float v)
{
	return (int16_t)std::clamp(std::lround(v), (long)INT16_MIN, (long)INT16_MAX);
}

} // namespace

std::vector<AudioDeviceInfo> findDualSenseAudioDevices()
{
	std::vector<AudioDeviceInfo> out;
	AudioObjectPropertyAddress addr = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMain };
	UInt32 size = 0;
	if(AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &size) != noErr || !size)
		return out;
	std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
	if(AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, ids.data()) != noErr)
		return out;
	for(AudioObjectID id : ids)
	{
		const std::string name = deviceName(id);
		if(name.find("DualSense") == std::string::npos && name.find("Wireless Controller") == std::string::npos)
			continue;
		const uint32_t ch = outputChannels(id);
		if(!ch)
			continue; // the microphone side
		out.push_back({ id, name, ch });
	}
	return out;
}

size_t upsampleHaptics(const int16_t *in, size_t in_frames, float gain, int16_t prev[2], int16_t *out)
{
	size_t o = 0;
	for(size_t i = 0; i < in_frames; i++)
	{
		const float l1 = in[2 * i] * gain, r1 = in[2 * i + 1] * gain;
		const float l0 = prev[0], r0 = prev[1];
		for(size_t k = 1; k <= kUpsample; k++)
		{
			const float t = (float)k / (float)kUpsample;
			int16_t *f = out + o * kChannels;
			f[0] = 0;
			f[1] = 0;
			f[2] = clampS16(l0 + (l1 - l0) * t);
			f[3] = clampS16(r0 + (r1 - r0) * t);
			o++;
		}
		prev[0] = clampS16(l1);
		prev[1] = clampS16(r1);
	}
	return o;
}

UsbHaptics::UsbHaptics() = default;

UsbHaptics::~UsbHaptics()
{
	close();
}

bool UsbHaptics::open(const AudioDeviceInfo &device, std::string *error)
{
	close();
	AudioOutputConfig config;
	config.device = device.id;
	config.follow_default_device = false;
	config.start_on_format = false;
	config.preferred_io_frames = 128;
	auto output = std::make_unique<AudioOutput>(config);
	if(!output->open(kChannels, 48000, error))
		return false;
	if(!output->start(error))
		return false;
	output_ = std::move(output);
	device_ = device;
	prev_[0] = prev_[1] = 0;
	return true;
}

void UsbHaptics::close()
{
	if(!output_)
		return;
	output_->stop();
	output_->close();
	output_.reset();
}

void UsbHaptics::push(const int16_t *stereo, size_t samples, float gain)
{
	if(!output_ || samples < 2)
		return;
	const size_t in_frames = samples / 2;
	buf_.resize(in_frames * kUpsample * kChannels);
	const size_t frames = upsampleHaptics(stereo, in_frames, gain, prev_, buf_.data());
	output_->onPcm(buf_.data(), frames);
}

std::string UsbHaptics::statsLine() const
{
	if(!output_)
		return std::string();
	const AudioOutputStats s = output_->stats();
	char buf[256];
	snprintf(buf, sizeof(buf), " usb_haptics=\"%s\" ch=%u underruns=%llu fill_avg_ms=%.2f latency_ms=%.2f",
			device_.name.c_str(), device_.output_channels, (unsigned long long)s.playout.underruns,
			s.playout.avg_fill_ms, s.latency.total_ms);
	return buf;
}

} // namespace plk::app
