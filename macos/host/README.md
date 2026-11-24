# macos/host: native session host

One chiaki session on macOS without Qt, SDL or FFmpeg: `SessionHost` (session, input,
events), `AudioOutput` (Opus PCM to CoreAudio), the read-only registration loader for
the Qt app's settings, the `PLKVDMP1` video dump writer, `plk-host-probe` and the unit
tests. LAN or direct addresses only: no PSN, registration, wake-up or discovery.

## Build

    macos/host/build.sh [<out-dir>]

It configures the top-level project with `-DCHIAKI_ENABLE_MACOS_NATIVE=ON
-DCHIAKI_ENABLE_GUI=OFF` (Homebrew `opus json-c miniupnpc openssl@3`, and the
`curl gf-complete jerasure nanopb` submodules), builds `plk-host-probe` and
`plk-host-tests`, and runs the tests. Set `PYTHON` to reuse a python with `protobuf`.

## Run

    plk-host-probe --list                     # nickname and console type, nothing else
    plk-host-probe --connect <nickname> <address> [--seconds N] [--dump file.plkvdmp]

`--connect` starts a real session (the console allows one at a time) and counts video
frames, bytes and losses; `--audio` also plays the game audio. `plk-host-tests` needs no
hardware or network; `PLK_TEST_COREAUDIO_OPEN=1` adds a test that opens (never starts)
the default output device.

## API

- `SessionHost(delegate, videoSink, pcmSink)`, `start(ConnectTarget, StreamSettings)`,
  `stop()` (any thread), `shutdown()` (joins; not from a callback), destructor = shutdown.
- `VideoSink::onVideoSample()` gets the library's buffer on the "Chiaki Takion" thread,
  no queue, no copy; its return value goes back to the library.
- `setControllerState()` and `requestKeyframe()` from any thread.
- `SessionDelegate`: events as the library raises them, on its threads (listed in
  `SessionHost.h`).
- `AudioOutput` is a `PcmSink`: hand it to `SessionHost` and it opens the default output
  on the stream's format. `stats()` has underruns, overflows, the fill histogram and the
  latency CoreAudio reports.
- `loadRegisteredConsoles(qtSettingsPlistPath(), consoles)`: never print anything from a
  `RegisteredConsole` but `nickname` and `consoleTypeName()`.
- `loadQtStreamSettings(qtSettingsPlistPath(), settings)`: the Qt app's non-secret stream
  settings for a PS5 on the LAN (resolution, fps, bitrate, codec, haptics gain, Bluetooth
  haptics, reported packet loss); nothing else is read.
