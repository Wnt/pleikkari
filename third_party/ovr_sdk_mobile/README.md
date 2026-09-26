# Local Oculus Mobile SDK for PLE-602

This directory is intentionally empty except for this README. Obtain a **Go-compatible
legacy Oculus Mobile SDK** through the operator's Pleikkari organization at
[Meta developer downloads](https://developers.meta.com/horizon/downloads/), accepting
its license there. The ticket targets the approximately v23 / 1.50 generation;
confirm Go support in the downloaded release's notes. Modern OpenXR SDKs cannot
replace VrApi on the Go. Do not download an unofficial binary mirror or commit the
SDK, its headers, libraries, archives, or license-protected samples.

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

The regular gate without the SDK still compiles the VR **Kotlin** in the unit-test
source set (not the APK). It cannot compile or link `src/vr/cpp/vr-cinema.cpp`.
A passing SDK-absent gate is **not evidence that the native integration compiles**.
Go VR and the PSN mock build use mutually exclusive manifest overlays; use
`-PchiakiGoVr=false` for a mock build.

## Using the experiment

On an arm64 Oculus Go (`Build.DEVICE == "pacific"`, API 25+), Settings offers
**Native VR cinema (Oculus Go)** only when both native libraries load. It defaults
to off. Both LAN and opted-in PSN Connect paths route through the same selection;
phones always open the original `StreamActivity`, even with an imported true
preference. Turning the setting off restores the existing Oculus TV stream.

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
built in every gate) for it; see `docs/design/vr-environments.md` §7. The wired
`vr-cinema.cpp` was type-checked against a local stub of the VrApi declarations, not
compiled against the SDK.

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

## Required device validation

The SDK was absent; SDK-enabled headset acceptance and latency were not run.
Before treating this as a usable Go build:

1. Build with the licensed Go SDK and inspect the APK for arm64 `libvrapi.so` and
   `libpleikkari-vr.so`. Resolve any API differences against that SDK, then run the
   full gate with it present. Also run `-PchiakiGoVr=false` to check the baseline.
2. Preserve registration: back up app data before any `adb install -r`; never
   uninstall or clear `fi.madekivi.pleikkari`. Launch from the headset Library,
   not `adb shell am start`. PLE-600 owns scripted Oculus TV launch.
3. With the setting off, verify the existing Oculus TV flow. Enable it, Connect,
   and capture Go serial `1KWPH802EW8203` / `192.168.1.202:5555`, logcat (`GoCinema`,
   VrApi, decoder and session messages), and `adb -s 192.168.1.202:5555 exec-out
   screencap -p`. Confirm entry from the Oculus TV task, actual 72 Hz, tracking,
   video orientation, controller mappings, sound, disconnect, headset sleep/wake,
   Home/resume, and repeated sessions without stale surfaces.
4. Library classification (PLE-609, resolved): Pleikkari stays a 2D app in the
   Go Library and opens in Oculus TV; the VR cinema is entered from Connect with
   the setting on. The Go's own VR apps are classified per package: the installed
   `games.b4t.epicrollercoasters.oculus` tags `vr_only` on the **application** node
   and exposes its activity as MAIN/INFO with no LAUNCHER (aapt2 dump of the APK
   pulled from Go `192.168.1.202:5555`, 2026-09-26). Doing the same here would
   launch `MainActivity` in VR mode, which breaks the Oculus TV path and the phone
   launcher, and a separate VR entry would need its own console picker in VrApi.
   So `vr_only` stays **activity-local**. Do not move it to the application node,
   and do not add a MAIN/INFO activity, unless a separate Go-only package or build
   variant owns them.
5. PLE-601 owns operator-in-headset A/B latency rounds. Keep codec, stream profile,
   pacing preferences, network and pad identical; record the two paths and actual
   refresh cadence. Do not infer a latency improvement from removing Oculus TV.
