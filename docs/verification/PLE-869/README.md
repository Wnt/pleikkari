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

**Not yet run:** presses reaching a live PS5 session. That needs the operator to
lift PS5-HOLD. Until then, the press path is proven only by host tests:
`PadScriptTest` (the grammar and the runner's order, release and cancel) and
`StreamInputScriptedTest` (the scripted layer merges with a physical pad's
state, and neither clears the other).
