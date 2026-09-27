# PLE-385 — the PS5 AvCap wedge (`InitResult:-6`/`-11`)

**Bottom line up front: the console is streaming normally.** The last trial of
this run (`Z-final-confirm`, 2026-09-20 23:44:32Z) connected and streamed;
`ps5-discover.py` reports `192.168.1.164:9302 PS5 PS5-466 … ready`; the
impairment rig is back to `active_profile=clean`; the phone's prefs are
byte-identical to the backup taken before the run.

**The headline result is a negative one: 32 valid trials (34 rows in
`results.tsv`; the two `C-kill` rows are invalid harness runs) and the wedge
never occurred once.** No sequence of client-side abuse reproduced it — not a
force-stop mid-stream, not a force-stop mid-handshake, not one with the console
link blacked out, not a codec switch between sessions, not rapid churn. See
[Rule](#the-rule-such-as-it-is).

## 1. What was already known (survey, done before touching the device)

Three recorded occurrences, all inside `capture.sh`-style impairment runs on the
night of 2026-09-17/18:

| When | Ticket | What preceded it | How it ended |
|---|---|---|---|
| 2026-09-18 ~00:0x | PLE-356 | `capture.sh` cleanup did `keyevent 4` (which only raises the confirm dialog) then `am force-stop` — the session died with no Disconnect | survived 9 connect attempts, a wake packet and both quality presets; cleared on its own after ~40 min |
| 2026-09-18 00:48 | PLE-357 | **no force-stop at all** — PLE-356 had exited cleanly via Back | one bare retry ~1 min later succeeded |
| 2026-09-18 ~00:2x | PLE-367 attempt 1 | a real `InitResult:-11`, hit while reworking the cleanup path | the harness's 60 s wait + relaunch fired (and then failed for an unrelated reason — the pre-fix tap-based overlay reveal) |

What they have in common, from the logs rather than the prose:

* **It is entirely console-side.** `build/captures/ple356-probe/session_logcat.txt`
  shows the client reaching `Session request successful`, `Ctrl received Login
  message: success`, a full Senkusha (RTT 4.75 ms, MTU 1454 both ways) and
  `StreamConnection successfully received bang` — and then, 2.1 s later, the
  **console** sends the disconnect carrying `Nagare did not init! AvCap failed
  to initialize video: [InitResult:-6]`, followed by `StreamConnection didn't
  receive streaminfo`. Nothing client-side fails first. There is no client-side
  defect here to fix, only a console behaviour to characterise.
* **The failure window is exactly "after bang, instead of streaminfo"** — the
  point where the console has accepted the session and is starting its AV
  capture.
* The recovery times recorded differ by two orders of magnitude (~1 min for
  PLE-357, ~40 min for PLE-356), which already suggests "wait N seconds" was
  never a single number.
* Force-stop is **not** necessary (PLE-357 had none) and, as of this run, not
  sufficient either.
* All three happened during impairment capture runs, which is why impairment was
  included as a trigger candidate below.

Not to be confused with two neighbours that look similar in a log:
`session_request_rp_in_use` ("Remote Play on Console is already in use",
PLE-428) and PLE-423's total-network-loss client wedge. Both were observed in
this run and neither is the AvCap wedge.

## 2. What was tried

`wedge-probe.sh` in this directory drives one trial: connect → stream
`STREAM_SECS` → leave by an exit mode → wait `WAIT_SECS` → connect again, and
classifies the second connect as `OK` / `AVCAP:<code>` / `RP_IN_USE` /
`QUIT:<reason>`. Since PLE-452 the failure classes come from the shared
`classify_connect_failure()` in `../lib/capture-guard.sh`, so new rows read
`AVCAP_WEDGE:<code>` / `OTHER_QUIT:<reason>` (the committed `results.tsv` keeps
the old labels), and an AvCap failure only counts after the bang. Raw logcat for both connects, screenshots and the results table
are under `build/captures/ple385/` (gitignored); `results.tsv` is copied here.

| Group | Provocation | n | Second connect |
|---|---|---|---|
| A | clean exit (BACK → `finish()`), 10 s | 1 | OK |
| B | `am force-stop` mid-stream, 10 s / 0 s | 2 | OK |
| C | `kill -9` mid-stream | 2 | **invalid** — `adb shell` cannot kill another uid's process; the app kept running (same pid). Mode removed. |
| D/E/F | `am force-stop` 2 s / 3 s / 5 s after tapping Connect (mid-handshake), 0–5 s | 7 | 6× OK, 1× `RP_IN_USE` (cleared by itself within 15 s) |
| G | impaired stream (`blip-200ms`; then `custom --loss 100%` for 30 s, i.e. a total blackout with `video received 0`, `takion 0/s`) then force-stop **while blacked out**, clean, 10 s | 2 | OK |
| H | rapid churn: stream 8 s → force-stop → reconnect immediately, 6× back to back | 6 | OK |
| I | link cut entirely (`loss 100%`) inside the handshake, then force-stop. I1/I3 landed in the exact wedge window — `StreamConnection successfully received bang` then `didn't receive streaminfo` — leaving the console initialising AvCap with no client at all | 4 | OK |
| J | same as B but with the client on h264/30 fps, the settings in force during all three recorded occurrences | 3 | OK |
| K | codec switched between the two connects (h265→h264, h264→h265), so the console must re-init AvCap for a different codec | 2 | OK |

Plus `baseline`, two post-`RP_IN_USE` polls and `Z-final-confirm`.

## 3. The rule, such as it is

**We tried A through K — 32 trials — and the wedge occurred after none of them.** The honest
statement is:

* **There is no evidence that force-stopping the app causes the wedge.** 13
  force-stops in this run — mid-stream, mid-handshake, under total packet loss,
  back-to-back — every one followed by a successful connect, several with zero
  wait. Combined with PLE-357's occurrence *without* any force-stop, the folklore
  rule ("a force-stop risks the wedge") is not supported by anything measured.
  Workers should stop navigating around it; a force-stop is a normal, cheap
  thing to do to this app.
* **The wedge's trigger remains unknown.** It is console-side, it lands in the
  bang→streaminfo window, and nothing a client can do provoked it in 32 trials.
  Something about the console's own state (the last three occurrences all came
  after a long evening of impairment captures, in the small hours) is the
  remaining candidate space; it cannot be probed from the client.
