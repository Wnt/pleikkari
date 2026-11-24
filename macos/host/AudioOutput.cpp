// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "AudioOutput.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace plk {

struct AudioOutput::ListenerContext
{
	std::mutex mutex;
	AudioOutput *owner = nullptr;
	dispatch_queue_t queue = nullptr;
};

namespace {

const AudioObjectPropertyAddress kDefaultOutputAddress = {
	kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };

// Properties of the bound device that mean "bind again".
const AudioObjectPropertyAddress kDeviceRebindAddresses[] = {
	{ kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
	{ kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
};

// Properties that only change the latency we report.
const AudioObjectPropertyAddress kDeviceLatencyAddresses[] = {
	{ kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain },
	{ kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain },
	{ kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
};

std::string statusString(const char *what, OSStatus status)
{
	char buf[160];
	uint32_t be = (uint32_t)status;
	char cc[5] = { (char)(be >> 24), (char)(be >> 16), (char)(be >> 8), (char)be, 0 };
	bool printable = true;
	for(int i = 0; i < 4; i++)
		printable = printable && cc[i] >= 0x20 && cc[i] < 0x7f;
	if(printable)
		snprintf(buf, sizeof(buf), "%s failed: '%s' (%d)", what, cc, (int)status);
	else
		snprintf(buf, sizeof(buf), "%s failed: %d", what, (int)status);
	return buf;
}

template <typename T>
bool getProperty(AudioObjectID object, AudioObjectPropertySelector selector, AudioObjectPropertyScope scope, T &out)
{
	AudioObjectPropertyAddress address = { selector, scope, kAudioObjectPropertyElementMain };
	UInt32 size = sizeof(T);
	return AudioObjectGetPropertyData(object, &address, 0, nullptr, &size, &out) == noErr && size == sizeof(T);
}

} // namespace

AudioOutput::AudioOutput(const AudioOutputConfig &config)
	: config_(config), ring_(config.ring_frames ? config.ring_frames : 16384, 8)
{
	queue_ = dispatch_queue_create("plk.audio-output", DISPATCH_QUEUE_SERIAL);
	listener_ctx_ = new ListenerContext;
	listener_ctx_->owner = this;
	listener_ctx_->queue = queue_;
	dispatch_retain(queue_); // the context's own reference
}

AudioOutput::~AudioOutput()
{
	{
		std::lock_guard<std::mutex> lock(control_mutex_);
		closeLocked();
	}
	{
		std::lock_guard<std::mutex> lock(listener_ctx_->mutex);
		listener_ctx_->owner = nullptr;
	}
	dispatch_sync_f(queue_, nullptr, [](void *) {}); // let queued rebinds see owner == nullptr
	dispatch_release(queue_);
	// listener_ctx_ (and the queue reference it holds) is leaked on purpose: CoreAudio does
	// not promise that a notification already running on one of its threads has finished
	// when AudioObjectRemovePropertyListener() returns, and that notification dereferences it.
}

void AudioOutput::blockProducer()
{
	accepting_.store(false, std::memory_order_seq_cst);
	while(producer_active_.load(std::memory_order_seq_cst) != 0)
		std::this_thread::yield();
}

void AudioOutput::unblockProducer()
{
	accepting_.store(true, std::memory_order_seq_cst);
}

bool AudioOutput::open(uint32_t channels, uint32_t rate, std::string *error)
{
	std::lock_guard<std::mutex> lock(control_mutex_);
	return openLocked(channels, rate, false, error);
}

bool AudioOutput::openOffline(uint32_t channels, uint32_t rate)
{
	std::lock_guard<std::mutex> lock(control_mutex_);
	return openLocked(channels, rate, true, nullptr);
}

bool AudioOutput::start(std::string *error)
{
	std::lock_guard<std::mutex> lock(control_mutex_);
	return startLocked(error);
}

void AudioOutput::stop()
{
	std::lock_guard<std::mutex> lock(control_mutex_);
	stopLocked();
}

void AudioOutput::close()
{
	std::lock_guard<std::mutex> lock(control_mutex_);
	closeLocked();
}

void AudioOutput::setGain(float gain)
{
	std::lock_guard<std::mutex> lock(control_mutex_);
	gain_ = gain;
	if(playout_)
		playout_->setGain(gain);
}

bool AudioOutput::openLocked(uint32_t channels, uint32_t rate, bool offline, std::string *error)
{
	closeLocked();
	if(channels == 0 || channels > ring_.maxChannels() || rate == 0)
	{
		if(error)
			*error = "unsupported audio format";
		return false;
	}

	// closeLocked() left the producer blocked and the unit stopped: neither side runs.
	ring_.setChannels(channels);
	ring_.reset();
	PlayoutConfig pc = config_.playout;
	pc.sample_rate = (double)rate;
	playout_.reset(new Playout(ring_, pc));
	playout_->setGain(gain_);
	channels_ = channels;
	rate_ = rate;
	offline_ = offline;

	if(!offline)
	{
		AudioComponentDescription desc = {};
		desc.componentType = kAudioUnitType_Output;
		desc.componentSubType = kAudioUnitSubType_HALOutput;
		desc.componentManufacturer = kAudioUnitManufacturer_Apple;
		AudioComponent component = AudioComponentFindNext(nullptr, &desc);
		if(!component)
		{
			if(error)
				*error = "no HAL output audio unit";
			return false;
		}
		OSStatus status = AudioComponentInstanceNew(component, &unit_);
		if(status != noErr)
		{
			unit_ = nullptr;
			if(error)
				*error = statusString("AudioComponentInstanceNew", status);
			return false;
		}
		if(!configureUnitLocked(error))
		{
			AudioComponentInstanceDispose(unit_);
			unit_ = nullptr;
			return false;
		}
		if(config_.follow_default_device && config_.device == kAudioObjectUnknown && !system_listener_installed_)
		{
			if(AudioObjectAddPropertyListener(kAudioObjectSystemObject, &kDefaultOutputAddress, propertyListener, listener_ctx_) == noErr)
				system_listener_installed_ = true;
		}
	}

	unblockProducer();
	return true;
}

AudioObjectID AudioOutput::resolveDeviceLocked() const
{
	if(config_.device != kAudioObjectUnknown)
		return config_.device;
	AudioObjectID device = kAudioObjectUnknown;
	getProperty(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, device);
	return device;
}

bool AudioOutput::configureUnitLocked(std::string *error)
{
	OSStatus status;
	UInt32 enable = 1, disable = 0;
	// Output only. Element 0 is the output element, element 1 the input element.
	status = AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &enable, sizeof(enable));
	if(status != noErr)
	{
		if(error)
			*error = statusString("EnableIO(output)", status);
		return false;
	}
	AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &disable, sizeof(disable));

	AudioObjectID device = resolveDeviceLocked();
	if(device == kAudioObjectUnknown)
	{
		if(error)
			*error = "no output device";
		return false;
	}
	status = AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &device, sizeof(device));
	if(status != noErr)
	{
		if(error)
			*error = statusString("CurrentDevice", status);
		return false;
	}

