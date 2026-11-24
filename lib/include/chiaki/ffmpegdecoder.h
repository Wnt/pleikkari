#ifndef CHIAKI_FFMPEG_DECODER_H
#define CHIAKI_FFMPEG_DECODER_H

#include <chiaki/config.h>
#include <chiaki/log.h>
#include <chiaki/seqnum.h>
#include <chiaki/thread.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <libavcodec/avcodec.h>
#include <libavutil/rational.h>

typedef struct chiaki_ffmpeg_decoder_t ChiakiFfmpegDecoder;

typedef struct chiaki_ffmpeg_frame_t ChiakiFfmpegFrame;

typedef void (*ChiakiFfmpegFrameAvailable)(ChiakiFfmpegDecoder *decover, void *user);

struct chiaki_ffmpeg_frame_t
{
	AVFrame *frame;
	double pts;
	double duration;
	bool recovered;
	/**
	 * Telemetry for the sample this frame was decoded from, all 0 when it is not known:
	 * the Takion frame serial and the time the receiver completed the frame, as the sample
	 * callback got them, and the monotonic times at which that callback was entered and at
	 * which avcodec_send_packet() returned.
	 */
	ChiakiSeqNum16 frame_index;
	uint64_t frame_ready_time_us;
	uint64_t decode_begin_us;
	uint64_t decode_end_us;
};

#define CHIAKI_FFMPEG_SAMPLE_TIMING_COUNT 16

typedef struct chiaki_ffmpeg_sample_timing_t
{
	int64_t pts;
	ChiakiSeqNum16 frame_index;
	uint64_t frame_ready_time_us;
	uint64_t decode_begin_us;
	uint64_t decode_end_us;
} ChiakiFfmpegSampleTiming;

struct chiaki_ffmpeg_decoder_t
{
	ChiakiLog *log;
	ChiakiMutex mutex;
	const AVCodec *av_codec;
	AVCodecContext *codec_context;
	enum AVPixelFormat hw_pix_fmt;
	AVBufferRef *hw_device_ctx;
	bool hdr_enabled;
	ChiakiMutex cb_mutex;
	ChiakiFfmpegFrameAvailable frame_available_cb;
	void *frame_available_cb_user;
	int32_t frames_lost;
	bool frame_recovered;
	int32_t session_bitrate_kbps;
	int64_t synthetic_packet_pts;
	AVRational synthetic_time_base;
	AVRational synthetic_framerate;
	double synthetic_frame_duration_us;
	double synthetic_candidate_duration_us;
	uint64_t synthetic_last_sample_time_us;
	uint8_t synthetic_candidate_count;
	// Timing of the last samples by packet pts, so a pulled frame can report its own. Under mutex.
	ChiakiFfmpegSampleTiming sample_timing[CHIAKI_FFMPEG_SAMPLE_TIMING_COUNT];
	unsigned int sample_timing_next;
};

CHIAKI_EXPORT ChiakiErrorCode chiaki_ffmpeg_decoder_init(ChiakiFfmpegDecoder *decoder, ChiakiLog *log,
		ChiakiCodec codec, unsigned int max_fps, const char *hw_decoder_name, AVBufferRef *hw_device_ctx,
		ChiakiFfmpegFrameAvailable frame_available_cb, void *frame_available_cb_user);
CHIAKI_EXPORT void chiaki_ffmpeg_decoder_fini(ChiakiFfmpegDecoder *decoder);
CHIAKI_EXPORT bool chiaki_ffmpeg_decoder_video_sample_cb(uint8_t *buf, size_t buf_size,
		ChiakiSeqNum16 frame_index, uint64_t frame_ready_time_us, int32_t frames_lost,
		bool frame_recovered, void *user);
CHIAKI_EXPORT ChiakiFfmpegFrame chiaki_ffmpeg_decoder_pull_frame(ChiakiFfmpegDecoder *decoder, int32_t *frames_lost);
CHIAKI_EXPORT enum AVPixelFormat chiaki_ffmpeg_decoder_get_pixel_format(ChiakiFfmpegDecoder *decoder);

/**
 * Compute the wall-clock pts (seconds) and frame duration (seconds) from raw
 * AVFrame timestamp fields and codec context timing metadata.  Exposed for
 * unit testing; normally called internally by chiaki_ffmpeg_decoder_pull_frame.
 *
 * Fallback chain for time_base: pkt_timebase → ctx_timebase → {1,1000000}.
 * Fallback chain for pts:       best_effort_timestamp → pts → 0.
 * Fallback for fps:             framerate or 60.0.
 */
CHIAKI_EXPORT void chiaki_ffmpeg_frame_get_timing(
	AVFrame *frame,
	AVRational pkt_timebase,
	AVRational ctx_timebase,
	AVRational framerate,
	double *out_pts,
	double *out_duration);

#ifdef __cplusplus
}
#endif

#endif // CHIAKI_FFMPEG_DECODER_H
