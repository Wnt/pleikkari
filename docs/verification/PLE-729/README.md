# PLE-729: the Go's scripted auto-connect (`go_stream.sh start` + `auto_connect_host`)

Oculus Go 192.168.1.202:5555, serial 1KWPH802EW8203, Android 7.1.1, against PS5-466 (192.168.1.164,
`ready` in `ps5-discover.py`). Every session ran under `device.py run --resource go PLE-729` through
`go-autoconnect.sh` (this folder). The script backs up prefs, databases, app data and base.apk
(PLE-654's `go-live.sh setup`) and uses `install -r` only. At the end it puts the snapshot APK and
the prefs back byte-identically, and the Go is back at vrshell. Excerpts are in `evidence.txt` and
`session-s1/2/3.txt`. The raw logcats and screencaps are in
`/home/wnt/gta6/build/captures/ple729/`.

## Bisect result: no commit broke it

The route works at both ends of PLE-659..HEAD, and at the builds in between that PLE-618 and
PLE-702 used:

| Build | Where | `go_stream.sh start --path native` |
|---|---|---|
| PLE-618's builds (with PLE-659) | 03:02–03:06 UTC, `wt/ple-618-ws/build/` | live (tv 9 s, native) |
| PLE-702's own build (before PLE-690) | 07:57 UTC, `build/ple-702/` | live, 8 s |
| ce8708c9, the operator's `op-latest` (PLE-690 and PLE-717) | s1 `resident-native` | live, 7 s, cinema entered, 7 `Cinema video:`, screen_check picture |
| b4e10346, fork/android-port HEAD, unchanged | s1 `head-native` | live, 9 s, cinema entered, 7 `Cinema video:`, screen_check picture |

PLE-691 stalled on MainActivity because the APK on the Go was **f05b5dec**. It is the snapshot
that PLE-654 took at 02:03 UTC, before PLE-659 landed at 02:34, and that later Go sessions
restored (`~/.config/pleikkari/backups/go-ple654-before-20260927T0203Z/base.apk`). Most
later backups hold it too: PLE-663 at 03:38, PLE-702 at 07:28, PLE-698 at 08:30 and PLE-700/701
at 10:02–10:04. That lasted until the operator installed `op-latest` at about 10:23. f05b5dec's dex has
no `auto_connect_host` (`unzip -p … 'classes*.dex' | grep -ac` gives 0), so it ignores the extra.
PLE-691 read "installed 09:32" as a current build, but that install was PLE-663 putting the
snapshot back.

A second defect is in `go_stream.sh` itself (workspace repo). `prefs_edit` took an absent
`stream_go_vr_enabled` as false. Since PLE-689 that pref defaults on on the Go, so on fresh prefs
`--path tv` streamed in the cinema while the script waited for `StreamActivity`. Session s1
`head-tv-fresh` shows it: the run went live (`VrApi cinema entered`, 7 `Cinema video:`, 37
`Feedback stats`), `StreamVrActivity` was resumed, and the script still failed with "no frames 45s
… is the build older than PLE-659".

## Fixes

* **Workspace, `scripts/dev/go-latency/go_stream.sh`:**
  * `start` refuses an installed build without the extra before it changes anything. It pulls
    base.apk once per md5 and caches the verdict.
  * The tv path always writes `stream_go_vr_enabled=false`.
  * A timeout says how far the app's `AutoConnect` lines got.
  * `test_go_stream.py` covers all three.
* **App (this branch):**
  * Debug builds log the extra under `AutoConnect`: one line when it is taken, one when the
    console is listed as registered and the connect starts.
  * Home's pending auto-play (`PendingAutoPlay`) and the console-list join (`joinDisplayHosts`)
    are pulled out unchanged. `PendingAutoPlayTest` covers the extra reaching a connect, with
    PS5-466's discovery id and the Go's byte-reversed `server_mac` 207636924143040.

## Proof with this branch (429d8a51) and the fixed `go_stream.sh`, session s2

* **`mine-native-fresh`:** fresh prefs (`prefs-fresh.xml`, no `stream_go_vr_enabled`), then
  `go_stream.sh start --path native`.
  * `live: StreamVrActivity on display 0 (native), frames after 8 s`.
  * The `AutoConnect` lines show `auto_connect_host 192.168.1.164: streaming as soon as …`, then
    `192.168.1.164 is listed as registered console PS5-466: connecting`.
  * `GoCinema: VrApi cinema entered`. `Cinema video:` reached 299 frames in 5 s (59.7 fps), with
    360 of 360 submits showing video.
  * 30 `Feedback stats` lines, 0 FATAL.
  * The screencap was taken with `debug.pleikkari.vr_full_pose=1` (set back to 0 by `stop`).
    `screen_check.py` says `mean luma 13.3, max 235, 14.1 % of the frame lit: picture`, and it
    shows the PS5 home screen in both eyes.
* **`mine-library` (PLE-690's Library VR entry, still passing):**
  * The MAIN/INFO `GoVrLibraryEntry` intent was handed to vrshell's desktop (uid 1000). The logs
    show `GoVrEntry: Picked PS5-466: the only of 1 linked console(s)`, then
    `Connecting to PS5-466 at 192.168.1.164 (awake)`.
  * `VrApi cinema entered`, 7 `Cinema video:` lines.
  * `screen_check.py`: picture.
* **`mine-tv-fresh`:** fresh prefs, `--path tv`.
  * `live: StreamActivity on display 210 (tv), frames after 9 s`, with both `AutoConnect` lines.
  * The screencap is 0 B. That is expected: vrshell's layer is secure while the panel hosts the
    app (PLE-600).

Session s3, `old-refused`: with f05b5dec installed, `go_stream.sh start --path native` exits in
3 s with "has no auto_connect_host in its dex: it predates PLE-659 …". The logcat shows no
`START u0` and no prefs were written. The snapshot APK went back afterwards.

## Notes

* `go-autoconnect.sh`'s own `installed_apk` label said `auto_connect_host=no` for every APK in
  s1 and s2. The cause is `grep -q` behind `unzip` under `pipefail`: grep's early exit SIGPIPEs
  unzip (exit 141). It now uses `grep -c`. The runs are unaffected.
* Session s1 ran from a helper whose queue wait outlived its `DEADLINE`. `<out>.session-s` gave
  it a fresh deadline when the lease arrived.
