# PLE-736: vr-ui.md §11 open questions, checked on the Oculus Go

Go `192.168.1.202:5555` (pacific, Android 7.1.1), 2026-09-27 12:50 UTC, nobody wearing it.
`go-checks.sh` backs up the app, `install -r`s this branch's debug APK (PLE-722 + PLE-731 code),
runs the synthetic cinema preview (`--ez vr_cinema_preview true`, no PS5), injects keys, takes
screencaps with `debug.pleikkari.vr_full_pose=1`, then puts the original APK and prefs back
(byte-identical) and leaves vrshell resumed. Evidence: `session.txt`, `keys-log.txt`,
`closed-960.png`, `stats-on-960.png` (downscaled from the 2560x1440 screencaps).
The Go's clock runs about 2 s ahead of the host's timestamps in `session.txt`.

| Question | Answer |
|---|---|
| Suggested eye FOV | **90.0 x 90.0 degrees** (`VRAPI_SYS_PROP_SUGGESTED_EYE_FOV_DEGREES_X/Y`). |
| `input gamepad keyevent` gives `SOURCE_GAMEPAD`? | **Yes.** A keyboard-source `KEYCODE_BACK` takes `StreamVrActivity.dispatchKeyEvent`'s remote path and logs `Remote Back held`; the same key via `input gamepad keyevent` does not, so it arrived with `SOURCE_GAMEPAD`/`SOURCE_JOYSTICK`. |
| Does a long Back reach the app? | **Injected: yes, but the hold time is garbage.** `input keyevent --longpress KEYCODE_BACK` reaches `dispatchKeyEvent` (down and up), but `Remote Back held` logs about **-27 700 000 ms** for both the plain and the long press: the down and up `eventTime`s are not on one clock on the Go, so `held >= 750` never fires and a long Back can never recentre. A physical remote long press is not proven (needs a wearer). |
| Screencaps show extra compositor layers? | **Yes.** With the stats overlay on (Panel 1, a cylinder layer), the display-0 screencap shows the panel. But in that frame the **right eye shows only the stats panel and no cinema screen**; with the overlay off both eyes show the screen. Whether the wearer sees the same or it is a screencap-only artefact is not proven. |

## Also found

- **The Menu key opens and closes the VR menu at once.** `input keyevent KEYCODE_MENU`:
  `Menu opened (pad Menu key)` then `Menu closed (pad)` 10 ms later. `VrUiHost.key` opens on
  the down (posted); the up then arrives with the menu open and `menuKey` closes on
  `KEYCODE_MENU` up. So the menu screencap could not be taken this way.