* **Recovery: retry first, don't wait 40 minutes.** The only two documented
  recoveries are ~1 min (one bare retry, PLE-357) and ~40 min (PLE-356). Nothing
  a client did shortened either. A connect-retry loop — which
  `docs/verification/PLE-356/capture.sh` already has — is the whole mitigation.
* The near-miss worth knowing is the *other* failure: force-stopping
  mid-handshake produced `session_request_rp_in_use` once (D1). It looks alarming
  in a log and is **not** this wedge; it cleared by itself inside 15 s.

No mitigation is proposed. There is nothing on the client to change, and one
non-reproduction is not grounds to touch the connect path (explicitly out of
scope for this ticket).

## 4. Reproducing this

```sh
OUT_DIR=build/captures/ple385-rerun \
  scripts/dev/device.py run PLE-385 -- \
  docs/verification/PLE-385/wedge-probe.sh <label> <exit-mode> <wait-secs>
```

Exit modes: `clean`, `forcestop`, `earlystop`, `blackout-handshake`, `none`
(poll only). Optional env: `STREAM_SECS`, `EARLY_SECS`, `BLACKOUT_DELAY`,
`IMPAIR_PROFILE`/`IMPAIR_ARGS`, `SWITCH_CODEC`/`SWITCH_FPS`, `CONNECT_TIMEOUT`.
Exit status is 0 for a streaming second connect, 10 for the AvCap wedge, 11 for
`rp_in_use`, 12 for any other session quit, 13 for a harness failure — so it can
be used as a wedge detector by a future ticket.

The script never uninstalls or `pm clear`s anything; `SWITCH_CODEC` edits only
`stream_codec`/`stream_fps` in the app's own prefs via `run-as`, with the app
stopped. Back up prefs (`app-state.sh backup`, plus a copy of the prefs XML)
before using it, as this run did.
