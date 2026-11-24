# Native DualSense driver (macOS, IOKit HID)

DualSense and DualSense Edge over USB and Bluetooth, straight on IOKit HID: no Qt, SDL
or hidapi. Every input report reaches the client as soon as IOKit delivers it (no
polling timer), and effects go through a coalescing writer that always sends the newest
state. The public API is in `include/DualSenseHID.h`, which also documents the
threading contract.

## Build

```sh
cmake -G Ninja -S macos/dualsense -B ../../build/native-dualsense
nice -n 15 ninja -C ../../build/native-dualsense -j4
../../build/native-dualsense/plk-dualsense-tests        # or: ctest --test-dir ...
```

Targets:

- `plk_dualsense_core`: protocol, writer and haptics pacing (pure C++, unit tested).
- `plk_dualsense`: adds the IOKit side.
- `plk-dualsense-probe`
- `plk-dualsense-tests`

## Probe

```sh
plk-dualsense-probe -t 8                  # read-only: info, report rate, interval
                                          # distributions, delivery delay, lost reports
plk-dualsense-probe -t 8 --realtime       # same, driver threads real-time
plk-dualsense-probe --sched-test -t 3     # no controller: QoS vs real-time wake-ups
plk-dualsense-probe --rumble | --trigger | --led | --haptics-tone   # WRITE to the controller
```

The default mode opens the controller non-exclusively, reads feature reports 0x20 and
0x05, and writes nothing. The output modes write to the controller, so use them only
when nothing else is using it.

## API in short

```cpp
plk::dualsense::Manager m;
plk::dualsense::Manager::Options o;   // allow_output = false, realtime_threads = false
plk::dualsense::Manager::Callbacks cb;
cb.input = [](Device &d, const InputState &s) { /* HID thread, every report */ };
m.Start(o, cb);
// with allow_output: device->SetRumble(l, r); SetTriggerEffects(l11, r11);
// SetLightbar(r, g, b); StartHaptics(); PushHapticsPcm16(...)
m.Stop();                             // effects off (if any were sent), threads joined
```

`InputState` mirrors `ChiakiControllerState`:

- buttons use the same bits;
- sticks are scaled to int16, the same as SDL;
- touch coordinates are raw, 0..1919 x 0..1079;
- gyro is calibrated, in rad/s;
- accel is calibrated, in g;
- the controller's sensor clock is in µs.

The header lists each field's units.

## Haptics over USB

Not handled here. Over USB the controller has a 4-channel audio device, and channels
3 and 4 drive the actuators, so that path belongs to the audio stack. Over Bluetooth,
haptics are HID report 0x32: `StartHaptics()` ports the Qt GUI's DualSenseBtHaptics
with the same tuning, and `src/HapticsSender.h` notes where its buffering costs latency.

## Test fixtures

`tests/fixtures` holds real reports, captured read-only. Each file's header says how
it was captured. Never store feature report 0x09: it contains the controller's MAC
address.
