# PLE-668: Go `stream_go_vr_room_high_gpu` A/B per environment

Oculus Go (serial in `serial.txt` of the capture), 2026-09-27 08:39–08:54 UTC, this branch's debug APK
(`-PchiakiGoVr=true`, = `fork/android-port` 87fce8a7) installed with `install -r` and the
original APK and prefs restored afterwards. The PS5 was on hold, so every arm is the debug
**no-console preview** (synthetic 1920x1080 picture at 60 fps), not a stream. The Go was on AC
power and on a table, `debug.pleikkari.vr_full_pose=1` during each arm. Every screencap passes
`screen_check.py` ("picture", ~31–34 % lit). Each arm ran 140 s; VrApi's first 10 lines skipped.
Order: void-off, void-on, cinema-on, cinema-off, terrace-off, terrace-on.

Reproduce (under the Go lease, each call ≤ 10 min):

    DEADLINE=$(( $(date +%s) + 570 )) SECS=140 scripts/dev/device.py run --resource go PLE-668 -- \
      bash docs/verification/PLE-668/go-ab.sh <out> setup ensure <apk> \
      armp void-off void stream_go_vr_room_high_gpu=false armp void-on void stream_go_vr_room_high_gpu=true ...
    # ... last call ends with `restore`
    python3 docs/verification/PLE-668/ab-summary.py <out>

## Result

| arm | clock line | s | Stale/s mean | Stale total | App= ms | GPU MHz | GPU% | VrApi Temp start→end | uA mean | batt °C start→end | pm °C start→end |
|---|---|---|---|---|---|---|---|---|---|---|---|
| cinema-off | CPU 2 / GPU 2 (accepted) | 141 | 7.92 | 1117 | 8.27 | 401 | 0.71 | 35.7→35.5 | -267790 | 35.5→35.5 | 61.0→60.3 |
| cinema-on | CPU 2 / GPU 4 (accepted) | 140 | 1.18 | 165 | 7.24 | 510 | 0.63 | 35.7→35.7 | -338463 | 35.7→35.7 | 61.1→60.6 |
| terrace-off | CPU 2 / GPU 2 (accepted) | 137 | 4.77 | 654 | 7.47 | 510 | 0.64 | 35.5→35.7 | -256132 | 35.5→35.7 | 60.9→60.4 |
| terrace-on | CPU 2 / GPU 4 (accepted) | 139 | 0.01 | 1 | 7.35 | 510 | 0.63 | 35.2→35.7 | -219216 | 35.7→35.5 | 61.1→60.4 |
| void-off | CPU 2 / GPU 2 (accepted) | 136 | 6.26 | 852 | 8.16 | 401 | 0.69 | 35.2→35.2 | -270750 | 35.7→35.5 | 60.6→60.0 |
| void-on | CPU 2 / GPU 4 (accepted) | 138 | 0.52 | 72 | 7.26 | 510 | 0.63 | 35.7→35.5 | -215605 | 35.2→35.5 | 60.6→60.4 |

- **Stale frames:** high GPU (level 4, the runtime then runs the GPU at 510 MHz) cuts VrApi
  Stale/s in every room: void 6.3 → 0.5, cinema 7.9 → 1.2, terrace 4.8 → 0.0.
- **Clock line:** `Clock levels CPU 2 / GPU 2|4 (accepted)` in every arm; never refused.
- Level 2 is not a fixed frequency: terrace-off ran at 510 MHz for most of the arm while
  void-off and cinema-off sat at 401 MHz, which is why terrace-off stales least of the off arms.
- **Power and heat:** not resolved at this length. On AC, current_now is negative (charging) in
  every arm and its arm-to-arm spread (-216 to -338 mA) has no on/off pattern; battery and PM
  thermal zones move ≤ 0.7 °C within an arm. The ticket's 20-minute session per arm did not fit
  a worker (10-minute Bash cap, no background jobs); it needs a human-run session off AC.
