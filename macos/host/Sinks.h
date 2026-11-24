// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// The two data paths out of a SessionHost. Both are called synchronously on the
// library's "Chiaki Takion" receive thread, the thread that reassembled the data:
// nothing is queued or copied between the library and the sink.

#ifndef PLK_HOST_SINKS_H
#define PLK_HOST_SINKS_H

#include <cstddef>
#include <cstdint>

namespace plk {

/**
 * Receives the video stream exactly as chiaki's ChiakiVideoSampleCallback delivers it.
 *
 * Thread: the library's "Chiaki Takion" thread. Return quickly: this thread also
 * receives every other packet of the session (audio, haptics, control).
 */
class VideoSink
{
public:
	virtual ~VideoSink() = default;

	/**
	 * One Annex-B access unit, or a codec configuration sample (VPS/SPS/PPS for H.265,
	 * SPS/PPS for H.264) sent when the stream starts or its profile changes.
	 *
	 * @param buf valid only for the duration of the call. The library guarantees
	 *        CHIAKI_VIDEO_BUFFER_PADDING_SIZE writable bytes after buf_size.
	 * @param frame_index the 16-bit Takion frame number; wraps every 65536 frames.
	 * @param frame_ready_time_us chiaki_time_now_monotonic_us() (CLOCK_MONOTONIC) at
	 *        which the frame finished reassembly; 0 for a configuration sample.
	 * @param frames_lost frames lost since the previous delivered sample.
	 * @param recovered the library re-pointed the frame's reference to an older frame
	 *        it still had, because the one it referenced was lost.
	 * @return false if the sample could not be decoded. The library then does not use
	 *         this frame as a reference and reports the frame corrupt to the console,
	 *         which makes the console send a recovery frame. To ask for an IDR
	 *         explicitly, call SessionHost::requestKeyframe().
	 */
	virtual bool onVideoSample(uint8_t *buf, size_t buf_size, uint16_t frame_index,
			uint64_t frame_ready_time_us, int32_t frames_lost, bool recovered) = 0;
};

/**
 * Receives decoded game audio from the library's Opus decoder.
 *
 * Thread: the library's "Chiaki Takion" thread, for both calls.
 */
class PcmSink
{
public:
	virtual ~PcmSink() = default;

	/** The stream's audio format. A PS5 sends 2 channels at 48000 Hz in 480-frame (10 ms) packets. */
	virtual void onAudioFormat(uint32_t channels, uint32_t rate) = 0;

	/** Interleaved signed 16-bit PCM, `frames` frames of `channels` samples. Valid only during the call. */
	virtual void onPcm(const int16_t *interleaved, size_t frames) = 0;
};

} // namespace plk

#endif // PLK_HOST_SINKS_H
