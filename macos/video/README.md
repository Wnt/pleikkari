# macos/video — native video pipeline

Compressed access units in, photons out: VideoToolbox hardware decode →
CVMetalTextureCache (zero copy) → one Metal pass (YCbCr→RGB, scale, letterbox)
→ `CAMetalLayer`. No Qt, FFmpeg, libplacebo or SDL. Standalone CMake project;
nothing else in the repository depends on it yet.

## Build

```sh
cmake -S macos/video -B build/native-video -G Ninja
ninja -C build/native-video          # libplkvideo.a, plk-video-replay, plk-video-tests
ctest --test-dir build/native-video  # bitstream + dump-format unit tests
```

The CMake file selects the Xcode SDK with `xcrun --sdk macosx` itself (the
default `xcrun` lookup can land on a Command Line Tools SDK that does not
work). Shaders are embedded as source and compiled at runtime, so the Metal
offline toolchain is not needed. Deployment target: macOS 14.

## API (`include/plk/VideoPipeline.h`)

```cpp
auto p = plk::video::VideoPipeline::create((__bridge void *)metalLayer, cfg,
        [](const plk::video::Error &e) { if(e.requestIdr) /* ask the console for an IDR */; });
p->submitConfig(header, header_size);               // chiaki config sample (VPS/SPS/PPS)
p->submitFrame(buf, size, frame_index, frame_ready_us, recovered, frames_lost);
p->setPacingMode(PacingMode::Immediate);            // runtime switch
p->setScaler(Scaler::MetalFX);                      // runtime switch
p->setDrawableSize(w, h);                           // on every view resize
plk::video::Stats s = p->stats();                   // counters + p50/p95/p99/max
plk::video::Stats c = p->counters();                // the same without the percentiles: cheap, for a caller that asks often
cfg.onFrame = [](const plk::video::FrameTiming &t) { /* one call per rendered frame */ };
```

`PipelineConfig::onFrame` reports every rendered frame's timeline (submit, decoded,
render start, commit, GPU start/end, presented) on a Metal callback thread once its
presentation time is known; it is never called after the destructor returns.

**Insets and overlay.** `setContentInsets(left, top, right, bottom)` keeps the picture
out of the edges of the target that something else covers (the app's panels): it is
fitted, with its aspect ratio, into what is left, and the rest stays black. Insets that
would leave less than 64 pixels either way are not used. `Stats::pictureWidth` and
`pictureHeight` say what the picture was fitted into.

`setOverlay(OverlayImage)` gives the pipeline a small picture (BGRA8, premultiplied
alpha, sRGB, top row first) to draw over the frame at a place counted from the picture's
top left corner; a null `bgra` takes it away. It is a second draw in the frame's last
render pass (the scaler's, or MetalFX's copy to the target), with blending, so it adds
no pass and no view: a fullscreen picture still goes straight to the display. The
texture is made on the caller's thread and handed over with the next frame, so it can be
replaced several times a second (the app's frame rate overlay does, at 10 Hz). On an SDR
target its code values are written as they are. On an HDR target its white is 100 nits
on a PQ target and 1.0 on a linear one, which is the display's SDR white in both, and
its colours are taken from BT.709 to the target's primaries (`fs_overlay` in
`Shaders.metal`; `tests/test_overlay.mm`, and `tests/test_enhancer_hdr.mm` for the HDR
levels).

**Enhancer** (`include/plk/FrameEnhancer.h`). `setEnhancer()` gives the pipeline something
that turns one R'G'B' picture into another, slowly: the neural rendering engine of
`macos/nr`, or a test's stand-in. `setEnhanceConfig()` says how (`EnhanceConfig`: on or
off, the height of the copy it is given, the strength, the comparison split). While the
enhancer is idle the pipeline renders a copy of the newest decoded frame at that height
in a pass of its own and offers it; the stream's frames never wait for the answer. A
result is kept together with the frame it was made from, and the fragment shader adds
their difference to every following frame, faded per pixel where the frame has moved
away from that one, until the next result or `maxAgeMs`.

