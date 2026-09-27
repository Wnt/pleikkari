# Local Oculus Mobile SDK for PLE-602

This directory is intentionally empty except for this README. Obtain a **Go-compatible
legacy Oculus Mobile SDK** through the operator's Pleikkari organization at
[Meta developer downloads](https://developers.meta.com/horizon/downloads/), accepting
its license there. Use **1.35.0** (v18, API 1.35): it matches the Go's VrApi runtime
(`com.oculus.systemdriver` 18.0.0.x) and its release notes name the Go, while 1.50.0
(v33) no longer defines `VRAPI_DEVICE_TYPE_OCULUSGO` (PLE-617). On CT950 it is stored
outside git at `/home/wnt/gta6/build/sdk/ovr_sdk_mobile/`; link it into a worktree with
`ln -s /home/wnt/gta6/build/sdk/ovr_sdk_mobile/VrApi third_party/ovr_sdk_mobile/VrApi`.
Modern OpenXR SDKs cannot replace VrApi on the Go. Do not download an unofficial binary
mirror or commit the SDK, its headers, libraries, archives, or license-protected samples.

Extract the SDK here, with no extra enclosing version directory:

```
third_party/ovr_sdk_mobile/
  README.md
  VrApi/Include/VrApi.h
  VrApi/Include/VrApi_Helpers.h
  VrApi/Include/VrApi_Input.h
  VrApi/Libs/Android/arm64-v8a/Release/libvrapi.so
```

Gradle checks the above files. Absent SDK: no VR activity, manifest overlay, JNI
library, or VrApi dependency in the APK. A partial extraction fails configuration
with an explanation. `-PchiakiGoVr=true` requires a complete SDK;
`-PchiakiGoVr=false` explicitly builds the baseline even if it is installed.
The default `auto` includes support only if the SDK is complete. CMake builds
`pleikkari-vr` only for arm64-v8a and AGP packages its imported `libvrapi.so`.
The phone launcher, minSdk 24 and application metadata are unchanged.

From `android/`, with the Android SDK environment loaded:

```
./gradlew assembleDebug -PchiakiGoVr=true -PchiakiAbiFilters=arm64-v8a
```

**CI (PLE-741).** `.github/workflows/build-android.yml` builds this variant too, but only on
pushes to `android-port`. Its job `build-android-go-vr` fetches the SDK subset (a pinned,
checksummed bundle) from the private workspace repo with a read-only deploy key
(`.github/scripts/fetch-ovr-sdk.py`, secret `OVR_SDK_DEPLOY_KEY`). It links the subset
here and fails unless `.github/scripts/check-go-vr-apk.sh` finds both libraries and
`StreamVrActivity` in the APK. The APK is published as the artifact `pleikkari-android-go-vr`
and as `pleikkari-android-go-vr.apk` on the `android-latest` prerelease. The SDK files
themselves stay in the private repo. The workspace's `docs/OCULUS-GO.md` explains the key,
how to rotate or revoke it, and how to refresh the bundle.

The regular gate without the SDK still compiles the VR **Kotlin** in the unit-test
source set (not the APK). It cannot compile or link `src/vr/cpp/vr-cinema.cpp`.
A passing SDK-absent gate is **not evidence that the native integration compiles**.
Go VR and the PSN mock build use mutually exclusive manifest overlays; use
`-PchiakiGoVr=false` for a mock build.

## Using the experiment

On an arm64 Oculus Go (`Build.DEVICE == "pacific"`, API 25+), Settings offers
**Native VR cinema (Oculus Go)** only when both native libraries load. It defaults
to on on the Go since PLE-689 (off everywhere else). Both LAN and opted-in PSN Connect paths route through the same selection;
phones always open the original `StreamActivity`, even with an imported true
preference. Turning the setting off restores the existing Oculus TV stream.

**Library launch (PLE-690).** With the setting on, launching Pleikkari from the Go
Library goes straight into the cinema, with no 2D panel step. The vr overlay declares an
`activity-alias` of `StreamVrActivity`, `.stream.GoVrLibraryEntry`, with `MAIN` + `INFO` +
`com.oculus.intent.category.VR`. vrshell's `PackageUtil.isVrApp()` accepts that category as
well as application-level `vr_only`/`dual` meta-data, so a VR app launch runs
`ShellApplication.sendLaunchIntent()`. That starts `getLaunchIntentForPackage()`, which prefers
`MAIN`/`INFO` over `MainActivity`'s `LAUNCHER`. The alias ships `enabled="false"`.
`GoVrSupport.syncLibraryEntry` enables it at app start, and when the setting changes, only on
an eligible Go with the setting on, so no phone ever resolves it. The package manager keeps that
state across `install -r`, but a fresh install needs one app start (the Library's 2D launch
into Oculus TV) before the Library treats the app as VR. `StreamVrActivity` has its own task
affinity (`<applicationId>.vr`), so a Library launch never joins a `MainActivity` task that
Oculus TV hosts.

A Library launch drops every intent extra (the alias is exported) and enters the cinema at
once. `GoVrLibraryFlow` reads the linked consoles and picks the last used one
(`last_console_mac`, written by every Connect), or the only one. It finds the console with LAN
discovery and wakes it from rest mode, then streams on the same `StreamViewModel`/`StreamSession`
path as Connect. With several consoles and none used last it shows an in-VR chooser. With none
linked it shows a message. Both have a row that opens `MainActivity` on the Oculus TV screen
(the Library's 2D launch intent, `GoVrSupport.panelIntent`) for linking and Settings. The touchpad
thirds move and choose; a Bluetooth pad uses the D-pad, A and B. Logcat tag `GoVrEntry`.

Settings stay reachable with one console too. A click while the app looks for or wakes the
console opens the chooser, and so does a Disconnect from the cinema menu: a Library launch
restarts `StreamVrActivity` (not exported) with its `library_chooser` extra instead of leaving.
There, the menu's right third reads Exit. For a proof on the Go with no console connect (the
operator's PS5 hold), a debug build honours `adb shell setprop debug.pleikkari.go_entry_choose 1`:
the Library launch logs the console it would stream and opens the chooser instead. Nobody clicks
it, so nothing connects. Set it back to 0 afterwards.

The cinema uses the existing `StreamViewModel`, `StreamSession`, `StreamInput`,
codec configuration and audio. The decoder writes to an external-OES
`SurfaceTexture`. A dedicated thread owns EGL, VrApi and that texture. It renders
one 80-degree curved screen, 3 metres away, in both eye swapchains and submits a
projection layer every panel interval, requesting 72 Hz. This is mono video in VR;
no stereo conversion or optional side-by-side mode is implemented. SurfaceTexture's
crop/flip transform is applied before sampling. The chosen projection path has an
extra eye render pass; it is not the native cylinder compositor optimization.

PLE-603 adds the room around the screen: the Go-only settings under **VR cinema
(Oculus Go)** pick an environment (plain, the default, keeps the path above unchanged;
void, cinema hall, night terrace) and the screen's distance, width, curve radius and
height. `libpleikkari-vr.so` links `libpleikkari-vr-environment.so` (plain GLES 3.0,
built in every gate) for it; see `docs/design/vr-environments.md` §7. `vr-cinema.cpp`,
with PLE-603's environment and PLE-615's MSAA wired in, compiles and links against the
real SDK 1.35.0 headers and `libvrapi.so`, and has no warnings under `-Wall -Wextra`
against either 1.35.0 or 1.50.0 (PLE-623). The stub it was first type-checked against
did not differ from the real declarations.

The screen remains world-locked. Go touchpad click recentres it. Go Back opens a
head-following menu: click the left/centre/right third of the touchpad for
Resume/Recentre/Disconnect. The Bluetooth pad retains `StreamInput` unchanged.
Connecting and failure messages are drawn inside VR. A console login PIN currently
requires returning to Oculus TV; the cinema displays this instruction. Initialization
failure logs `GoCinema`, shows an explanation and returns to the existing task,
without silently reconnecting or changing the saved preference.

Lifecycle ordering: resume plus a valid window starts VR; pause or surface change
stops/detaches the decoder, waits for the rendering thread to release its consumer
and leave VR, then returns the window to Android. Nothing uses GLSurfaceView.
[Meta's lifecycle documentation](https://developers.meta.com/horizon/documentation/native/android/mobile-vrapi/)
explicitly rules out GLSurfaceView for VrApi. Go input uses
[VrApi's input API](https://developers.meta.com/horizon/documentation/native/android/mobile-vrapi-input-api/).

## Debug preview: the cinema with no console

A debug build runs the real VrApi cinema without a PS5 or the setting (PLE-623):
`StreamVrActivity` with `--ez vr_cinema_preview true` skips the session and queues a
moving 1280x720 test picture into the decoder's `SurfaceTexture` at 60 fps, so the
picture, the environment's glow map and `Environment frame:` run as in a stream.
`--es environment plain|void|cinema|terrace` overrides the stored environment for that
run, `--ei environment_msaa 1` turns the rooms' 4x MSAA off (PLE-653), and
`--ei sky_variant N` draws a PLE-650 debug sky in the void or terrace (PLE-666); no
preference is written. The activity is not exported, so start it as the app's
own uid, with `--user 0` (`am`'s default `current` user needs a permission the app lacks):

```
adb -s 192.168.1.202:5555 shell run-as fi.madekivi.pleikkari am start --user 0 \
    -n fi.madekivi.pleikkari/.stream.StreamVrActivity --ez vr_cinema_preview true --es environment cinema
adb -s 192.168.1.202:5555 logcat -s GoCinema VrApi
```

`am force-stop fi.madekivi.pleikkari` ends it. A release build ignores the extra.

**A Go on a table screencaps the room's floor or sky, not the screen.** The screen sits
level, at the head's yaw, and the Go lies at about -87 degrees of pitch. In the void,
that view is near-black by design (luma mean 1.4, max 3). It looks exactly like a
broken cinema, and PLE-702 was filed as a black-eye-buffer regression because of it. For
a screencap that shows the picture, run `adb shell setprop debug.pleikkari.vr_full_pose 1`
before the cinema starts. That makes a debug build place the screen along the full head
pose (PLE-675). The workspace's `scripts/dev/go-latency/go_stream.sh --path native` sets it,
and `screen_check.py` fails a near-black native screencap. Every placement is logged:
`GoCinema: Screen placed at frame N (reason): head yaw Y, pitch P degrees, runtime recentres C[, full pose][; provisional, …]`.

The runtime recentres its LOCAL tracking space by itself. It does so once as it handles
`HMT was mounted` in the first frame's submit, again when a worn Go is put back on, and
on a long press of the Oculus button. `VRAPI_SYS_STATUS_RECENTER_COUNT` rises each time,
and since PLE-702 the cinema places the screen again after every rise (`runtime recentre`).
Before that, the first frame's placement survived the re-base, so the screen sat off by
however far the head had turned since the runtime's last recentre.

A placement made with the head steeper than 60 degrees is `provisional`, unless it uses the
full pose. The screen is placed again once the head has stayed within 30 degrees of level
for 36 frames (`head came level`). A stream started on the table places the screen at the
face-down Go's yaw. While `go-keepawake.sh` pins the proximity sensor, picking the Go up
fires no mount event, so before this the screen could stay behind the new wearer. In the
void or plain room that looks totally black (PLE-702). The decision is
`android/app/src/main/cpp/vr-screen-placement.c`, tested in chiaki-unit
(`/chiaki/vr_screen_placement`).
The Go's first results with it (every room 7.5 to 8.6 ms of GPU in VrApi, and
`Environment frame:` under-reporting that) are in `docs/design/vr-environments.md` §8.
The streaming cinema can be reached from adb too, through the real Connect path
(PLE-643, PLE-654). The Library path is no use for this: it opens `MainActivity` inside
the Oculus TV panel (PLE-600's `go.sh launch`), where `input tap` cannot reach it.
Instead:
1. Force-stop the app, and check that none of its activities remains on any display.
2. Write `stream_go_vr_enabled=true` into its prefs through `run-as`.
3. `am start -W` `MainActivity`, which opens on display 0.
4. `input tap` the enabled `playButton` found by `uiautomator dump`.
5. `StreamVrActivity` streams live. End it with `am force-stop`.

`docs/verification/PLE-654/go-live.sh` does all of this under the Go lease, with backup
and restore. Every 5 s the cinema logs `GoCinema: Cinema video: N decoder frames latched
in … s (… fps), X of Y submitted frames showed video`. Read it next to the session log's
`Feedback stats` `decoded`: on the Go about 4 % of decoded frames are never latched at
72 Hz, and 9-12 % at 60 Hz (`docs/verification/PLE-654.md`). PLE-623 once saw vrshell's
`ClearActivity` cover a display-0 `MainActivity`; seven PLE-654 arms did not.

PLE-698 times each video frame's own path through the cinema in software, **only with the stats
log on** (`stream_feedback_stats_log`). It does not show the PS5 or the network. Every second,
`chiaki-jni` logs a `Cinema latency:` line to logcat (tag `Chiaki`) and to the session log. The
line gives p50/p95/max/mean for each stage, all on `CLOCK_MONOTONIC`:
* arrival→decoded: from chiaki completing the frame to the decoder output being dequeued;
* decoded→latched: to the cinema's `updateTexImage`;
* latched→submitted: to its `vrapi_SubmitFrame2` call;
* submitted→predicted photon: to that submit's `vrapi_GetPredictedDisplayTime`;
* the total, from arrival to predicted photon.

The line also counts frames a later frame replaced before any latch. Frames are joined by their
SurfaceTexture buffer timestamp: `video-presenter.c` records it on release and the cinema reads
it back with `getTimestamp()`. At start the cinema logs `GoCinema: VrApi clock minus
CLOCK_MONOTONIC`. `scripts/dev/go-latency/run.sh native --start-stream` reports all of it;
`docs/verification/PLE-698.md` has the first Go numbers.

The debug preview times its synthetic picture the same way when the stats log is on, and the
cinema logs the line itself every second, because there is no session. With no decoder, every
latch is `unmatched`, so the preview gives only latched→submitted and submitted→predicted photon.
With `stream_go_vr_match_60hz` on, the preview runs the panel at 60 Hz, as a 60 fps stream would.

## Required device validation

Built with SDK 1.35.0 (PLE-617, PLE-623). A live PS5 stream plays through the cinema on
the Go, started from Connect with nobody in the headset (PLE-643, PLE-654). Headset
acceptance and latency were not run. Before treating this as a usable Go build:

1. Done (PLE-617, PLE-623): the SDK build packages arm64 `libvrapi.so`,
   `libpleikkari-vr.so` and `libpleikkari-vr-environment.so`, `vr-cinema.cpp` needed no
   API changes, and the gate passes with the SDK linked in.
2. Preserve registration: back up app data before any `adb install -r`; never
   uninstall or clear `fi.madekivi.pleikkari`. Launch from the headset Library,
   not `adb shell am start`. PLE-600 owns scripted Oculus TV launch.
3. With the setting off, verify the existing Oculus TV flow. Enable it, Connect,
   and capture Go serial `1KWPH802EW8203` / `192.168.1.202:5555`, logcat (`GoCinema`,
   VrApi, decoder and session messages), and `adb -s 192.168.1.202:5555 exec-out
   screencap -p`. Confirm entry from the Oculus TV task, actual 72 Hz, tracking,
   video orientation, controller mappings, sound, disconnect, headset sleep/wake,
   Home/resume, and repeated sessions without stale surfaces. Done headless (PLE-654,
   `docs/verification/PLE-654.md`): Connect enters the cinema, the panel holds 72 Hz
   (60 Hz with `stream_go_vr_match_60hz`), decoded frames are latched and drawn, and
   seven sessions, each in a fresh process, ran with no crash. Everything seen, heard or
   held in the headset still needs a person.
4. Library classification (PLE-609, then PLE-690): `vr_only` stays **activity-local**. Do not
   move it to the application node, which would classify the whole package, `MainActivity` and
   the phone launcher included (the installed `games.b4t.epicrollercoasters.oculus` does that
   and exposes a MAIN/INFO activity with no LAUNCHER). PLE-690 makes the Library launch VR in a
   different way: the vrshell dex (`com.oculus.vrshell.util.PackageUtil.isVrApp`) also accepts a
   `MAIN` + `com.oculus.intent.category.VR` activity, and the only such activity here is the
   disabled-by-default `GoVrLibraryEntry` alias described above. It has its own VrApi console
   picker (`GoVrLibraryFlow`).
5. PLE-601 owns operator-in-headset A/B latency rounds. Keep codec, stream profile,
   pacing preferences, network and pad identical; record the two paths and actual
   refresh cadence. Do not infer a latency improvement from removing Oculus TV.
