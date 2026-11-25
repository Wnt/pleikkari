# Pleikkari.app

A native macOS PS5 Remote Play client for the lowest latency this Mac can give. It is an
AppKit app with no Qt, SDL or libplacebo, built from three modules and one that is optional:

| module | what it does |
|---|---|
| `macos/host` | `SessionHost` (chiaki session), `AudioOutput` (CoreAudio), registration loader |
| `macos/video` | `VideoPipeline`: VideoToolbox decode, then Metal (zero copy), then `CAMetalLayer`, presented on arrival; HDR10 goes to EDR |
| `macos/dualsense` | `DualSenseHID`: IOKit HID input reports, effects, Bluetooth haptics |
| `macos/nr` (optional) | `libplknr.dylib`: NVIDIA's DLSS 5 Neural Rendering network on Apple Silicon, running beside the stream; a build of its own, see [`macos/nr/README.md`](../nr/README.md) |

The data paths have no queues or timers between the stages:

- **Video.** On the lib's Takion thread: `VideoPath::onVideoSample`, then `VideoPipeline::submitFrame`. Decoded frames are rendered and presented as soon as they arrive (`--pacing immediate`).
- **Audio.** On the Takion thread: Opus, then a lock-free ring, then the CoreAudio IO thread.
- **Controller.** On the DualSense HID thread, each input report becomes `ChiakiControllerState` and goes to `SessionHost::setControllerState`.
- **Feedback.** On the Takion thread, console events go to the `DualSenseHID` setters, then to the writer thread. Changes within 2 ms leave as one report.
- **Other controllers.** An Xbox controller, a DualShock 4, a Switch Pro controller or an MFi pad comes through GameController (`Gamepads.mm`); the DualSense stays with the HID driver. Each change is mapped by position (A is Cross, B Circle, X Square, Y Triangle, Menu is Options, View is Create, the Xbox button is PS, Share is the touchpad click) on a queue of its own and merged with the DualSense's state. The system's shortcuts on the Home, Menu, View and Share buttons are turned off for the app. Rumble: the console's rumble events drive the heavy motor (left handle) and the light one (right handle) through CoreHaptics; a PS5 game's haptics stream cannot be played by a rumble motor, so its loudness drives both, with the Qt app's formula (`GamepadRumble.h`). Adaptive trigger effects, motion and the touchpad surface have no counterpart on these controllers.

## Build

```sh
wt/native-app/macos/app/build.sh            # configure (first time), build, run the unit tests
wt/native-app/macos/app/build.sh --no-tests
```

The build goes into `<workspace>/build/native-app/app`, and the app is at `macos/app/Pleikkari.app` inside it. `build.sh` uses `nice -n 15` and `-j 4` (change them with `NICE` and `JOBS`). It also uses the full Xcode SDK (`xcrun --sdk macosx`) and Homebrew's openssl@3. `PYTHON` defaults to `build/mac/venv/bin/python`.

`build.sh` runs the equivalent of this:

```sh
cmake -G Ninja -S wt/native-app -B build/native-app/app -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_SYSROOT=$(xcrun --sdk macosx --show-sdk-path) -DCHIAKI_ENABLE_MACOS_NATIVE=ON \
  -DCHIAKI_ENABLE_GUI=OFF -DCHIAKI_ENABLE_CLI=OFF ...   # see build.sh
cmake --build build/native-app/app -j 4
```

`CHIAKI_ENABLE_MACOS_NATIVE` builds `macos/{host,video,dualsense,app}`. Everything under `macos/` targets macOS 14, because the video pipeline uses `CAMetalDisplayLink`. The bundle is signed ad hoc after every build.

**Neural rendering** is not part of this build. `macos/nr/build.sh` builds the engine (Swift, MLX), and `build.sh` here copies it into the bundle when its `dist` directory exists (`NR_DIST`, default `<workspace>/build/native-app/nr/dist`): `Contents/Frameworks/libplknr.dylib` and `Contents/Resources/mlx-swift_Cmlx.bundle`. The models are made once with `macos/nr/tools/prepare-model.sh` and live in `~/Library/Application Support/Pleikkari/models`. An app without the engine or without a model runs as before and logs one line saying which is missing.

## Run

```sh
APP=build/native-app/app/macos/app/Pleikkari.app/Contents/MacOS/Pleikkari
$APP list                                    # registered consoles: nickname and type only
$APP stream PS5-466 192.168.1.164 --fullscreen --log /tmp/pleikkari.log
$APP replay video.plkv --headless --log /tmp/replay.log   # a recorded stream, no console
$APP --help
```

