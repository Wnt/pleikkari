# PLE-730: VR Home on the real Oculus Go

**Setup.**

- The Go: 192.168.1.202:5555, serial `1KWPH802EW8203` (`serial.txt`), Android 7.1.1.
- The APK: the debug build of this branch at `d38c0d5a`, with the VR SDK linked; the gate log shows GATE: PASS.
- The session: one run of `go-home.sh` under `device.py run --resource go PLE-730`, from 12:31:58 to 12:36:54 UTC (15:3x on the Go's clock).
- The PS5: PS5-466 probed `ready` beforehand (`scripts/dev/ps5-discover.py`), and no PS5 hold was set.
- Raw capture: `build/captures/ple730/20260927T123158/` in the worktree, holding logcat, `shot-*.png` and the backup.
- Excerpts here: `home-log.txt` (GoVrUi, GoVrEntry and Cinema video lines), `vrapi-per-second.txt`, `screen-check.txt` and `session.txt`.

## What was shown

The run launched the app the way the Library does: MAIN/INFO `.stream.GoVrLibraryEntry`, handed to vrshell's desktop. `debug.pleikkari.go_entry_choose=1` made it open Home instead of streaming on its own, and `vr_full_pose=1` put the panel in front of the Go lying on the table. PLE-739's `DEBUG_GO_VR_INPUT` broadcast drove the pad: `right` is D-pad down, `centre` is A, `back` is Back.

| Shot | Log | Seen in both eyes |
| --- | --- | --- |
| `shot-1-cards` | `Home: 'Looking for your PlayStation…'`, then `Home: 1 console card(s)` 0.13 s later | The "Choose a PlayStation" sheet. The PS5-466 card shows a Ready dot, "Ready · last played" and a Play pill. Below it is the Oculus TV screen row, then Settings and Exit. |
| `shot-2-pad-focus` | `Focus card 'PS5-466'` | The accent focus ring is on the last-played card after the first pad move. |
| `shot-3-pointer-card`, `shot-4-pointer-play` | `Debug pointer at -12.0, 6.9`, `Hover card 'PS5-466'`, then no change at 19, 6.9 | The hover tint, reticle and laser over the card's name, then over its Play pill. It is still the same target, because the whole card is the target. |
| `shot-5-menu-over-home` | `Menu opened (back)`, then `Menu closed (back)` | The in-stream menu, with Exit because nothing streams yet, replaces Home on the same panel. Back returns to Home. |
| `shot-6-status-sheet` | `Activate card 'PS5-466' (pad)`, `Home: Play(...)`, `Connecting to PS5-466 at 192.168.1.164 (awake)`, `Home: 'Connecting to PS5-466…'` | The status sheet: title PS5-466, a spinner, "Connecting to PS5-466…" and Cancel. The console was awake, so the looking and waking sheets were skipped. |
| `shot-7-stream` | `Home closed` 2.98 s after Play, on `StreamStateConnected`. Then `Cinema video:` 56.9, 59.7, 59.4, 59.3, 59.5, 60.0, 58.7, 59.8 and 58.7 fps, with 360 of 360 submitted frames showing video from the second window on | The live PS5 home screen, with no panel. |
| `shot-8-no-console`, `shot-9-no-console-focus` | `debug.pleikkari.go_entry_no_console=1`: `No PS5 linked (0 registered console(s))`, `Home: 'No PlayStation 5 is linked to Pleikkari yet.'`, `Focus button 'Oculus TV screen'` | The no-console sheet: its message, "Link one in Pleikkari on the Oculus TV screen.", then Exit and a primary "Oculus TV screen" button, which the pad focuses first. |

`screen_check.py` classed all nine shots as `picture`. No run logged a FATAL line.

## The head loop (`vrapi-per-second.txt`)

- **Home up** (15:35:02 to 15:35:29, 28 s, cards, pointer, menu and connecting sheet): FPS 72 (73 once), `LCnt=2` (the projection layer plus the Home panel) and Prd 33 ms. `Stale` was 0 in 25 of the 28 seconds. The exceptions were 2 at 15:35:02 (the first second after the cinema started), 1 at 15:35:24 (the cards, before Play) and 1 at 15:35:28 (during the spinner).
- **After `Home closed`**: `LCnt=1`, FPS 72 and Prd 32 ms. `Stale` was 0, except one second with 2 (15:35:41).
- **The no-console run**: Prd was 47 ms with `LCnt=2`. vrshell had shown the same 47–48 ms right before the launch (15:34:58, 15:36:22), and PLE-715 documents that regime.

## The GoVrUi thread

- The spinner redraws the status sheet 8 times a second. Steady redraws took 1.86–2.52 ms on average (`Redraws: 9 in 1.1 s`, `8 in 1.0 s`).
- An isolated redraw after an idle gap takes 7–10 ms. The first menu draw over Home took 31 ms. §10.9 budgets 3 ms. This work stays off the GoCinema thread, and the head loop above did not move.

## Restored

- The snapshot APK is back (md5-checked) and the prefs are back byte-identically.
- `vr_full_pose=0`, `go_entry_choose=0`, `go_entry_no_console=0` and `vr_pointer=off` were each read back with `getprop`.
- vrshell MainActivity was resumed at the end.

## Not shown on the Go

- The looking and waking sheets, and the not-found sheet with Try again. They need a PS5 in rest mode or off the network, and PS5-466 was awake. JVM tests cover their layout and in-place updates (`VrHomeTest`).
- Cancel on a sheet, Settings and the Oculus TV row. Activating the row leaves VR.
- The real Go remote's trigger and touchpad click on a card, with a person holding the remote. The pointer states were driven by `debug.pleikkari.vr_pointer`.
- A Bluetooth pad. The pad focus and A were driven through the debug broadcast, which goes through the same `menuKey` path as a pad key.

## Session 2: the merged build (`ea27da6f`, after PLE-722, PLE-731 and PLE-732 landed)

**Setup.** This session used the same Go, the same script (plus the Settings step, with the entry step's `am start -W` dropped) and PS5-466 awake. Capture: `build/captures/ple730/20260927T125200/`. Excerpts: `home-log-merged.txt`, `vrapi-per-second-merged.txt` and `screen-check-merged.txt`, where all 12 shots are `picture`.

**Repeated from session 1, all the same:** the cards, the pad focus, the pointer on the card and on Play, the menu over Home, Play, the connecting sheet, `Home closed` 2.5 s after Play with 59 fps video after it, and the no-console sheet.

**New in this session: Settings from Home.** With the pad, the focus walked from the card to the Oculus TV row, then to `Focus button 'Settings'`. A logged `Home: Settings`, `Menu opened (Home Settings)`, `Menu page SETTINGS`. The shot `shot-5c-settings` shows PLE-732's Settings sheet over Home. Back logged `Menu page MENU`, and a second Back logged `Menu closed (back)`, with Home shown again in `shot-5d-home-again`.

**The head loop.** For the 45 s that Home or the menu was up, `LCnt=2` in every second and FPS was 72 in 44 of the 45 seconds. `Stale` was 1 in two seconds, 15:53:42 and 15:53:45, the seconds the menu opened over Home and closed again. After `Home closed`, `LCnt=1`, FPS was 72 and Stale was 0 except 2 in the first second.
