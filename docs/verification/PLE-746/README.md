# PLE-746: the Go's input-to-photon probe, live against PS5-466

The baseline these sessions produced, and the ranked lag producers, are in the workspace repo:
`docs/latency/go-input-to-photon/baseline-2026-09-27/summary.md` (with `presses.csv` and `frames.csv`).
This file records what ran on the Go.

* **Device:** Oculus Go `1KWPH802EW8203`, `oculus/vr_pacific/pacific:7.1.1/NGI77B/20008400174500000`.
* **Console:** PS5-466, 192.168.1.164, `ready` in `scripts/dev/ps5-discover.py`.
* **APK:** the gate's arm64 debug build of `1f822412` (the same change is `b0fb10ad` on the rebased branch), installed with
  `install -r` over `go-live.sh setup`'s backup.
* **Harness:** `go-probe.sh` (this directory), one `device.py run --resource go` lease per session.
* **Raw data:** on CT950 under `/home/wnt/gta6/build/ple746/` (not committed).

| session (UTC) | what | result |
|---|---|---|
| `s1-look-124754` 12:49 | Library launch, stream, screencap | streaming in 6 s; the probe wrote `frames.csv` with luma; PS5 on its home screen |
| `s2-look-125026` 12:52 | pad `up right right right` | focus to the Settings gear, one screencap per step |
| `s3-look-125313` 12:55 | pad `cross`, `down` x5 | Settings open; the probe logged the pad's Cross (state set -> history packet sent 0.35 ms) |
| `s4-look-125655`, `s5-look-125904` 12:57-13:00 | pad to Accessibility -> Display and Sound -> Invert Colours | cursor parked on Invert Colours |
| `r1-rounds-130039` 13:01-13:06 | 3 rounds x 32 `input keyevent KEYCODE_BUTTON_A`, atrace each | 96 presses sent, 15,316 frames; **luma never moved (35-52)**: Invert Colours is not in the Remote Play video (screencap: toggle on, picture not inverted) |
| `s7-explore-131537` 13:16 | pad: Invert off, High Contrast on/off, Colour Correction open/close | High Contrast moves the picture's luma 40 <-> 21 with a ~200 ms fade: the stimulus |
| `b1-rounds-132644` 13:27-13:33 | 3 rounds x 32 `input keyevent` on High Contrast, atrace each | 36 of 96 presses flipped the picture (injected down/up ~1 ms apart); 19,360 frames; round 3 ended at 13:32:27 by VrApi's `external_temp_warning` |
| `b2-rounds-134214` 13:42 | the rebased build, pad presses | refused: `ClearActivity` "kept for the user". The Go was at **7 % battery, not on a charger** (`USB powered: false`), with `low_battery` reposted every 10 s |

**Not run:** the rounds on the rebased build (PLE-715/722/730 landed during the sessions), with
pad presses. The Go could not start an activity until charged.

**Restored after every session:** the snapshot prefs, byte-identical, and
`debug.pleikkari.vr_full_pose=0`. The Go was left on `ClearActivity` (the low-battery state),
not on vrshell's home.

**APK restored:** at 13:57 UTC, `go-probe.sh … restore` with `BACKUP=/home/wnt/gta6/build/ple746/backup2`
reinstalled the snapshot APK the Go had at 13:42 UTC and put its prefs back byte-identically.

## PLE-803: faster stimuli (`STIMULUS=`)

High Contrast costs about 190 ms of the 265 ms p50 per press in the PS5's own Settings UI and its fade.
`go-probe.sh` now takes `STIMULUS=`:

* `high-contrast` (default, the fallback): Cross on the High Contrast toggle, as above.
* `create`: the Create button (the debug pad's new `create`), on any screen; the Create menu opens over
  a dimmed picture, and the next press closes it.
* `home-focus`: D-pad right then left on the PS5 home screen, between two game tiles.

The script sets `debug.pleikkari.probe_buttons` (a chiaki button mask) for the session, so the
probe's `presses.csv` times that button instead of Cross, and clears it at the end.
`input_to_photon.py` needs no change for `create`. Its `MIN_STEP` of 10 luma levels may be too
large for `home-focus`, whose art change depends on the installed games. Check `frames.csv` first.

**Not measured yet.** Press → first changed frame p50/p95 for `create` and `home-focus` is open. At
14:40 UTC 2026-09-27 the Go was still at 7 % with no charger (`dumpsys battery`: AC/USB powered
false), so no session ran. Run `STIMULUS=create PRESSES=32 … go-probe.sh <out> <apk> rounds`, then
`input_to_photon.py`, and set it against High Contrast's 265.5 ms p50 / 378.4 ms p95.