The launcher script creates a run directory with the log in it, and prints the commands to analyse it. By default it starts the app through `open`, so LaunchServices activates the app and the local network prompt names Pleikkari; `DIRECT=1` runs the binary from the shell instead:

```sh
FULLSCREEN=1 wt/native-app/macos/app/run-pleikkari.sh mylabel
FULLSCREEN=1 wt/native-app/macos/app/run-pleikkari.sh hdr -- --codec h265-hdr
```

A recorded stream plays in a window the same way, looping, with no console; this is how the Video menu, neural rendering and the analysis panel are tried on the same pictures again and again:

```sh
wt/native-app/macos/app/replay-pleikkari.sh <dump.plkv> -- --keys 2:i      # the analysis panel after 2 s
```

**The console.** The registration comes from the Qt app's settings: `~/Library/Preferences/com.chiaki.Chiaki.plist`, or the `--profile` one. The app reads them through `macos/host/Registration`, never writes them, and never logs a value from them except the nickname and the console type. Register the console in the Qt app first.

**Stream settings.** The app reads these Qt settings for a local PS5 as defaults:

- resolution, fps, bitrate and codec;
- `haptic_override` (the haptics gain);
- `dualsense_bt_haptics`;
- `packet_loss_reported_max`.

Where a setting is missing, the default is the Qt app's own: 1080p, 60 fps, H.265 and the preset bitrate (1080p: 15000 kbit/s). Command-line options override both. The log says where each value came from.

**Ending a stream.** Use Cmd-Q or close the window; the stream also ends when the console ends the session. Shutdown runs in this order:

1. `SessionHost::stop` and `shutdown`;
2. the DualSense manager stops, which stops haptics and sends one report with effects off;
3. the video pipeline;
4. the audio output.

A login PIN request is logged and ends the session. Set the PIN up in the Qt app, or turn it off on the console.

**While streaming:**

- Cmd-F toggles native fullscreen. In fullscreen, macOS Game Mode turns on and halves the DualSense's Bluetooth report interval.
- The cursor is hidden over the video.
- The app holds `NSActivityLatencyCritical | NSActivityUserInitiated` and keeps the display awake.
- The **Video menu** changes the picture while the stream runs. It is in the menu bar, under a right click on the picture, and under the M key, where the arrow keys and Return work in it. Each choice also has a key of its own, pressed without a modifier (the keyboard is free: the DualSense is the input device). A line over the picture says for two seconds what changed.

