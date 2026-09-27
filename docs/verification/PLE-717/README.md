# PLE-717: the Go Library VR entry, live against PS5-466

Oculus Go 192.168.1.202:5555, serial 1KWPH802EW8203, Android 7.1.1. The PS5-466 probe at 12:49 (Go local time) showed `ready`, so the wake-from-rest path was not exercised. Debug APK from this branch (see `gate.txt`, GATE: PASS).

## MODE=stream (PLE-690's go-library.sh, unchanged)

Raw capture: `build/captures/ple717/stream/` (logcat.txt, shot-1.png, shot-2.png, session.log). The excerpts are in `stream-entry-cinema.txt` and `stream-session.txt`.

- The launch went through vrshell's desktop hand-off (uid 1000), `.stream.GoVrLibraryEntry`.
- `GoVrEntry: Picked PS5-466: the only of 1 linked console(s)` / `Connecting to PS5-466 at 192.168.1.164 (awake)`.
- `GoCinema: VrApi cinema entered; requested 72 Hz ...` came 0.27 s after the launch, and Takion connected 3 s later.
- `Cinema video:` showed 57-59 fps latched, with 360 of 360 submitted frames showing video, for about 60 s. There were 0 FATAL lines.
- Screencaps were taken with `debug.pleikkari.vr_full_pose=1` (set back to 0 afterwards and confirmed by getprop). `scripts/dev/go-latency/screen_check.py` said:
  `shot-1.png: mean luma 12.2, max 182, 16.2 % lit: picture`, `shot-2.png: mean luma 13.5, max 220, 17.9 % lit: picture`
  (Gran Turismo in-car view in both eyes).
- The APK and prefs were restored byte-identically, and vrshell MainActivity was resumed at the end.

## MODE=keys (`go-library-keys.sh`): the chooser cannot be driven over adb on this Go

`StreamVrActivity.dispatchKeyEvent` hands keys to `GoVrLibraryFlow.key()` only when
`event.isFromSource(SOURCE_GAMEPAD|SOURCE_JOYSTICK)`. Android 7.1.1's `input` has no source argument
(`keyevent ... (Default: keyboard)`, in `keys-run.txt`), so `input keyevent KEYCODE_BUTTON_A` arrives as a
keyboard event and the chooser ignores it. The chooser opened (`in-VR chooser first, no connect until one is chosen`).
DPAD/BUTTON_A were injected, but nothing connected (`keys-entry-cinema.txt`: 0 decoder frames for 60 s).

During a stream, gamepad keys go to the session (they reach the PS5), and Disconnect is only in the VrApi-polled menu:
the Go remote's Back button, then a click on the right third of the touchpad. No injected key reaches it.

## Stale secure vrshell layer

Before the first session, `go.sh status` showed `hosted-by-tv-panel=no`. After that session was force-stopped, it stayed `yes` for over 2 minutes,
although `dumpsys activity` showed nothing on any virtual display. So `go.sh am-start` refuses the next launch.
This is what refused PLE-690's second session, not another ticket's app. `DIRECT=1` in
`go-library-keys.sh` sends the same intent with a plain `am start` in that state.

## Left for the operator (a human wearing or holding the Go with a real pad)

- In-VR chooser: D-pad moves the selection, A picks the console, B leaves. This needs a real Bluetooth gamepad or the touchpad.
- Disconnect: Go remote Back, then a touchpad click on the right third, back to the chooser.
- The no-console message: it needs an install with no linked PS5. Clearing app data is forbidden here.
- The wake path: the PS5 has to be in rest mode by itself.

## PLE-744: MODE=keys now uses the debug broadcast; MODE=noconsole

`go-library-keys.sh` no longer injects `input gamepad keyevent` (the Go's `input` has no source argument).
MODE=keys sends PLE-739's `DEBUG_GO_VR_INPUT` broadcast instead: `right` (the pad's first move focuses the
last-played card), `centre` to Play, then after the stream `back`, `right` until GoVrUi logs
`Focus button 'Disconnect'`, and `centre`, which must log `Disconnect: back to the Library chooser`
(screencaps `chooser.png`, `menu-disconnect.png`, `after-disconnect.png`). The prefs now also set
`stream_go_vr_ui=true`, so the Library launch shows VR Home (PLE-730). MODE=noconsole sets
`debug.pleikkari.go_entry_no_console=1` (never connects, allowed under a PS5 hold) and records the
no-console sheet. All three debug properties go back to 0 on every exit path.

Not yet run on the Go: the first attempt (15:18 UTC 2026-09-27, `build/captures/ple744/keys/` in the
PLE-744 worktree) stopped at the entry step's battery check, the Go at 1 %. APK and prefs were restored.
