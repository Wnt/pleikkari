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

}  // namespace plk::video