The copy of an SDR picture is BGRA8, the stream's own code values. Of an HDR10 picture
the enhancer is given an SDR rendition in half float (`sdr_rendition()` in
`Shaders.metal`): BT.709 primaries, SDR white at 203 nits, linear light as it is up to
0.6 of that and on a logarithmic shoulder above it that reaches 1.0 at 1000 nits, 2.2
gamma. The difference is then made to the frame's rendition, and the pixel keeps the
ratio it had to its rendition: below the knee that is the SDR mix exactly, and a
highlight changes by the proportion its rendition changed by. The shoulder is never
inverted, so a small change by the enhancer stays a small change at any brightness, and
a pixel the enhancer left alone keeps its code values to the bit. All of it happens on
the stream's code values, before the brightness, the tone mapping and the scaler.

`PipelineConfig::onEnhance` reports every result (size, GPU time of the
input pass, the enhancer's own time, the round trip), `FrameTiming::enhanceAgeMs` the age
of the result a frame carried, and `Stats` the counters. `plk-video-enhancer-test` runs
the whole path offscreen with a stand-in enhancer and checks the pixels;
`plk-video-enhancer-hdr-test` does the same with an HEVC Main 10 stream in PQ, against
the rendition and the mix worked out a second time in the test.

`submitFrame` maps directly onto chiaki's `ChiakiVideoSampleCallback` and
returns false when the frame was not queued (no session, waiting for an IDR,
rejected); chiaki then withholds the frame as a reference and reports the gap
as a corrupt frame. Asynchronous decode errors (and session rebuilds) arrive
through the error callback with `requestIdr` set, rate-limited to one per
100 ms: forward those to `chiaki_session_request_idr()`, from another thread
or by scheduling it, not inline. A host app must keep App Nap away while
streaming (`NSActivityLatencyCritical`), or timers and threads get throttled.

Pacing modes: `Immediate` (default; present on decode, display sync off),
`ImmediateVsync` (display sync on, tear-free in direct-to-display fullscreen)
and `DisplayLink` (`CAMetalDisplayLink`, newest frame per refresh). In a
window on macOS 27 the display link presents ~41.6 ms after its callback, so
it is the slowest by far; see the comments in `VideoPipeline.mm`.

Clock: everything is `mach_absolute_time` (`CACurrentMediaTime` base).
chiaki's `frame_ready_us` is `CLOCK_MONOTONIC`, which on macOS includes sleep
time; the pipeline translates it at submit.

## plk-video-replay

```sh
plk-video-replay [flags] clip.h265|clip.h264|stream.plkv
```

Accessory app (no Dock icon, never takes focus); borderless, click-through
640x360 window in the bottom-left corner. Runs are capped at 15 s unless
`--allow-long`. Useful flags (`--help` lists all):

| flag | meaning |
|---|---|
| `--pacing immediate\|immediate-vsync\|displaylink` | pacing mode |
| `--scaler bilinear\|bicubic\|lanczos\|metalfx` | scaler |
| `--phases P:S,P:S --phase-seconds N` | back-to-back phases, stats per phase |
| `--fps N`, `--cadence recorded\|fixed`, `--loop`, `--frames N`, `--duration S` | playback |
| `--offscreen WxH` | render offscreen at panel size (GPU timing, nothing on screen) |
| `--readback X,Y` | offscreen: print the rendered pixel (colour verification) |
| `--hdr-output pq\|linear`, `--compressed-output`, `--sync-decode`, `--drawables 2\|3` | A/B switches |
| `--invalidate-at N` | invalidate the decoder session (recovery test) |
| `--write-dump OUT` | convert a raw stream to PLKVDMP1 with the measured PS5 arrival jitter |
| `--json` | one JSON line per phase |

Dumps (`PLKVDMP1`, format in `tools/replay/Dump.h`) replay at their recorded
`frame_ready_us` cadence by default. Every run prints the feeder's wake-up
lateness: if it is more than ~1 ms the machine was busy and the latency
numbers of that run are suspect.

The pipeline shows frames in the order they are decoded, which is the order a console
sends them in. A clip with B-frames (x265's default) therefore plays with its pictures
out of order: anything that moves jumps back and forth. Encode test clips without them.

Test clips (1080p60, no B-frames, periodic IDR) can be made with ffmpeg, e.g.
`ffmpeg -f lavfi -i testsrc2=size=1920x1080:rate=60 -t 10 -pix_fmt yuv420p10le -c:v libx265
-x265-params bframes=0:keyint=120:colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:hdr10=1
-f hevc hdr.h265`; concatenating two raw streams of different sizes exercises
the resolution-change path. Keep clips and dumps out of git.
