# PLE-869: pressing pad buttons on the streamed PS5 from adb

A script, or an agent, needs to walk PS5 and game menus without anyone holding a
controller. `adb shell input keyevent` cannot do it: the D-pad only reaches
the app as a gamepad's HAT motion, and in a landscape stream a scripted tap lands
on the on-screen gamepad (PLE-453). This is the devtools affordance that does.

## What it does

In a **debug build** (`BuildConfig.DEBUG`), `StreamActivity` (phone) and
`StreamVrActivity` (Go) register, while resumed,
`PadScriptReceiver` for `fi.madekivi.pleikkari.PAD`. A broadcast carries a whole
sequence of presses. `PadScriptRunner` plays it on its own thread into
`StreamInput.scriptedControllerState`, a layer of the controller state of its
own. The console gets it merged with the physical pad, touch controls and
sensors, so a script and a held pad button never clear each other.

- **Only adb can send it.** The receiver requires the sender to hold
  `android.permission.DUMP`. adb's shell has it and an installed app cannot get
  it. A release build never registers the receiver.
- **One round trip per sequence.** The broadcast completes only when the
  sequence has been sent, or after 8 s with the rest still playing. Its result
  says how it went. So the next command, such as a screenshot, sees the outcome.
- **It is real controller input.** It reaches the console exactly as a pad would.
  `scripts/dev/pad.sh` refuses to send while `PS5-HOLD` is set.

## How to use it

From the workspace, with a stream open on the S22:

```sh
scripts/dev/pad.sh down*3 cross --shot /tmp/menu.jpg     # press, then screenshot (1280 px wide)
scripts/dev/pad.sh ps:1200                               # hold PS for 1.2 s (control centre)
scripts/dev/pad.sh --shot /tmp/now.jpg                   # screenshot only
scripts/dev/pad.sh --cancel                              # stop a long sequence, release the pad
```

Or with adb directly. `--receiver-foreground` keeps the broadcast off the
slower background queue:

```sh
adb shell "am broadcast --receiver-foreground -a fi.madekivi.pleikkari.PAD -p fi.madekivi.pleikkari --es seq 'down*3 cross'"
# Broadcast completed: result=-1, data="ok 4/4 presses in 805 ms"
```

| Result | Meaning |
|---|---|
| `result=-1, data="ok N/N presses in T ms"` | sent |
| `result=-1, data="running ..."` | over 8 s; the rest is still playing |
| `result=1, data="error: ..."` | refused: a parse error, or `not streaming (<state>)` |
| `result=1, data="cancelled ..."` | `--ez cancel true` arrived while it played |
| `result=0`, no data | nothing listening: no stream activity, or not a debug build |

## Sequence grammar

Tokens are separated by spaces or commas, and case does not matter.
`PadScript.kt` is the reference, and `PadScriptTest` covers every form below.

| Token | Effect |
|---|---|
| `cross` `circle` `square` `triangle` (`x`, `o`) | tap a face button |
| `up` `down` `left` `right` | tap the D-pad |
| `l1` `r1` `l2` `r2` `l3` `r3` | tap a shoulder, trigger (fully) or stick click |
| `options` `create` (`share`) `ps` `touchpad` (`tp`) | tap a system button |
| `lx=` `ly=` `rx=` `ry=` -1..1 | a stick; negative is left or up, so `ly=-1` is fully up |
| `l2=` `r2=` 0..1 | an analog trigger |
| `a+b` | a chord, held together: `l1+r1`, `r2+lx=0.5` |
| `token*N` | N taps, each followed by the gap: `down*3` |
| `token:MS` | hold for MS instead of the default: `ps:1200`, `ly=-1:800`, `down*3:60` |
| `wait:MS` | nothing pressed for MS, for example while a menu animates in |

A press holds for 100 ms and is followed by a 100 ms release. Change these with
`--el hold_ms N` and `--el gap_ms N` (`pad.sh --hold N --gap N`). A hold is
20..10000 ms, a wait up to 30 s, a repeat up to 50 and a sequence up to 500
presses. A hold under about 50 ms can be missed by a game that polls the pad
slowly. A release gap of 0 merges two taps of the same button into one press.

The receiver logs each sequence and its outcome under the `PadScript` tag:
`adb logcat -s PadScript`.

## Frame grab: the decoded frame at native resolution (GRAB)

`adb screencap` gives the video only at its on-screen size (1080x608 in portrait
for a 1280x720 stream) and with the touch controls drawn over it. A debug
`StreamActivity` also registers, while resumed, `FrameGrabReceiver` for
`fi.madekivi.pleikkari.GRAB` (same `android.permission.DUMP` sender rule, same
create/register/unregister lifecycle as the pad receiver; the Go VR activity is
not covered).

```
adb shell am broadcast --receiver-foreground -a fi.madekivi.pleikkari.GRAB -p fi.madekivi.pleikkari [--es name menu.png]
adb exec-out cat /sdcard/Android/data/fi.madekivi.pleikkari/files/grab/latest.png > frame.png
```

The reply is `ok <W>x<H> <absolute path> in <ms> ms`, or `error: ...`
(`not streaming (<state>)`, `grab not supported on ... renderer`, ...); a bare
`result=0` means nothing is listening. The file is written through a temp file
and a rename, so a poller never reads half a PNG; `name` must be a plain name
ending in `.png`.

- **Default path only.** With post-processing (debanding) off, which is the
  default, the video decodes straight into the `SurfaceView` and the grab is
  `PixelCopy.request(surfaceView, bitmap(videoW x videoH))`, which reads the
  surface's own buffer, so the pixels are the decoder's, not scaled to the view.
  Overlays are separate windows and are not in it. With debanding on (GLSurfaceView
  or native EGL) the grab answers `error: grab not supported on ... renderer`.
- **Size** is the stream profile's width x height (`connectInfo.videoProfile`),
  which is also what the native decoder is configured with; the decoder's output
  format is not read back from the NDK codec. Switching the stream to 1080p
  therefore gives 1920x1080 frames without any change here.
- The workspace wrapper is `scripts/scratch/grab.sh`.