	// What we hand the unit: interleaved float32 at the stream's rate. The AUHAL
	// converts to the device's rate and layout if they differ.
	AudioStreamBasicDescription asbd = {};
	asbd.mSampleRate = (Float64)rate_;
	asbd.mFormatID = kAudioFormatLinearPCM;
	asbd.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
	asbd.mFramesPerPacket = 1;
	asbd.mChannelsPerFrame = channels_;
	asbd.mBitsPerChannel = 32;
	asbd.mBytesPerFrame = 4 * channels_;
	asbd.mBytesPerPacket = asbd.mBytesPerFrame;
	status = AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &asbd, sizeof(asbd));
	if(status != noErr)
	{
		if(error)
			*error = statusString("StreamFormat", status);
		return false;
	}

	AURenderCallbackStruct cb = { renderCallback, this };
	status = AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof(cb));
	if(status != noErr)
	{
		if(error)
			*error = statusString("SetRenderCallback", status);
		return false;
	}

	if(config_.preferred_io_frames)
	{
		AudioValueRange range = {};
		UInt32 frames = config_.preferred_io_frames;
		if(getProperty(device, kAudioDevicePropertyBufferFrameSizeRange, kAudioObjectPropertyScopeGlobal, range))
			frames = (UInt32)std::min<Float64>(std::max<Float64>(frames, range.mMinimum), range.mMaximum);
		// Not fatal: a device that refuses keeps its own IO size.
		AudioUnitSetProperty(unit_, kAudioDevicePropertyBufferFrameSize, kAudioUnitScope_Global, 0, &frames, sizeof(frames));
	}

	status = AudioUnitInitialize(unit_);
	if(status != noErr)
	{
		if(error)
			*error = statusString("AudioUnitInitialize", status);
		return false;
	}
	bindDeviceListenersLocked(device);
	refreshLatencyLocked();
	return true;
}

void AudioOutput::bindDeviceListenersLocked(AudioObjectID device)
{
	unbindDeviceListenersLocked();
	bound_device_ = device;
	for(const auto &address : kDeviceRebindAddresses)
		AudioObjectAddPropertyListener(device, &address, propertyListener, listener_ctx_);
	for(const auto &address : kDeviceLatencyAddresses)
		AudioObjectAddPropertyListener(device, &address, propertyListener, listener_ctx_);
	getProperty(device, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, bound_device_rate_);
}

