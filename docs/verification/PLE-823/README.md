# PLE-823: a stick-only change inside the state minimum goes out as the minimum ends

The fix (`feedback_sender_wait_locked` in `lib/src/feedbacksender.c` takes the sender's
timeout afresh each time it wakes) is committed without any of the logging below. The S22
numbers come from two debug builds carrying a temporary log line, `instrumentation.patch`:
one with the fix, one with the fix removed. Neither build is committed.

**Build base:** both builds are `ed3e2eb7` (the fix, on `fork/android-port` at `e1fbcbd9`)
plus `instrumentation.patch`; the fix-removed build also reverses the commit's
`lib/src/feedbacksender.c` change.

## Host: chiaki-unit `/chiaki/feedback_sender`

A real sender on a real takion, sending to PLE-490's loopback console. The console now
decrypts state packets to read the left stick.

| build | stick-only move 1-7 ms after a state packet → next state packet | three samples inside the minimum → packet with the last one |
| --- | --- | --- |
| with the fix | 7.4-8.9 ms after the previous state packet, carrying the move (one run with timings logged) | one packet, 8.3 ms after the state packet, carrying the last sample, nothing after it |
| fix removed | 199.3 ms after the move: both new cases fail | 195.1 ms after the last sample |

40 of 40 runs passed at host load 20-26, and 30 of 30 with 16 extra CPU spinners (load 34).
Over those 30 runs, 34 of the 274 tries were retries. In the worst try the host ran the move
19 ms after the state packet instead of 4, and the packet left 68.9 ms after the move. So the
suite's per-try hard bound moved from 100 to 150 ms. A stale deadline never takes less than
192 ms.

## S22: live stream to PS5-466, 40 left-stick sweeps per build

* **Setup:** `SM-S908B`, serial `192.168.1.105:5555`. Stream to PS5-466 (192.168.1.164) with
  the phone's stored prefs, except **motion off** (`motion_enabled=false`).
  * With motion on (the default), the sensors change the controller state about every
    1.3 ms. Each change wakes the sender, so a stuck change waits at most about 1.3 ms past
    the minimum, and the defect barely shows.
  * Motion off is the case it hurts: a gamepad or the touch sticks, with nothing else moving.
* **Session:** `session.sh`, one Samsung lease per build, 2026-09-27 15:50 and 15:55 UTC.
  * It backs up the app state, runs `install -r`, and writes only the `motion_enabled` entry
    into the prefs. It then taps Connect.
  * Afterwards it puts the original prefs back, checked byte for byte.
* **Stimulus:** `stim.sh` sends 40 sweeps with `input joystick swipe`: 0 → 0.15 → 0 on the
  left stick, 60 ms each way, 0.5 s apart. There is no touch on the screen.
  * Android delivers a sweep's moves about once a frame (16.7 ms here). The final sample, the
    swipe's UP event, comes 1-3 ms after the last move's state packet, inside the minimum.
  * 15 % is under the PS5 menus' stick threshold.
* **Metric:** one log line per state packet. It gives the sequence number and the left stick
  the packet carried. When a stick change was waiting, it also gives how long after the last
  and the first unsent change the packet went out. `analyze.py` summarises the lines into
  `analysis.txt`; the raw lines are in `unfixed-state.txt` and `fixed-state.txt`.

| build (sha256 prefix) | stick back at centre: change → sent, p50 / p95 / max | far end of the sweep sent | longest any change waited | state packets during the sweeps |
| --- | --- | --- | --- | --- |
| fix removed (`969e5f24`) | 197.9 / 199.2 / 199.4 ms | 0 of 40 | 199.4 ms | 417 |
| with the fix (`1a32be5d`) | 5.6 / 6.6 / 8.0 ms | 40 of 40 | 8.0 ms | 500 |

* **Without the fix:**
  * The stick's return to centre waited for the 200 ms keepalive in all 40 sweeps, and rode
    on it. The console saw the stick still deflected for about 200 ms after it was released.
  * The sweep's far end (4915) was stuck 80-115 ms, until the next sweep's first move
    replaced it. It never reached the console.
* **With the fix:** both samples went out as the minimum ended, at most 8.0 ms after the
  change.
* **Packet count:** 83 more state packets over the 40 sweeps, about 2 per sweep: the two
  samples that used to be replaced or carried by a keepalive now get their own packet. That
  is still at most one per 8 ms minimum.
* **Sequence numbers:** they ran without a gap in both sessions (53-469 and 55-554). Only
  state packets were sent: the sweeps queue no history packets.
* **Console:** the stream kept running through both sessions, and no session quit.
  `stream_start.png` and `stream_end.png` show the console screen before and after the sweeps.
  Focus stayed on Settings → Accessibility → Display and Sound → High Contrast in both.
* **Raw artifacts, outside git:** `/home/wnt/gta6/build/ple823/{unfixed,fixed}/`. Each holds
  `logcat.txt`, `session.txt`, the app-state backup log, screencaps and the prefs copies
  (mode 600: they hold the PSN account id).
