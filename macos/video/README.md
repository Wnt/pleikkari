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
cfg.onFrame = [](const plk::video::FrameTiming &t) { /* one call per rendered frame */ };
```

`PipelineConfig::onFrame` reports every rendered frame's timeline (submit, decoded,
render start, commit, GPU start/end, presented) on a Metal callback thread once its
presentation time is known; it is never called after the destructor returns.

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

Test clips (1080p60, no B-frames, periodic IDR) can be made with ffmpeg, e.g.
`ffmpeg -f lavfi -i testsrc2=size=1920x1080:rate=60 -t 10 -pix_fmt yuv420p10le -c:v libx265
-x265-params bframes=0:keyint=120:colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:hdr10=1
-f hevc hdr.h265`; concatenating two raw streams of different sizes exercises
the resolution-change path. Keep clips and dumps out of git.
