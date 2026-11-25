// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// The frame rate overlay: frames on glass per second, and the last present intervals as a
// line with the newest at the right, over the picture's top left corner.
//
//   FPS  60
//   Frametime      16.7 ms
//   ___/\_____________/\__     against the stream's frame interval (dashed)
//
// It is drawn here into a small bitmap, about ten times a second, and the video pipeline
// puts that over the picture in the pass that draws the frame (VideoPipeline::setOverlay).
// So it costs the stream no pass of its own, a fullscreen picture still goes straight to
// the display with it, and a headless run has it in the frames it captures.
#ifndef PLK_APP_FRAMEHUD_H
#define PLK_APP_FRAMEHUD_H

#include "FrameAnalysis.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace plk::app {

/** BGRA, alpha premultiplied, sRGB, the top row first: what video::OverlayImage takes. */
struct HudImage
{
	std::vector<uint8_t> bgra;
	int width = 0, height = 0;
	size_t bytes_per_row = 0;
};

// Its size, and how far in from the picture's corner it sits, in points.
constexpr int kHudWidth = 164, kHudHeight = 78, kHudMargin = 12;

/** scale: pixels per point, 1 to 4 (2 on a Retina display). */
HudImage renderFrameHud(const PaceSnapshot &pace, int scale);

/**
 * How the frame rate stands against the stream's own, for the colour of the number:
 * 0 as it should be (within 5 %), 1 below it, 2 less than four fifths of it; 0 as well
 * while the stream's rate is not known.
 */
int paceHealth(const PaceSnapshot &pace);

} // namespace plk::app

#endif // PLK_APP_FRAMEHUD_H