| key | choice | |
|---|---|---|
| 1, 2, 3, 4 | scaling: bilinear, bicubic, Lanczos, MetalFX | with the next frame |
| A, V | colour of an SDR picture: accurate (the console's colours), vivid (stretched to the display's full gamut, as a renderer without colour management shows it) | with the next frame; an HDR picture is not affected |
| H | HDR stream on or off | **reconnects**: the console fixes the codec when a session starts, so the app ends the session and starts a new one. Window, video pipeline, audio output and controller stay; the picture is gone for a few seconds. PS5 only |
| P, L | HDR output: PQ code values, or linear light | with the next frame; an SDR picture is not affected |
| +, -, 0 | HDR brightness: 5 % brighter, 5 % darker (20 to 200 %; a held key keeps stepping), back to 100 % | with the next frame; an SDR picture is not affected. A factor on the picture's linear light, so colours keep their hue. At 100 % HDR reference white (203 nits) is as bright as the display's SDR white |
| T | HDR tone mapping by macOS instead of the app | with the next frame. The app's own (the default) fits the picture to the display in the shader and puts no HDR metadata on the layer, so macOS hands a fullscreen picture straight to the display: frame ready to on screen 4.9 ms at the median, as for SDR. With macOS's, the picture goes through the compositor (about 10 ms), comes out about 1.7 times brighter in the midtones with its peak stretched to the display's full headroom, and in fullscreen stops for about 55 ms every 64 refreshes of the display |
| S | Fullscreen in step with the display, or not | with the next frame. A fullscreen picture goes straight to the display, past the compositor. In step (the default) each frame is switched in at a refresh, so the picture cannot tear; otherwise it is switched in at once, wherever the refresh is, and tears. A window is composited either way and does not tear. Measured on the built-in 120 Hz panel with a 60 fps clip in a console's cadence: frame ready to on screen 9.4 to 10.0 ms at the median and 14 to 16 ms at p95 in step, 4.1 to 4.8 and 6.6 to 7.1 ms otherwise, so no tearing costs about 5 ms |
| N | neural rendering on or off | on: with the first result, a fraction of a second after the model has loaded (0.3 to 0.5 s; several seconds the very first time). Off: with the next frame. On by default when the engine and a model are installed. An SDR and an HDR picture alike: of an HDR one the network is given an SDR rendition, and its change is made to the HDR picture in proportion, so highlights stay highlights (see [`macos/nr/README.md`](../nr/README.md)) |
| 5, 6 | the network: performance model (small, the default), original model (HT, about half as fast) | loads the other model beside the stream; the old one keeps working until the new one is ready. A network whose model is not installed is not switched to, and a line says what is missing |
| 7, 8, 9 | how much picture the network is given: 540p, 720p (the default), the stream's size | with the next result. Smaller is faster, so a result is fresher when it reaches a frame |
| X, Z | strength: 10 % stronger, 10 % weaker (10 to 200 %; a held key keeps stepping). The menu's "Strength" item goes back to 100 % | with the next frame. A factor on the change the network made, applied in the shader |
| E | next look: standard, natural, cinematic, neutral (the network's own rendering profiles); the menu also has one item for each | with the next result |
| C | compare: the left half of the picture is shown without the network's change, with a line between | with the next frame |
| I | frame time analysis: a panel over the bottom of the picture, see below | at once. Not remembered |
| G | the pipeline panel: every choice above, laid out as the pipeline it changes, at the right of the window; see below | at once. Not remembered |
| F | frame rate overlay: frames a second, the frame time and the last 120 frame intervals over the picture's top left corner; see below | with the next frame. On by default in a window |

Neural rendering runs **beside** the stream: every frame is decoded, scaled and presented as before, and the network's change to an earlier frame is mixed in where the picture still looks like that frame. Nothing waits for the network. What it costs is GPU time that the frames share with it; [`macos/nr/README.md`](../nr/README.md) has the numbers and the limits.

**The analysis panel** (I) shows where a frame's time goes, as averages of the last second:

- **Signal**: the stream, the output size and scaler, the colour space, the neural rendering in use, and the order of the stages.
- **Stages**: one bar for each consecutive stage of a frame (to decoder, decode, queue, render, GPU wait, GPU, scan-out), as a share of the frame interval (the budget), with a mark at the 95th percentile. Beside them the network's own times (input pass, network, round trip, age of the result on a frame; these are not in a frame's path) and the two sums, processing and to glass. With neural rendering on, each value carries its change against the last full second measured with it off: toggle N to refresh that comparison.
- **Frame rate and pacing**: frames a second on glass, the 1 % low (from the 99th percentile of the frame intervals of the last 10 s), the last 120 intervals against the budget, and three gauges: processing time against the budget, the share of frames that carried a result, and the result's age. The line at the bottom turns orange when frames are not shown or processing exceeds the budget.

The panel is an ordinary view over the picture, so while it is up macOS composites a fullscreen picture instead of handing it straight to the display. Measure latency with the log, and use the panel to see what a choice does.

**The pipeline panel** (G) is the Video menu as a picture of the pipeline, after the panel of Veyra-NRVideo. It takes 384 points at the right of the window (never more than half of it) and is used with the mouse; the keys keep working while it is open.

- **Tabs**: Quality (neural rendering, scaling), Colour (HDR stream, HDR picture, SDR colour), Display (in step with the display, frame rate overlay, frame time analysis, the panel itself).
- **Processing order**: the stages a frame goes through, in order: input, neural rendering, HDR, scaling, colour, display. A stage that does nothing to the picture as it is now (neural rendering off or not installed, the fitting of an HDR picture while the picture is SDR, vivid colour while it is accurate or the picture is HDR) is struck out. A click on a stage shows its card, on whichever tab that is.
- **Cards**: one for each stage, with an icon, a line about what it is doing now (the size, the strength and what a result takes for the neural rendering; the sizes and the GPU time for the scaler), a switch where the stage can be off, and its settings: a row of named buttons, a switch, or a number with − and + (a click on the number takes it back to 100 %). A click on a card's head folds its settings away. A card that does nothing to this picture is dimmed and still works. The neural rendering card says how old a result is when it is shown, in orange, because that age is what a moving picture ghosts by.
- **Footer**: a dot (green, or orange when the analysis says something is not keeping up), the sizes a frame goes through (`Input 1920×1080 / NR 1280×720 / output 2880×1620`), and **Reset this page**, which takes the tab's settings back to their defaults. It leaves the HDR stream alone: that one reconnects.