void AudioOutput::unbindDeviceListenersLocked()
{
	if(bound_device_ == kAudioObjectUnknown)
		return;
	for(const auto &address : kDeviceRebindAddresses)
		AudioObjectRemovePropertyListener(bound_device_, &address, propertyListener, listener_ctx_);
	for(const auto &address : kDeviceLatencyAddresses)
		AudioObjectRemovePropertyListener(bound_device_, &address, propertyListener, listener_ctx_);
	bound_device_ = kAudioObjectUnknown;
	bound_device_rate_ = 0.0;
}

void AudioOutput::refreshLatencyLocked()
{
	AudioDeviceLatency l;
	l.device = bound_device_;
	if(bound_device_ != kAudioObjectUnknown)
	{
		Float64 rate = 0.0;
		if(getProperty(bound_device_, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, rate))
			l.device_rate = rate;
		UInt32 v = 0;
		if(getProperty(bound_device_, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, v))
			l.io_buffer_frames = v;
		if(getProperty(bound_device_, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput, v))
			l.safety_offset_frames = v;
		if(getProperty(bound_device_, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput, v))
			l.device_latency_frames = v;
		AudioObjectPropertyAddress streams_address = { kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
		UInt32 size = 0;
		if(AudioObjectGetPropertyDataSize(bound_device_, &streams_address, 0, nullptr, &size) == noErr && size >= sizeof(AudioStreamID))
		{
			std::vector<AudioStreamID> streams(size / sizeof(AudioStreamID));
			if(AudioObjectGetPropertyData(bound_device_, &streams_address, 0, nullptr, &size, streams.data()) == noErr
				&& getProperty(streams[0], kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal, v))
				l.stream_latency_frames = v;
		}
	}
	if(unit_)
	{
		Float64 seconds = 0.0;
		UInt32 size = sizeof(seconds);
		if(AudioUnitGetProperty(unit_, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0, &seconds, &size) == noErr)
			l.unit_latency_s = seconds;
	}
	if(l.device_rate > 0.0)
		l.total_ms = (double)(l.io_buffer_frames + l.safety_offset_frames + l.device_latency_frames + l.stream_latency_frames)
			* 1000.0 / l.device_rate;
	l.total_ms += l.unit_latency_s * 1000.0;
	latency_ = l;
}

bool AudioOutput::startLocked(std::string *error)
{
	if(offline_)
	{
		running_ = true;
		return true;
	}
	if(!unit_)
	{
		if(error)
			*error = "audio output is not open";
		return false;
	}
	if(running_)
		return true;
	OSStatus status = AudioOutputUnitStart(unit_);
	if(status != noErr)
	{
		if(error)
			*error = statusString("AudioOutputUnitStart", status);
		return false;
	}
	running_ = true;
	return true;
}

void AudioOutput::stopLocked()
{
	if(unit_ && running_)
		AudioOutputUnitStop(unit_); // synchronous: no render callback runs after it returns
	running_ = false;
}

void AudioOutput::closeLocked()
{
	blockProducer();
	stopLocked();
	if(system_listener_installed_)
	{
		AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &kDefaultOutputAddress, propertyListener, listener_ctx_);
		system_listener_installed_ = false;
	}
	unbindDeviceListenersLocked();
	if(unit_)
	{
		AudioUnitUninitialize(unit_);
		AudioComponentInstanceDispose(unit_);
		unit_ = nullptr;
	}
	offline_ = false;
	channels_ = 0;
	rate_ = 0;
	// playout_ is kept until the next open so stats() still reports the last session.
}

void AudioOutput::onAudioFormat(uint32_t channels, uint32_t rate)
{
	// Library thread: the same thread that calls onPcm(), so onPcm() is not running now.
	format_changes_.fetch_add(1, std::memory_order_relaxed);
	std::lock_guard<std::mutex> lock(control_mutex_);
	const bool is_open = unit_ || offline_;
	if(is_open && channels == channels_ && rate == rate_)
	{
		playout_->requestFlush(); // a new stream: nothing the previous one left is wanted
	}
	else if(!openLocked(channels, rate, offline_, nullptr))
	{
		return;
	}
	if(config_.start_on_format)
		startLocked(nullptr);
}

void AudioOutput::onPcm(const int16_t *interleaved, size_t frames)
{
	producer_active_.fetch_add(1, std::memory_order_seq_cst);
	if(accepting_.load(std::memory_order_seq_cst))
	{
		pcm_frames_in_.fetch_add(frames, std::memory_order_relaxed);
		if(!ring_.writeS16(interleaved, frames))
		{
			overflow_events_.fetch_add(1, std::memory_order_relaxed);
			overflow_frames_.fetch_add(frames, std::memory_order_relaxed);
		}
	}
	producer_active_.fetch_sub(1, std::memory_order_seq_cst);
}

