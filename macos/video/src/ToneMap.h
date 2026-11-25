// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// HdrToneMap::App: how an HDR10 (PQ) picture is fitted to the display in the shader. Plain
// arithmetic, kept apart from the pipeline so it can be tested without a GPU.
//
// Values are "EDR" values: linear light with 1.0 = the display's SDR white, the unit of
// NSScreen's headroom.
//
//   brightness   HDR reference white, 203 nits (BT.2408), is shown as bright as SDR white,
//                times the brightness factor.
//   roll-off     when the picture's peak is above what the display shows now (its
//                headroom), light above a knee follows
//                    knee + (headroom - knee) * (1 - exp(-(x - knee) / (headroom - knee)))
//                which leaves the knee with slope 1 and never reaches the headroom. Applied
//                to the brightest of R, G, B; the other two keep their ratio to it.
//   knee         70 % of the headroom, or higher when the peak is only a little above the
//                headroom (2 * headroom - peak), so the curve fades into "nothing to do" as
//                the headroom grows past the peak and there is no step where it switches off.

#ifndef PLK_VIDEO_TONEMAP_H
#define PLK_VIDEO_TONEMAP_H

namespace plk::video {

constexpr float kHdrReferenceWhiteNits = 203.0f;
/** The mastering peak assumed for a stream that carries no metadata (a PS5's). */
constexpr float kDefaultMasteringPeakNits = 1000.0f;

struct AppToneMap
{
	float nitsToEdr = 0;  // EDR value of one nit
	float knee = 0;       // EDR; meaningless when headroom is 0
	float headroom = 0;   // EDR; 0: nothing is rolled off
};

/**
 * @param brightness       PipelineConfig::hdrBrightness
 * @param peakNits         the picture's mastering peak
 * @param displayHeadroom  what the display shows now in EDR; 0 if not known
 */
AppToneMap appToneMap(float brightness, float peakNits, float displayHeadroom);

/** The curve the shader applies to the brightest channel (headroom 0: x itself). */
float rollOff(float x, float knee, float headroom);

} // namespace plk::video

#endif // PLK_VIDEO_TONEMAP_H