A click does what the menu item or the key does, by the same code: the choice is announced over the picture and remembered the same way. With the panel open the picture is fitted **beside** it, whole and smaller, above the analysis panel when that is open too; the panel's own card has the other choice, **under**, where the picture keeps the size it is played at and the panel lies over its right edge. That is the size to judge the scaler at: fitted beside the panel, a 1080p stream is scaled to a different size than in play. The choice is remembered.

Like the analysis panel it is a view over the picture, so a fullscreen picture is composited while it is open. Veyra's panel has tabs and cards for stages this pipeline does not have (DLAA, frame generation, RTX Video HDR, several effect layers and their order, an output stabiliser, protected areas, audio). They are not in this panel: a control that does nothing would say otherwise.

**The frame rate overlay** (F) is drawn by the video pipeline into the frame itself, in the pass that scales the picture, not by a view. So it costs no extra pass, it is there in a fullscreen picture that goes straight to the display, and a headless `--capture` shows it. It has the frames a second on glass over the last second (white, orange under 95 % of the stream's rate, red under 80 %), the mean frame time of the last quarter second, and the last 120 intervals between frames as a line, against a dashed line at the stream's frame interval. It is redrawn ten times a second on a thread of its own. Over an HDR picture its white is the display's SDR white.

What is picked there is remembered (`defaults read fi.madekivi.pleikkari.mac`) and used by the next run that does not give the matching option; `--no-saved-choices` neither uses nor remembers it. Headless runs never use it.

| option | default | |
|---|---|---|
| `--resolution 360p\|540p\|720p\|1080p` | Qt setting, else 1080p | |
| `--fps 30\|60` | Qt setting, else 60 | |
| `--bitrate KBPS` | Qt setting, else the preset | 0 = preset |
| `--codec h264\|h265\|h265-hdr` | Qt setting, else h265 | `h265-hdr`: HDR10 through EDR |
| `--fullscreen` | off | |
| `--feedback-interval MS` | 8 (lib) | minimum spacing of stick and motion packets |
| `--no-reorder` | reordering on | the lib does not wait up to 16 ms for a missing video packet |
| `--log PATH` | stderr | |
| `--pacing immediate\|immediate-vsync\|displaylink` | immediate | immediate: a frame is shown as soon as it is decoded, without display sync; immediate-vsync: with it |
| `--fullscreen-sync on\|off` | the menu's choice, else on | on: `--pacing immediate` becomes immediate-vsync while the window is fullscreen (no tearing, about 5 ms slower) |
| `--scaler bilinear\|bicubic\|lanczos\|metalfx` | the menu's choice, else bilinear | |
| `--color accurate\|vivid` | the menu's choice, else accurate | colour of an SDR picture |
| `--hdr on\|off` | the menu's choice, else what the codec says | HDR stream or not, on top of `--codec` and the Qt setting |
| `--hdr-output pq\|linear` | the menu's choice, else pq | |
| `--hdr-brightness PCT` | the menu's choice, else 100 | 20 to 200; at 100 HDR reference white is as bright as SDR white |
| `--hdr-tone-map app\|system` | the menu's choice, else app | who fits an HDR picture to the display |
| `--nr on\|off` | the menu's choice, else on in a window and off with `--headless` | neural rendering of the picture, SDR or HDR; needs the engine and a model |
| `--nr-network small\|ht` | the menu's choice, else small | |
| `--nr-height N` | the menu's choice, else 720 | height the network is given, 360 to 2160; 0 = the stream's |
| `--nr-strength PCT` | the menu's choice, else 100 | 10 to 200 |
| `--nr-look standard\|natural\|cinematic\|neutral` | the menu's choice, else standard | |
| `--nr-library PATH` | `Contents/Frameworks/libplknr.dylib` of the app | the engine |
| `--nr-models DIR` | `~/Library/Application Support/Pleikkari/models` | where the two models are looked for |
| `--nr-model PATH` | | one `.dlssmodel` package, whatever the network choice |
| `--fps-overlay on\|off` | the menu's choice, else on in a window and off with `--headless` | the frame rate over the picture's corner |
| `--panel-picture beside\|under` | the panel's choice, else beside | where the picture is while the pipeline panel is open |
| `--no-saved-choices` | | neither use nor remember the Video menu's choices |
| `--stats-interval S` | 10 | 0 = off |
| `--no-frame-log` | per-frame lines on | |
| `--no-controller`, `--no-controller-output`, `--no-realtime-controller` | | |
| `--bt-haptics on\|off`, `--haptic-gain G` | Qt settings | |
| `--capture DIR` | | with `--headless`: keep copies of rendered frames and write them to DIR at the end (`NNNN.raw`, `frames.csv` with each frame's stream index and the age of the neural rendering result mixed into it). `--capture-skip N` lets N rendered frames pass first, `--capture-frames N` keeps N (default 120; 8 MB each at 1920x1080, in memory), `--capture-every N` one in N. `tools/capture-to-png.py DIR [--pq]` makes pictures of them |
| `--dump PATH` | | record the video samples (PLKVDMP1). PATH gets the first stream. A different stream that starts later (H restarts the session in the other codec; the console may change the stream) goes to a file of its own beside it, named after what it is: `video.plkv`, `video-2-sdr.plkv`, `video-3-hdr.plkv`. Each file starts with its own config sample and replays by itself. `DUMP=1 run-pleikkari.sh <label>` records into the run directory |
| `--headless` | | no window or NSApplication; video renders offscreen; the controller is only read |
| `--retry-window S` | 20 | restart a session that ends before it connects |
| `--seconds S` | | end after S seconds |

Environment variables:

- `CHIAKI_TRIGGER_LOG=1` logs every trigger-effect message from the console at info level, followed by a line for what was applied to the controller.
- `CHIAKI_BT_HAPTICS_KEEP_RUMBLE=1` keeps the console's rumble while Bluetooth haptics play.
- `CHIAKI_BT_HAPTICS_*` sets the haptics tuning (see `macos/dualsense`).

## Reading the telemetry

The log uses the Qt app's session-log line format, `[YYYY-MM-DD HH:MM:SS:uuuuuu] [L] message`, so `build/mac/profile/frames.py` and `triggers.py` read it unchanged:

```sh
build/mac/profile/frames.py  <run>/session.log 20     # per-frame latency, skipping the first 20 s
build/mac/profile/triggers.py <run>/session.log        # trigger effect messages (CHIAKI_TRIGGER_LOG=1)
```

**Per frame.** The app logs one line per frame that reached the glass. A frame the compositor replaced before showing it gets no line; the stats count it as `not_presented`.

```
[frame] idx=4242 pkts=37 first=996000 ready=1000000 swap=1010000 assemble_us=4000 to_decoder_us=50
        decode_us=2500 queue_us=50 render_us=500 swap_us=6900 total_us=10000 total_first_us=14000
        gpu_wait_us=100 gpu_us=500 scanout_us=6300 key=1
```

`first`, `ready` and `swap` are absolute times in microseconds on the `mach_absolute_time` clock, which is also the clock of `MTLDrawable.presentedTime`:

- `first`: the first video packet of the frame, from the lib's trace hook;
- `ready`: the lib finished reassembling the frame;
- `swap`: the frame was on glass (`presentedTime`).

The lib stamps `ready` with `CLOCK_MONOTONIC`, which on macOS includes time asleep. The pipeline translates it when the frame is submitted, so all three can be subtracted from each other.

The stages are consecutive and add up to `total_us`:

| stage | from | to |
|---|---|---|
| `assemble_us` | first packet | frame complete (outside `total_us`; inside `total_first_us`) |
| `to_decoder_us` | frame complete | `submitFrame()` |
| `decode_us` | `submitFrame()` | VideoToolbox output |
| `queue_us` | decoded | render thread starts the frame |
| `render_us` | render start | command buffer committed |
| `swap_us` | committed | `presentedTime` (GPU, compositor, scan-out) |

`swap_us` is broken down further, outside the sum:

- `gpu_wait_us`: commit to GPU start;
- `gpu_us`: GPU time;
- `scanout_us`: GPU end to `presentedTime`.

A value of -1 means the stage was not measured. With `--headless` nothing is presented: those lines carry `offscreen=1`, and `swap` is the GPU end.

**Every `--stats-interval` seconds** the app logs one line for each part:

- `video:` the stream format and frame counters, with p50/p95/p99/max in ms of the submit call, decode, queue, render CPU and GPU, frame-ready to presented, and the present interval (all for that window);
- `audio:` frames in, underruns, silence, hard drops, ring fill, playout margin, drift, and device latency;
- `controller:` input reports and their rate, the largest motion step on the sensor clock, and console rumble and trigger events (applied, dropped or suppressed). Per device it adds reports in, CRC errors, effect reports written, reports merged by the 2 ms window, write times, and Bluetooth haptics reports and underruns;
- `gamepads:` controllers other than the DualSense, the input changes they reported, their rumble motors, how often a motor level was changed (and refused), and haptics-stream packets seen;
- `frames:` the number of lines logged and of frames not presented;
- `nr:` (when neural rendering is on or did something in the window) the choices in use, frames offered to the network, results completed and failed, how many rendered frames carried a result, and p50/p95/p99/max in ms of the round trip, of the input pass on the GPU, and of the result's age on a frame;
- `analysis:` what the analysis panel shows, as text: the budget, fps and 1 % low, the mean of each stage over the last second, then the network's times after `| nr:`, then, while neural rendering is on, the stage means of the last second measured with it off after `| with nr off:`.

`--diagnostics` adds the lib's per-second `stream:` line: frames, loss, jitter, FEC, and feedback gaps.

## Tests

`plk-app-tests` covers option parsing and settings precedence, input mapping (buttons, touches, motion on the sensor clock, motion reset), the mapping of other controllers and their rumble levels, the motor-power encoding, the frame-line format against frames.py's parser, the log format against triggers.py's pattern, the analysis' arithmetic (stages add up, the comparison against neural rendering off, the 1 % low), and where the neural rendering engine and its models are looked for. `build.sh` runs it together with `plk-host-tests`, `plk-dualsense-tests` and the video module's tests. None of them needs a console, a window or a controller.

`plk-analysis-render OUT.png [WIDTH]` (next to the app in the build directory) draws the analysis panel into a PNG from made-up numbers: a look at its layout with no window.

`plk-panel-render OUTDIR [--picture FILE] [--hdr] [--size WxH]` does the same for the pipeline panel: its three pages, the frame rate overlay, and the whole window as it is laid out with the panel open, the picture beside the panel and under it. `plk-panel-render --check` (the test `plk-panel-view`) clicks on what the panel drew and checks what every click sends: each setting's own command, a card's head folding it, a tab, a stage of the processing order bringing its card into sight, the reset, and that a switch that cannot be used and a control scrolled out of sight take no click. What the panel is made of for a given state of the run (which stages are struck out, which button is lit, what a reset sends) is `PipelinePanelTests.cpp`; the overlay's picture is `FrameHudTests.cpp`.

`plk-gamepad-probe` (next to the app in the build directory) shows what the gamepad support sees with no window, stream or console: the controllers, whether their rumble engines start, and every mapped change. `--pulse` runs the motors for 2.5 s: heavy, light, then the haptics-stream path. `--seconds N`, `--quiet`.

The Video menu can be driven with no console and no hands: `--keys` makes the app press keys itself, through its own event queue, so it needs no permission to control the computer. This replays a dump in a window, picks Lanczos at 2 s, vivid at 4 s, opens the menu at 6 s and picks its second item with the arrow keys:

```sh
$APP replay <dump.plkv> --loop --seconds 12 --no-saved-choices --stats-interval 2 --log /tmp/m.log \
    --keys "2:3,4:v,6:m,7:down,7.4:down,8.2:enter" && grep 'keys\|Video:\|scaler=' /tmp/m.log
```

The `video:` statistics line names the scaler and pacing in use and the colour space the layer is tagged with.

A replay that is to measure a fullscreen picture needs its dump outside `~/Documents`, `~/Desktop` and `~/Downloads`. The app, started through `open`, has to ask before it may read a file there that it did not write itself, and it asks again after every rebuild (the build is signed ad hoc). While macOS's question is on screen the fullscreen picture is composited instead of going straight to the display, which measured 20 ms from GPU work to screen instead of 6; and the question stays up after the app has ended.

`--keys` also works with `--headless`, for the keys that need no window (everything in the table above except H, S and I, and not the menu itself). One replay then measures a choice against another on the same pictures. This one runs 10 s without neural rendering, 20 s with it and 10 s without, and prints what the frames paid for it:

```sh
$APP replay <dump.plkv> --headless --loop --seconds 41 --stats-interval 10 --no-frame-log --log /tmp/nr.log \
    --keys "10.2:n,30.2:n" && grep -E '\] \[I\] (video|nr|analysis):|keys' /tmp/nr.log
```

To exercise the whole video path and telemetry with no console, replay a dump headless:

```sh
$APP replay build/mac/profile/<run>/video.plkv --headless --log /tmp/r.log && build/mac/profile/frames.py /tmp/r.log
```
