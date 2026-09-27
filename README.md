> **Pleikkari** is an Android PS5 Remote Play client, forked from [chiaki-ng](https://github.com/streetpea/chiaki-ng) by Street Pea, itself a fork of [Chiaki](https://github.com/thestr4ng3r/chiaki) by Florian Märkl.
> Licensed under AGPL-3.0. Not affiliated with, or endorsed by, Sony Interactive Entertainment.

![chiaki-ng logo](gui/res/chiaking-logo.svg)

# Pleikkari Android

Pleikkari Android is an experimental, Android-only fork of chiaki-ng focused on reducing end-to-end latency for PS5 Remote Play toward a GeForce NOW-class experience. Development and measurements currently target one Galaxy S22 Ultra (SM-S908B, Exynos 2200); desktop and other non-Android targets are not maintained in this fork.

## What it looks like

The app opens with one clear path into Remote Play. After sign-in, the home screen keeps consoles, their current state, the Play action, and three practical quality presets together. Advanced experiments stay out of the everyday flow.

<p align="center">
  <img src="docs/screenshots/onboarding.png" width="30%" alt="Pleikkari Android welcome screen with Sign in with PSN and Add by address actions">
  <img src="docs/screenshots/home.png" width="30%" alt="Pleikkari Android console home screen with quality presets and a Play action">
  <img src="docs/screenshots/settings.png" width="30%" alt="Pleikkari Android settings screen with account, controller and about sections">
</p>

The three static-interface images above are Android emulator captures.

<p align="center">
  <img src="docs/screenshots/stream-overlay.jpg" width="46%" alt="Live PS5 Remote Play session on a Galaxy S22 Ultra with the in-stream control dock and network-quality chip visible">
  <img src="docs/screenshots/stream-summary.png" width="46%" alt="Pleikkari Android stream-end summary showing session duration, average latency, dropped frames, and network quality">
</p>

These two images are from a real Remote Play session between the Galaxy S22 Ultra and PS5-466: the in-stream controls with their network-quality chip, followed by the stream-end summary.

## Download

Download the latest `android-port` APK from the stable [pleikkari-android-debug.apk](https://github.com/Wnt/pleikkari/releases/download/android-latest/pleikkari-android-debug.apk) link. Each workflow run also keeps a `pleikkari-android-debug` artifact for 30 days under the repository's [Actions](https://github.com/Wnt/pleikkari/actions/workflows/build-android.yml) page.

For the Oculus Go, download [pleikkari-android-go-vr.apk](https://github.com/Wnt/pleikkari/releases/download/android-latest/pleikkari-android-go-vr.apk) (workflow artifact `pleikkari-android-go-vr`). It is the same build plus the native VrApi cinema and the Go Library VR entry. It bundles Meta's `libvrapi.so`, as every Go VR app does. The Oculus Mobile SDK is licensed and is not in this repository: CI fetches it from a private repository, only on pushes to `android-port`.

This is a debug build signed with the project debug key. Android may ask you to allow installs from your browser or file manager. Existing development installs can be updated with `adb install -r pleikkari-android-debug.apk` without removing registration data; do not uninstall first if you want to retain registered consoles.

## Status

**PS5 only.** This fork does not support the PS4: registration always targets a PS5 and there is no console-type choice. A PS4 registration carried over from an older install is shown as unsupported with a Delete action, and the app will not stream to it. The PS4 code in the shared C library is kept only to ease upstream merges; the Android app never selects it, and nobody tests it.

This is a research fork, not a finished general-purpose release. Most changes are opt-in so they can be measured against upstream behavior, results come from one phone and one PS5, and some experiments are known to regress performance or not work. The project issue tracker and ticket board are private.

This project is not endorsed or certified by Sony Interactive Entertainment LLC.

## Based on chiaki-ng

This fork is based on [streetpea/chiaki-ng](https://github.com/streetpea/chiaki-ng), the open-source PlayStation Remote Play client for PlayStation 4 and PlayStation 5. For the maintained desktop clients, general chiaki-ng documentation, community support, and upstream releases, use the [upstream project](https://github.com/streetpea/chiaki-ng) and its [documentation site](https://streetpea.github.io/chiaki-ng/).

The fork's working branch is `android-port`; the upstream project is not responsible for the experimental changes described below.

## What changed from upstream

Defaults below are the effective defaults in this branch. Preference keys are shown because they also identify the exact A/B controls used in the verification reports.

| Ticket | Change | Setting or build control | Default |
| --- | --- | --- | --- |
| PLE-6 | Three-tier MediaCodec low-latency configuration with automatic fallback | `stream_decoder_low_latency` | Off |
| PLE-7 | Surface frame-rate and display-mode selection: system default, match stream, or highest refresh | `stream_display_refresh_rate` | `system_default` |
| PLE-8 | Bypass the 16 ms Takion video reorder queue and rely on frame FEC | `stream_takion_video_packet_reordering_disabled` | Off |
| PLE-9 | Drop stale decoder output and request an IDR at five queued frames | `stream_decoder_late_frame_recovery` | Off |
| PLE-10, PLE-47 | Raise Takion `SO_RCVBUF` to at least 1 MiB and mark traffic DSCP EF | `-PchiakiTakionSocketTuning=true` | Off |
| PLE-11, PLE-59 | Raise stream-thread priority and pin Takion, decoder-output, and presenter threads to big cores (CPUs 4–7) | `stream_thread_priority_boost` | Off |
| PLE-12 | Coalesce touch-control redraws to vsync and request unbuffered touch dispatch | `preferences_coalesce_touch_redraw_enabled` | Off |
| PLE-13, PLE-44 | Coalesce controller updates into one JNI call per display frame | `stream_controller_input_coalescing` | Off |
| PLE-14 | Use the display cutout and request minimal post-processing / TV game mode | `preferences_stream_window_optimizations_enabled` | Off |
| PLE-15, PLE-65 | Decouple decoder input from network receive with a latest-frame handoff | `stream_decoder_input_thread` | **On** |
| PLE-16, PLE-54 | Use the PS5 frame index as the decoder PTS; timestamped modes self-pin the highest display refresh | `stream_real_video_timestamps` | Off |
| PLE-17 | Fixed receive-buffer pool and Linux/Android `recvmmsg` batching | `-PchiakiTakionReceiveBatching=true` | Off |
| PLE-18, PLE-52, PLE-74 | Render the deband shader only when a new frame is available; PLE-74 fixed the lost-notification freeze | `stream_debanding_render_when_dirty` | Off |
| PLE-19 | Reduce the controller feedback-state minimum interval from 8 ms to 4 ms | `stream_feedback_reduced_interval` | Off |
| PLE-20 | Configure the maximum packet loss reported to the console | `stream_packet_loss_max_percent` | 5% |
| PLE-23, PLE-54, PLE-107, PLE-211 | Vsync-paced presenter with lowest-latency, balanced, and smoothest modes, plus selectable late-frame recovery | `stream_video_pacing_enabled`; `stream_video_pacing_mode`; `stream_video_recovery_strategy` | Off; `balanced`; `flush` |
| PLE-24 | Experimental AImageReader/EGL deband renderer with a front-buffer attempt and double-buffer fallback | `-PchiakiAndroidEglRenderer=true` | Off |
| PLE-25 | In-app PSN sign-in and account-ID retrieval for registration | `psn_sign_in_registration_enabled` | Off |
| PLE-26, PLE-50 | Kotlin PSN remote-play control plane, PIN-less registration path, and PSN consoles/actions on the home screen | `psn_remote_play_enabled` | Off; depends on PSN sign-in |
| PLE-57 | Once-per-second feedback packet-rate diagnostics in the session log | `stream_feedback_stats_log` | Off |
| PLE-67 | Name native Android Takion, decoder, and presenter threads for diagnostics | No setting | Always enabled on Android |
| PLE-70 | Sustained-performance request, decoder operating-rate request, and Android Dynamic Performance Framework frame-budget hints | `stream_performance_mode` | Off |

The remaining landed tickets produced evidence or build support rather than a user-facing runtime switch: PLE-5 and PLE-21 are baseline/reference captures; PLE-28 verified the Android build and launch; PLE-47 and PLE-64 ran the two A/B rounds; and PLE-48 caches Android OpenSSL external-project builds. They do not change a user setting. PLE-52 attempted to repair the PLE-18 dirty-render path, PLE-54 made timestamped modes request the highest refresh rate, PLE-59 extended PLE-11 with big-core affinity, PLE-65 promoted only decoder-input decoupling to default-on, and PLE-74 subsequently repaired and device-verified the dirty-render path.

## Measurements so far

Start with the [pre-change baseline](docs/verification/PLE-5.md), then read [A/B round 1](docs/verification/AB-2026-09-16.md) and [A/B round 2](docs/verification/AB-2026-09-16-round2.md). These are short Perfetto/logcat studies on the single S22 Ultra, not broad benchmarks or controller-to-photon measurements.

- **Won:** decoder-input decoupling reduced Takion wakeups by about 18–20% without a measured latency cost, so it became the sole default-on latency change. Big-core affinity reduced Takion CPU time by 26–37%, but showed no latency win and still needs power/thermal testing.
- **Lost or showed no gain:** real PTS and the vsync-paced presenter increased decode delay and dropped frames even with the panel pinned at 120 Hz. The low-latency decoder request, reorder bypass, late-frame recovery, socket tuning, touch/controller coalescing, window flags, receive batching, feedback interval, and packet-loss cap showed no latency signal in the tested clean-LAN scenarios.
- **Broken in the linked A/B rounds:** deband render-when-dirty froze after the PLE-52 repair attempt. PLE-74 later fixed the lost frame-ready notification and device-verified roughly one GL swap per video frame, but the experiment remains default-off. The EGL front-buffer build improves the deband path relative to continuous GLSurfaceView rendering, but remains slower than direct decoder-to-SurfaceView presentation and stays build-time opt-in.

## Build the Android app

The Android project is in `android/`. It requires Android SDK/compile SDK 35, Android NDK `28.2.13676358`, CMake 3.22.1, and Java 17. The app's `minSdk` is 24 and its `targetSdk` is 35; the exact authoritative values are in [`android/app/build.gradle`](android/app/build.gradle).

With the SDK and NDK installed and your Android environment configured:

```bash
cd android
./gradlew assembleDebug
```

The APK is written to `android/app/build/outputs/apk/debug/app-debug.apk` relative to the repository root.

By default, the build includes all configured Android ABIs. Pass a comma-separated list with `-PchiakiAbiFilters=arm64-v8a,x86_64` to narrow a development build; the rolling Android CI build uses only `arm64-v8a`, while release builds leave the property empty and continue to include every ABI.

## Register a console and stream

1. Open the app and choose **Sign in with PSN**. **Add by address** remains available for LAN-only setups.
2. Choose a console and tap **Play**. The app registers it automatically when possible.
3. If the console asks for a PIN, on the PS5 open **Settings → System → Remote Play → Link Device** and enter the displayed eight-digit PIN in the app. Tap **Play** when registration completes.

Choose **Balanced**, **Low latency**, or **Data saver** directly on the home screen. Balanced is ready to use without visiting Settings. During a stream, tap the video to reveal the connection-quality indicator and touch controls; ending the session returns to a short summary of duration, latency, dropped frames, and network quality.

## License and disclaimer

Chiaki is a Free and Open Source Software client for PlayStation 4 and PlayStation 5 Remote Play. See [LICENSES](LICENSES) and the source-file SPDX headers for licensing details.

This project is not endorsed or certified by Sony Interactive Entertainment LLC. PlayStation, PS4, and PS5 are trademarks or registered trademarks of Sony Interactive Entertainment Inc.
