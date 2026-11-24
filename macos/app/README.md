# Pleikkari.app

A native macOS PS5 Remote Play client for the lowest latency this Mac can give. It is an
AppKit app with no Qt, SDL or libplacebo, built from three modules:

| module | what it does |
|---|---|
| `macos/host` | `SessionHost` (chiaki session), `AudioOutput` (CoreAudio), registration loader |
| `macos/video` | `VideoPipeline`: VideoToolbox decode, then Metal (zero copy), then `CAMetalLayer`, presented on arrival; HDR10 goes to EDR |
| `macos/dualsense` | `DualSenseHID`: IOKit HID input reports, effects, Bluetooth haptics |

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
| P, L | HDR output: PQ code values tone mapped by macOS, or linear light decoded in the shader | with the next frame; an SDR picture is not affected |

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
| `--pacing immediate\|immediate-vsync\|displaylink` | immediate | |
| `--scaler bilinear\|bicubic\|lanczos\|metalfx` | the menu's choice, else bilinear | |
| `--color accurate\|vivid` | the menu's choice, else accurate | colour of an SDR picture |
| `--hdr on\|off` | the menu's choice, else what the codec says | HDR stream or not, on top of `--codec` and the Qt setting |
| `--hdr-output pq\|linear` | the menu's choice, else pq | |
| `--no-saved-choices` | | neither use nor remember the Video menu's choices |
| `--stats-interval S` | 10 | 0 = off |
| `--no-frame-log` | per-frame lines on | |
| `--no-controller`, `--no-controller-output`, `--no-realtime-controller` | | |
| `--bt-haptics on\|off`, `--haptic-gain G` | Qt settings | |
| `--dump PATH` | | record the video samples (PLKVDMP1) |
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
- `frames:` the number of lines logged and of frames not presented.

`--diagnostics` adds the lib's per-second `stream:` line: frames, loss, jitter, FEC, and feedback gaps.

## Tests

`plk-app-tests` covers option parsing and settings precedence, input mapping (buttons, touches, motion on the sensor clock, motion reset), the mapping of other controllers and their rumble levels, the motor-power encoding, the frame-line format against frames.py's parser, and the log format against triggers.py's pattern. `build.sh` runs it together with `plk-host-tests`, `plk-dualsense-tests` and the video module's tests. None of them needs a console, a window or a controller.

`plk-gamepad-probe` (next to the app in the build directory) shows what the gamepad support sees with no window, stream or console: the controllers, whether their rumble engines start, and every mapped change. `--pulse` runs the motors for 2.5 s: heavy, light, then the haptics-stream path. `--seconds N`, `--quiet`.

The Video menu can be driven with no console and no hands: `--keys` makes the app press keys itself, through its own event queue, so it needs no permission to control the computer. This replays a dump in a window, picks Lanczos at 2 s, vivid at 4 s, opens the menu at 6 s and picks its second item with the arrow keys:

```sh
$APP replay <dump.plkv> --loop --seconds 12 --no-saved-choices --stats-interval 2 --log /tmp/m.log \
    --keys "2:3,4:v,6:m,7:down,7.4:down,8.2:enter" && grep 'keys\|Video:\|scaler=' /tmp/m.log
```

The `video:` statistics line names the scaler in use and the colour space the layer is tagged with.

To exercise the whole video path and telemetry with no console, replay a dump headless:

```sh
$APP replay build/mac/profile/<run>/video.plkv --headless --log /tmp/r.log && build/mac/profile/frames.py /tmp/r.log
```
