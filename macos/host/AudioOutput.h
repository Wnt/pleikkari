// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Game audio out through a CoreAudio output unit (AUHAL) on the default output device.
//
//   library thread (Opus PCM) --FrameRing (wait-free SPSC)--> CoreAudio IO thread (Playout)
//
// The producer converts to float and appends to the ring; nothing else happens on the
// library thread. The render callback runs Playout (adaptive margin, drift resampling,
// backlog cut, click-free underrun handling; see Playout.h). The device's IO buffer is
// asked to be small (128 frames by default, clamped to the device's range).
//
// Device changes: when the system default output changes, or the bound device dies or
// changes its nominal rate, the unit is rebound on a private serial queue and playout
// rebuffers from fresh audio.
//
// Compared with the Qt app (gui/src/streamsession.cpp): there the PCM goes through a
// mutex-guarded byte ring, a drain thread polling every 4 ms, SDL's queue topped up to
// 2 x audio_buffer_size (default 9600 bytes = 50 ms, so 100 ms) and SDL's device buffer
// of audio_buffer_size (50 ms), with no drift handling: a fast console clock grows the
// backlog until the 400 ms ring overflows, a slow one runs SDL dry. Here the ring holds
// about 9-11 ms on average (a 1-3 ms margin, half a 10 ms packet, one render; measured by
// the simulations in tests/AudioTests.cpp), then the IO buffer and what CoreAudio reports
// in AudioDeviceLatency.

#ifndef PLK_HOST_AUDIOOUTPUT_H
#define PLK_HOST_AUDIOOUTPUT_H

#include "FrameRing.h"
#include "Playout.h"
#include "Sinks.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <dispatch/dispatch.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace plk {

struct AudioOutputConfig
{
	PlayoutConfig playout;                       // sample_rate is taken from the stream format
	uint32_t preferred_io_frames = 128;          // device IO buffer to ask for; 0 = leave the device's
	bool follow_default_device = true;           // rebind when the system default output changes
	AudioObjectID device = kAudioObjectUnknown;  // a specific device; unknown = the default output
	size_t ring_frames = 16384;                  // ring capacity (341 ms at 48 kHz); a bound, not a target
	bool start_on_format = true;                 // onAudioFormat() opens (if needed) and starts the unit
};

/** What CoreAudio reports it adds after our ring, for the bound device. */
struct AudioDeviceLatency
{
	AudioObjectID device = kAudioObjectUnknown;
	double device_rate = 0.0;          // nominal rate of the device
	uint32_t io_buffer_frames = 0;     // kAudioDevicePropertyBufferFrameSize
	uint32_t safety_offset_frames = 0; // kAudioDevicePropertySafetyOffset (output)
	uint32_t device_latency_frames = 0;// kAudioDevicePropertyLatency (output)
	uint32_t stream_latency_frames = 0;// kAudioStreamPropertyLatency of the first output stream
	double unit_latency_s = 0.0;       // kAudioUnitProperty_Latency of the AUHAL (its rate converter)
	double total_ms = 0.0;             // all of the above, in milliseconds
};

struct AudioOutputStats
{
	PlayoutStats playout;
	uint64_t pcm_frames_in = 0;        // frames the library delivered
	uint64_t overflow_events = 0;      // deliveries dropped because the ring was full
	uint64_t overflow_frames = 0;
	uint64_t format_changes = 0;
	uint64_t device_changes = 0;
	uint64_t render_errors = 0;        // render calls with a buffer layout we did not ask for
	uint32_t channels = 0;
	uint32_t rate = 0;
	bool open = false;
	bool running = false;
	AudioDeviceLatency latency;
};

class AudioOutput : public PcmSink
{
public:
	explicit AudioOutput(const AudioOutputConfig &config = AudioOutputConfig());
	~AudioOutput() override;

	AudioOutput(const AudioOutput &) = delete;
	AudioOutput &operator=(const AudioOutput &) = delete;

	/** Create, bind and initialise the output unit for this format. Does not start it. */
	bool open(uint32_t channels, uint32_t rate, std::string *error = nullptr);

	/**
	 * Set up ring and playout for this format without any CoreAudio unit: an offline
	 * output whose renderCallback() the caller drives (tests, or another clock).
	 */
	bool openOffline(uint32_t channels, uint32_t rate);

	bool start(std::string *error = nullptr);
	void stop();
	void close();

	void setGain(float gain);
	AudioOutputStats stats() const;

	// PcmSink (library thread)
	void onAudioFormat(uint32_t channels, uint32_t rate) override;
	void onPcm(const int16_t *interleaved, size_t frames) override;

	/**
	 * The AURenderCallback installed on the unit. Public so an offline output can be
	 * driven with a fake AudioBufferList. Expects one interleaved float32 buffer.
	 */
	static OSStatus renderCallback(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *timestamp,
			UInt32 bus, UInt32 frames, AudioBufferList *data);

private:
	struct ListenerContext;

	bool openLocked(uint32_t channels, uint32_t rate, bool offline, std::string *error);
	bool startLocked(std::string *error);
	void stopLocked();
	void closeLocked();
	bool configureUnitLocked(std::string *error);
	AudioObjectID resolveDeviceLocked() const;
	void bindDeviceListenersLocked(AudioObjectID device);
	void unbindDeviceListenersLocked();
	void refreshLatencyLocked();
	void blockProducer();
	void unblockProducer();

	static OSStatus propertyListener(AudioObjectID object, UInt32 count, const AudioObjectPropertyAddress *addresses, void *client);
	static void rebindThunk(void *ctx);
	static void latencyThunk(void *ctx);
	void rebindOnQueue();

	const AudioOutputConfig config_;
	FrameRing ring_;
	std::unique_ptr<Playout> playout_;

	mutable std::mutex control_mutex_; // open/start/stop/close/rebind; never taken by onPcm or the render callback
	AudioUnit unit_ = nullptr;
	AudioObjectID bound_device_ = kAudioObjectUnknown;
	Float64 bound_device_rate_ = 0.0;
	uint32_t channels_ = 0;
	uint32_t rate_ = 0;
	bool offline_ = false;
	bool running_ = false;
	float gain_ = 1.0f;
	AudioDeviceLatency latency_;

	dispatch_queue_t queue_ = nullptr;
	ListenerContext *listener_ctx_ = nullptr; // intentionally leaked on destruction, see the destructor
	bool system_listener_installed_ = false;

	std::atomic<bool> accepting_{false};
	std::atomic<int> producer_active_{0};
	std::atomic<uint64_t> pcm_frames_in_{0};
	std::atomic<uint64_t> overflow_events_{0};
	std::atomic<uint64_t> overflow_frames_{0};
	std::atomic<uint64_t> format_changes_{0};
	std::atomic<uint64_t> device_changes_{0};
	std::atomic<uint64_t> render_errors_{0};
};

} // namespace plk

#endif // PLK_HOST_AUDIOOUTPUT_H
