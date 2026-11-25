# macos/nr — neural rendering of the stream

`libplknr.dylib` runs NVIDIA's DLSS 5 Neural Rendering network on Apple Silicon, through
[MLX-DLSS](https://github.com/iamwavecut/MLX-DLSS) (Apache-2.0, pinned by revision in
`Package.swift`). The approach is the one of
[Veyra-NRVideo](https://github.com/Likely7/Veyra-NRVideo): the network is given a finished
picture, here a decoded frame of the PS5's stream, and returns the same picture relit and
with more detail.

The network is far slower than the stream (44 ms a picture at 720p on an M1 Max, against
16.7 ms a frame), so it runs **beside** the stream and never in its path:

```
decode ──► scale + mix ──► display            every frame, as before
   │            ▲
   ▼            │ the change the network made to its frame, applied to the frames after it
 input pass ─► network                        when it is idle: the newest decoded frame
```

- The video pipeline (`macos/video`) offers the engine a copy of a decoded frame whenever
  the engine is idle. The engine never queues, so it always works on the newest frame it
  could take and nothing ever waits for it.
- A result is kept as the **change** it made (result minus the frame it was made from).
  The frames that follow get that change added in the same shader pass that scales them.
  Where a frame differs from the one the result was made from (something moved), the
  change is faded out per pixel; a result older than 400 ms is dropped.
- The stream's own frames are decoded, rendered and presented exactly as without the
  engine. What the engine costs them is contention for the GPU; see the numbers below.

The network knows SDR pictures. An SDR stream's frames go to it as they are. Of an HDR
stream's frame it is given an **SDR rendition** (SDR white at 203 nits, the highlights
packed above it), and the change it makes to the rendition is made to the HDR frame in
proportion: what is under about 120 nits changes exactly as it would in an SDR stream,
and a highlight changes by the same share as its rendition did and stays a highlight.
[`macos/video/README.md`](../video/README.md) has the curve.

## Build

```sh
macos/nr/build.sh                       # into <workspace>/build/native-app/nr
macos/app/build.sh                      # picks the engine up from its dist directory
```

Needs Xcode 26 or later (Swift 6.2) with its Metal Toolchain component
(`xcodebuild -downloadComponent MetalToolchain`), and the network the first time: SwiftPM
fetches MLX-DLSS and mlx-swift. `build.sh` leaves what others use in `<scratch>/dist`:

| file | |
|---|---|
| `libplknr.dylib` | the engine; `macos/app/build.sh` copies it to `Pleikkari.app/Contents/Frameworks` |
| `mlx-swift_Cmlx.bundle` | MLX's Metal kernels; copied to `Pleikkari.app/Contents/Resources`, where MLX looks |
| `mlx.metallib` | the same file under the name MLX looks for beside a binary outside a bundle |
| `plk-nr-bench` | the engine without the app |

The app loads the library at run time (`dlopen`), so the app's CMake build needs neither
Swift nor MLX, and an app without the library runs as before and says so in one log line.
`macos/app/build.sh` takes another dist directory from `NR_DIST`.

## Models

No model is in the repository, in the app bundle or downloaded by anything here. The
weights are NVIDIA's; they come out of an `nvngx_dlssnr.dll` that you supply (it ships
with software that uses DLSS 5 Neural Rendering; Veyra-NRVideo's release archive carries
one):

```sh
macos/nr/tools/prepare-model.sh /path/to/nvngx_dlssnr.dll
```

writes the packages the app looks for into `~/Library/Application Support/Pleikkari/models`:

| package | network | Video menu |
|---|---|---|
| `NeuralRendering-Small.dlssmodel` (216 MB) | 31 blocks; NVIDIA's default, about twice as fast | Performance model (5) |
| `NeuralRendering.dlssmodel` (278 MB) | HT, 71 blocks | Original model (6) |

FileVersion 310.9.16.0 of the DLL gives both; 310.8.0.0 gives HT only. The script uses
MLX-DLSS's own weight tool from SwiftPM's checkout, in a virtual environment under the
scratch directory. `tools/make-synthetic-model.py` writes a package with random weights
in the same format, for exercising the engine with no NVIDIA file at hand.

Keep the DLL and everything made from it out of git and out of anything that is
distributed.

## The interface (`Sources/CPlkNR/include/plknr.h`)

```c
plk_nr_engine *e = plk_nr_create(&config, error, sizeof(error));   // off the render thread
int taken = plk_nr_submit(e, frame, seq, reset, callback, context); // never blocks, never queues
plk_nr_set_controls(e, &controls);                                  // with the next frame taken
plk_nr_destroy(e);                                                  // waits for the frame in flight
```

Frames in and out are IOSurface-backed pixel buffers of non-linear R'G'B' in 0...1, BGRA8
or RGBA half float (the rendition of an HDR picture, which 8 bits are too coarse for);
the output has the size and the format of the input. The callback runs on a thread of
the engine's own, exactly once for a frame that was taken, and callbacks never overlap.
The engine is idle again before the callback is called, so the next frame can be offered
from inside it. `plk_nr_destroy` must not be called from inside it. `PLK_NR_ABI_VERSION`
is checked by the app when it loads the library.

In the app, `NeuralEnhancer` (`macos/app/src`) wraps an engine. Its destructor returns at
once on whatever thread drops the last reference (the render thread, a Metal callback)
and ends the engine on a queue of its own, because ending it waits for the frame in
flight.

## Without the app

```sh
D=build/native-app/nr/dist
$D/plk-nr-bench --model <models>/NeuralRendering-Small.dlssmodel --size 1280x720,960x540 --frames 30
$D/plk-nr-bench --model <models>/NeuralRendering-Small.dlssmodel --image shot.png --out shot-nr.png
```

