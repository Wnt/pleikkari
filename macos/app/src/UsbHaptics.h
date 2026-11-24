// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// DualSense haptics over USB. A DualSense on USB is also a 4-channel USB audio device
// whose channels 3 and 4 drive the left and right actuators. The console's haptics stream
// (stereo int16 at 3 kHz) goes there:
//
//   3 kHz stereo -> gain -> linear upsampling x16 -> 48 kHz, 4 channels (1, 2 silent;
//   3 = left, 4 = right) -> plk::AudioOutput bound to the DualSense audio device
//
// as the Qt app's StreamSession::ConnectHaptics() / PushHapticsFrame() do it with SDL
// audio. As there, the device must be set to 4 channels ("quadraphonic") in Audio MIDI
// Setup; with 2 channels the actuator channels do not exist and nothing is felt.
//
// push() runs on the lib's Takion thread (the single producer of the AudioOutput ring);
// open()/close() on any other thread.

#ifndef PLK_APP_USBHAPTICS_H
#define PLK_APP_USBHAPTICS_H

#include <CoreAudio/CoreAudio.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace plk {
class AudioOutput;
}

namespace plk::app {

struct AudioDeviceInfo
{
	AudioObjectID id = kAudioObjectUnknown;
	std::string name;
	uint32_t output_channels = 0;
};

/** Output devices whose name contains "DualSense" or "Wireless Controller". */
std::vector<AudioDeviceInfo> findDualSenseAudioDevices();

/**
 * 3 kHz interleaved stereo int16 -> 48 kHz 4-channel int16 (channels 3/4), linear
 * interpolation from the previous call's last sample. `prev` holds that sample (L, R).
 * Returns the number of output frames (16 per input frame).
 */
size_t upsampleHaptics(const int16_t *in, size_t in_frames, float gain, int16_t prev[2], int16_t *out);

class UsbHaptics
{
public:
	UsbHaptics();
	~UsbHaptics();

	/** Open and start the output on `device`; false with *error. */
	bool open(const AudioDeviceInfo &device, std::string *error);
	void close();
	bool isOpen() const { return output_ != nullptr; }
	void push(const int16_t *stereo, size_t samples, float gain);
	std::string statsLine() const;

private:
	std::unique_ptr<AudioOutput> output_;
	AudioDeviceInfo device_;
	int16_t prev_[2] = {0, 0};
	std::vector<int16_t> buf_;
};

} // namespace plk::app

#endif // PLK_APP_USBHAPTICS_H