OSStatus AudioOutput::renderCallback(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *timestamp,
		UInt32 bus, UInt32 frames, AudioBufferList *data)
{
	(void)timestamp;
	(void)bus;
	AudioOutput *self = static_cast<AudioOutput *>(ref);
	if(!data || data->mNumberBuffers == 0)
		return noErr;
	AudioBuffer &buffer = data->mBuffers[0];
	const unsigned ch = self->ring_.channels();
	const bool layout_ok = self->playout_ && data->mNumberBuffers == 1 && buffer.mNumberChannels == ch
		&& buffer.mData && buffer.mDataByteSize >= frames * ch * sizeof(float);
	if(!layout_ok)
	{
		for(UInt32 i = 0; i < data->mNumberBuffers; i++)
			if(data->mBuffers[i].mData)
				memset(data->mBuffers[i].mData, 0, data->mBuffers[i].mDataByteSize);
		self->render_errors_.fetch_add(1, std::memory_order_relaxed);
		if(flags)
			*flags |= kAudioUnitRenderAction_OutputIsSilence;
		return noErr;
	}
	if(!self->playout_->render(static_cast<float *>(buffer.mData), frames) && flags)
		*flags |= kAudioUnitRenderAction_OutputIsSilence;
	return noErr;
}

OSStatus AudioOutput::propertyListener(AudioObjectID object, UInt32 count, const AudioObjectPropertyAddress *addresses, void *client)
{
	(void)object;
	ListenerContext *ctx = static_cast<ListenerContext *>(client);
	bool rebind = false;
	bool latency = false;
	for(UInt32 i = 0; i < count; i++)
	{
		switch(addresses[i].mSelector)
		{
			case kAudioHardwarePropertyDefaultOutputDevice:
			case kAudioDevicePropertyDeviceIsAlive:
			case kAudioDevicePropertyNominalSampleRate:
				rebind = true;
				break;
			default:
				latency = true;
				break;
		}
	}
	if(rebind)
		dispatch_async_f(ctx->queue, ctx, rebindThunk);
	else if(latency)
		dispatch_async_f(ctx->queue, ctx, latencyThunk);
	return noErr;
}

void AudioOutput::rebindThunk(void *ctx_ptr)
{
	ListenerContext *ctx = static_cast<ListenerContext *>(ctx_ptr);
	std::lock_guard<std::mutex> lock(ctx->mutex);
	if(ctx->owner)
		ctx->owner->rebindOnQueue();
}

void AudioOutput::latencyThunk(void *ctx_ptr)
{
	ListenerContext *ctx = static_cast<ListenerContext *>(ctx_ptr);
	std::lock_guard<std::mutex> lock(ctx->mutex);
	if(ctx->owner)
	{
		std::lock_guard<std::mutex> control(ctx->owner->control_mutex_);
		ctx->owner->refreshLatencyLocked();
	}
}

void AudioOutput::rebindOnQueue()
{
	std::lock_guard<std::mutex> lock(control_mutex_);
	if(!unit_ || offline_)
		return;
	const AudioObjectID device = resolveDeviceLocked();
	UInt32 alive = 1;
	Float64 rate = 0.0;
	getProperty(bound_device_, kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, alive);
	getProperty(bound_device_, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, rate);
	if(device == bound_device_ && alive && rate == bound_device_rate_)
		return; // a duplicate notification; nothing changed for us

	const bool was_running = running_;
	stopLocked();
	unbindDeviceListenersLocked();
	AudioUnitUninitialize(unit_);
	device_changes_.fetch_add(1, std::memory_order_relaxed);
	if(!configureUnitLocked(nullptr))
		return; // stays stopped; the next default-device change tries again
	// A new device is a new clock: forget the drift estimate and start from fresh audio.
	playout_->reset(false);
	playout_->requestFlush();
	if(was_running)
		startLocked(nullptr);
}

AudioOutputStats AudioOutput::stats() const
{
	AudioOutputStats s;
	{
		std::lock_guard<std::mutex> lock(control_mutex_);
		if(playout_)
			s.playout = playout_->stats();
		s.channels = channels_;
		s.rate = rate_;
		s.open = unit_ != nullptr || offline_;
		s.running = running_;
		s.latency = latency_;
	}
	s.pcm_frames_in = pcm_frames_in_.load(std::memory_order_relaxed);
	s.overflow_events = overflow_events_.load(std::memory_order_relaxed);
	s.overflow_frames = overflow_frames_.load(std::memory_order_relaxed);
	s.format_changes = format_changes_.load(std::memory_order_relaxed);
	s.device_changes = device_changes_.load(std::memory_order_relaxed);
	s.render_errors = render_errors_.load(std::memory_order_relaxed);
	return s;
}

} // namespace plk
