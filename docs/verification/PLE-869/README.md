# PLE-869 verification: adb pad-script API

## S22 (serial 192.168.1.107:5555, SM-S908B, Android SDK 36), 2026-10-07

`s22-session.txt` is the whole device session and `s22-preview-logcat.txt` is
its `PadScript`/`StreamActivity` logcat. The session took a `device.py` lease
for PLE-869, backed up the app state (registration `PS5-466` verified), ran
`install -r` on this branch's debug APK, then:

| Probe | Result |
|---|---|
| broadcast with no stream activity open | `result=0`, no data: nothing listening |
| `am start ... --ez diagnostics_preview true` | `PadScript: listening for fi.madekivi.pleikkari.PAD` |
| `--es seq 'down*3 cross'` on the preview (no console) | `result=1, data="error: not streaming (StreamStateIdle)"` |
| `--ez cancel true` | `result=-1, data="ok cancelled"` |
| 5 round trips, host to S22 over Wi-Fi adb | 146-168 ms each |

This proves the receiver registers with the stream activity, that adb's shell
passes the `android.permission.DUMP` sender check, and that the result travels
back through `goAsync`. The diagnostics preview never opens a session
(`externallyManaged`), so nothing contacted the PS5. PS5-HOLD was in place
(since 2026-09-27).

## Live PS5 (operator lifted PS5-HOLD), 2026-10-07 23:05 local

S22 `192.168.1.107:5555` streamed to PS5-466 (`192.168.1.164`) after a debug
`am start ... -e auto_connect_host 192.168.1.164`. The console was running
Gran Turismo 7 (`live-0-gt7.jpg`). Each step was one `scripts/dev/pad.sh` call,
with a screenshot taken after it. `s22-live-logcat.txt` holds the `PadScript`
lines.

| Call | App reply (round trip) | On the console |
|---|---|---|
| `wait:50` probes while connecting | `not streaming (StreamStateConnecting)` x3, then `ok 0/0` about 4 s after `am start` | |
| `pad.sh ps` | `ok 1/1 presses in 202 ms` (330 ms) | the control centre opens over the game: `live-1-ps-control-centre.jpg` |
| `pad.sh right*2` | `ok 2/2 presses in 402 ms` (532 ms) | focus moves two cards, Official news to GT7: `live-2-right-x2.jpg` |
| `pad.sh circle` | `ok 1/1 presses in 201 ms` (317 ms) | the control centre closes and the game is back: `live-3-circle-closed.jpg` |
| `input keyevent 4` (BACK), then a probe | `result=0` | the stream ended and the receiver is gone with the activity |

### Found and fixed on the device

The first live attempt was answered by a stopped diagnostics-preview
`StreamActivity` left in the back stack (`not streaming (StreamStateIdle)`),
because the phone registered the receiver from `onCreate` to `onDestroy`. It now
registers only while resumed, as the Go activity already did. The last probe in
the table above is the proof.