The first form times the network on a moving synthetic picture; the second writes what
the network makes of one picture, for a look at its effect.

## Measured

On a MacBook Pro M1 Max (64 GB), 2026-11-25, float16, the network's own history on. The
target machine, an M5 Max, has not been measured.

The machine was in use during every run (load average 15 to 22; the replay's feeder woke
1.5 ms late on average, with the engine or without). The comparisons below are within one
run each, so the engine's share of a difference is real; the absolute tails would be
shorter on a quiet machine.

**The network**, in the app: a 20 s recording of a 1080p60 SDR game picture replayed
headless (`replay --headless --nr on`, the picture rendered offscreen at 3456x2234),
medians of 2 s windows:

| network | processed at | per picture | results a second | age of the result on a frame, p50 / p95 |
|---|---|---|---|---|
| small | 960x540 | 28.6 ms | 25 | 51 / 69 ms |
| small | 1280x720 (the default) | 43.6 ms | 18 to 19 | 72 / 100 ms |
| small | 1920x1080 | 89.9 ms | 10 | 149 / 197 ms |
| HT | 1280x720 | 80.0 ms | 11 | 127 / 168 ms |

The model loads in 0.3 to 0.5 s. The first picture after the very first load on a machine
took 4.3 s (MLX compiles its kernels); after that the first result is there within a few
frames, and after a change of size in 0.3 to 0.5 s.

**What it costs the stream's frames**, same replay, 10 s with the engine off, 20 s on, 10 s
off, 600 frames a window. GPU time of a frame's own pass, p50 / p95 / p99 in ms:

| scaler | off | on | off again |
|---|---|---|---|
| MetalFX | 2.42 / 2.69 / 2.86 | 1.83 / 4.03 / 5.85 and 1.91 / 4.03 / 5.69 | 2.22 / 2.67 / 3.25 |
| Lanczos | 3.00 / 3.25 / 3.67 | 2.44 / 5.03 / 6.45 and 2.58 / 4.89 / 6.61 | 2.59 / 3.29 / 4.24 |

So the median frame is not slower, and the slow frames are: about 1.3 to 1.8 ms more at
the 95th percentile and 2.5 to 3 ms at the 99th. (The median is lower with the engine on.
A GPU that never idles between frames would explain that; it was not looked into.) Decode
time did not change (p50 1.3 to 1.4 ms either way), and no frame was lost or replaced in
any of the runs.

**An HDR picture** (the same recording made into HDR10 with its highlights at 1000 nits,
replayed the same way; load average 14 to 19). The small network at 1280x720 took 46 to
53 ms a picture, 15 to 16 results a second, and the result on a frame was 76 to 90 ms old
at the median: the half-float rendition costs the network nothing that shows. The frames
pay for the mix, which converts every pixel to the rendition and back. GPU time of a
frame's own pass, p50 / p95 / p99 in ms, 600-frame windows (300 for bilinear):

| scaler | off | on | off again |
|---|---|---|---|
| MetalFX | 2.58 / 2.84 / 4.21 | 2.10 / 4.77 / 6.56 | 2.58 / 2.91 / 3.96 |
| Lanczos | 2.38 / 4.17 / 5.79 | 3.30 / 6.15 / 7.32 | 2.99 / 3.83 / 5.46 |
| bilinear | 0.76 / 0.80 / 1.31 | 1.37 / 4.35 / 6.35 and 2.11 / 4.94 / 6.83 | 0.76 / 0.80 / 0.81 |

With MetalFX the mix is done at the stream's size, before the scaler, and costs what it
costs an SDR picture: nothing at the median, about 2 ms at the 95th percentile. The other
scalers do it at the display's size (3456x2234 here), and there it adds 0.6 to 1.3 ms to
the median frame.

**Memory**, peak footprint of the app in a 10 s replay (`/usr/bin/time -l`; on Apple
Silicon it includes what the GPU holds):

| | |
|---|---|
| engine off | 95 MB |
| small network at 540p / 720p / 1080p | 1.9 / 2.2 / 3.0 GB |
| HT network at 720p | 3.1 GB |

Not measured here: frame ready to on glass. A headless run presents nothing, and a
windowed one takes the screen of a machine that is in use. The app's analysis panel
(Video menu, key I) shows it live, with the change against the engine off.

## Limits

By design:

- **No motion compensation.** The change is applied where the picture still looks like
  the frame it was made from. Fast motion therefore shows mostly the plain stream, and a
  still or slowly moving picture shows all of the network's work. Veyra warps its result
  along optical flow; that is not done here.
- **An HDR picture is enhanced through an SDR rendition.** The network never sees more
  than 1000 nits (brighter is clipped for it, not for the picture), and it sees the
  highlights with less contrast than they have. Where SDR white sits is fixed at 203
  nits; a game whose picture is much brighter or dimmer than that looks over- or
  underexposed to the network.
- **The GPU and the memory are shared**, see above.

Not yet looked at on a screen (everything above was measured headless):

- A result replaces the one before it at once, 10 to 25 times a second. Where two
  results disagree, a surface may shimmer; a cross-fade between results would be the
  remedy.
- How the picture looks while the change fades in and out around moving things.
- **An HDR picture from a console.** The HDR path is checked by numbers
  (`plk-video-enhancer-hdr-test`) and was run with the network on a recording converted
  to HDR10, which has the levels the rendition expects by construction. What the network
  makes of a real PS5 HDR picture has not been looked at.
- Frame ready to on glass with the engine on, in a window and in fullscreen.
