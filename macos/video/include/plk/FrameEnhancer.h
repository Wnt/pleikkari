// FrameEnhancer: something slow that improves a picture (neural rendering), next to a
// pipeline that must never wait for it.
//
// The pipeline offers the enhancer a small R'G'B' copy of a decoded frame whenever the
// enhancer is idle, and keeps presenting every frame the moment it is decoded. When a
// result comes back, the pipeline mixes its *change* (result minus the frame it was made
// from, the key frame) into the frames that follow, and only where a frame still looks
// like the key frame; see EnhanceConfig in VideoPipeline.h. So the enhancer costs GPU
// time but no frame ever waits for it, and a slower enhancer means an older change, not
// a later picture.
#pragma once

#include <CoreVideo/CoreVideo.h>

#include <cstdint>
#include <functional>
#include <string>

namespace plk::video {

class FrameEnhancer {
public:
	// `output` is nullptr when the enhancer failed, otherwise an IOSurface-backed buffer
	// of the input's size and pixel format, valid during the call (retain it to keep it).
	// `totalMs` is the enhancer's own time for the frame.
	using Done = std::function<void(CVPixelBufferRef output, double totalMs)>;

	virtual ~FrameEnhancer() = default;

	// Offers one frame, IOSurface-backed, non-linear R'G'B' of an SDR picture in 0...1:
	// kCVPixelFormatType_32BGRA with the code values of an SDR stream, or
	// kCVPixelFormatType_64RGBAHalf with the SDR rendition of an HDR stream's picture
	// (BT.709 primaries, 2.2 gamma). Must not block. Returns true when the frame was
	// taken; `done` is then called exactly once, from any thread. Returns false when the
	// enhancer is busy or cannot use the buffer; `done` is then dropped uncalled.
	// `reset`: this frame does not continue the previous one given (a new stream, a new
	// size, a pause in between).
	virtual bool submit(CVPixelBufferRef frame, uint64_t seq, bool reset, Done done) = 0;

	// One line for the statistics: which model, what precision.
	virtual std::string describe() const = 0;
};

// Makes the picture between two frames (frame generation). The pipeline gives it the two
// as whole R'G'B' pictures in the stream's own code values and shows what comes back
// between them.
class FrameInterpolator {
public:
	// One call for each generated frame, in order: `output` is an IOSurface-backed buffer
	// of the inputs' size and pixel format, valid during the call (retain it to keep it),
	// the `index`th of `count`. Or one call with nullptr and a count of 0: it failed.
	using Done = std::function<void(CVPixelBufferRef output, int index, int count, double totalMs)>;

	virtual ~FrameInterpolator() = default;

	// Two frames of one size and pixel format (kCVPixelFormatType_32BGRA, or
	// kCVPixelFormatType_64RGBAHalf for the code values of an HDR picture), IOSurface-backed,
	// the earlier one first; `count` frames evenly spaced between them are asked for (1:
	// the one halfway). Must not block. Returns true when they were taken; `done` is then
	// called as said above, from any thread. Returns false when it is busy or cannot use
	// the buffers; `done` is then dropped uncalled.
	virtual bool interpolate(CVPixelBufferRef previous, CVPixelBufferRef current, int count, uint64_t seq, Done done) = 0;

	virtual std::string describe() const = 0;
};

}  // namespace plk::video
