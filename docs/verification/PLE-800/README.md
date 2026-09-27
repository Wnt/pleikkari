# PLE-800: feedback history packets no longer wait out the state throttle

The fix (`state_cond_check` in `lib/src/feedbacksender.c` ends the sender's wait when a
history packet is queued) is committed without any of the logging below. The S22 numbers
come from two debug builds carrying a temporary log line, `instrumentation.patch`: one with
the fix, one with the fix removed. Neither build is committed or left on the phone.

**Build base:** both builds, and the tests below, ran on the fix's first commit `230945c8`, on
`436c6bf8`. The branch was then rebased over PLE-746's landing (`5d1b7a7e`) to `07f5b02c`,
with the same change. PLE-746 adds trace calls to the same sender, so `instrumentation.patch`
applies to `230945c8` only.

## Host: chiaki-unit `/chiaki/feedback_sender`

A real sender on a real takion, sending to PLE-490's loopback console.

| build | press 1-7 ms after a state packet → history packet at the console |
| --- | --- |
| with the fix | 90-955 µs over two runs; one try took 1,264 µs and passed on its retry (109 µs) |
| fix removed | 199,079 µs: the sender slept to its keepalive deadline; both cases fail |

On a CT950 loaded to 41 (20 spinners on 10 cores), 34 of 34 runs passed. Over 313 presses:
106 µs p50, 1.6 ms p95, 6.0 ms max. That scheduling noise is why each press gets up to 10
tries in the test.

## S22: live stream to PS5-466, 100 R3 presses per build

* **Setup:** `SM-S908B`, serial `192.168.1.105:5555`, on battery at 81 %. Stream to PS5-466
  (192.168.1.164) with the phone's stored prefs; motion is on by default.
* **Session:** `session.sh`, one Samsung lease per build, 2026-09-27 14:40 and 14:42 UTC. It
  backs up the app state, runs `install -r`, and taps Connect. `stim.sh` then sends R3
  (keycode 107) 100 times, each held 80 ms, so every press is a down and an up history packet.
* **Console:** its screen stayed on Settings → Accessibility → Display and Sound. R3 does
  nothing there.
* **Metric:** the log line gives each history packet's time from the controller state that
  queued it to its send (`queued_to_sent_us`), and how long after the last state packet it was
  queued. `analyze.py` summarises it into `analysis.txt`. The raw lines are in
  `fixed-history.txt` and `unfixed-history.txt`.

| build (sha256 prefix) | packets | queued inside the 8 ms state minimum | queued → sent p50 / p95 / max | ≥ 1 ms |
| --- | --- | --- | --- | --- |
| fix removed (`2d85b807`) | 200 | 187 | 4,669 / 9,629 / 12,553 µs | 172 |
| with the fix (`848a1a3f`) | 200 | 184 | 214 / 527 / 1,041 µs | 1 |

* **Why most presses land inside the minimum:** with motion on, the phone's sensors change the
  controller state every frame, so state packets flow continuously. A press is queued a median
  4 ms after the last one.
* **Without the fix:** the history packet waited for the throttle to open, and 199 of 200 went
  out in the same pass as the next state packet.
* **With the fix:** every one of the 184 packets queued inside the minimum was sent within
  685 µs.
* **The one packet over 1 ms (1,041 µs):** it was queued 8.1 ms after a state packet, outside
  the minimum. It went out in the same pass as a state packet, which the sender sends first,
  as before.
* **Sequence numbers:** history numbers ran 0-199 without a gap in both sessions.
* **Afterwards:** the phone was left on a clean `assembleDebug` of `230945c8`, the pre-rebase fix.
* **Raw artifacts, outside git:** `/home/wnt/gta6/build/ple800/{fixed,unfixed}/`. Each holds
  `logcat.txt`, `session.txt`, the app-state backup log and screencaps; `stream_end.png` shows
  the live stream.
