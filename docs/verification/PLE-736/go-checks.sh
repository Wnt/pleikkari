#!/usr/bin/env bash
# PLE-736: answer vr-ui.md §11's open questions on the real Oculus Go, with nobody in the headset.
# The synthetic cinema preview, so nothing connects to a PS5. With <apk>, PLE-654's go-live.sh
# snapshots the app (prefs, databases, data tar, base.apk), install -r's <apk>, and restores after.
#   scripts/dev/device.py run --resource go PLE-736 -- bash docs/verification/PLE-736/go-checks.sh <out> [apk]
# Steps: record the installed build; vr_full_pose=1; start StreamVrActivity --ez vr_cinema_preview true
# as the app uid; read "Suggested eye FOV"; screencap with the menu closed and open (the menu is a
# second VrApi cylinder layer); close the menu with a keyboard-source then a gamepad-source BUTTON_B
# (only a SOURCE_GAMEPAD/JOYSTICK event closes it, VrUiHost.isPad); inject a long Back; dump the
# input dispatcher's view; force-stop, vr_full_pose back, vrshell resumed.
set -uo pipefail
ROOT=/home/wnt/gta6
A=$ROOT/scripts/dev/device-bin/adb
G=192.168.1.202:5555
PKG=fi.madekivi.pleikkari
OUT=${1:?out dir}
APK=${2:-}
WT=$(cd "$(dirname "$0")/../../.." && pwd)
mkdir -p "$OUT"
export DEADLINE=${DEADLINE:-$(( $(date +%s) + 580 ))}
golive() { BACKUP=$OUT/backup bash "$WT/docs/verification/PLE-654/go-live.sh" "$OUT" "$@"; }
a() { timeout 60 "$A" -s "$G" "$@"; }
say() { echo "$(date -u +%T) $*" | tee -a "$OUT/session.txt"; }
shot() { a exec-out screencap -p > "$OUT/$1.png"; say "shot $1: $(stat -c %s "$OUT/$1.png") bytes"; }
logs() { a shell logcat -d -v threadtime | tr -d '\r' > "$OUT/logcat.txt"; }

timeout 20 "$A" connect "$G" >/dev/null 2>&1
say "device $(a shell getprop ro.product.device | tr -d '\r') android $(a shell getprop ro.build.version.release | tr -d '\r')"
[ -n "$APK" ] && golive setup ensure "$APK"
a shell dumpsys package $PKG | tr -d '\r' | grep -E 'versionName|lastUpdateTime' | tee -a "$OUT/session.txt"
a shell input 2>&1 | tr -d '\r' > "$OUT/input-usage.txt"
old_pose=$(a shell getprop debug.pleikkari.vr_full_pose | tr -d '\r')
a shell setprop debug.pleikkari.vr_full_pose 1
preview() { # <label>
	a shell am force-stop $PKG
	a shell input keyevent KEYCODE_WAKEUP
	say "start preview $1: $(a shell run-as $PKG am start --user 0 -n $PKG/.stream.StreamVrActivity --ez vr_cinema_preview true 2>&1 | tr -d '\r' | tail -1)"
	sleep 10
}
a shell logcat -c
preview stats-off
shot closed
# Source A/B: a keyboard-source Back takes the remote path (logs "Remote Back held"); a gamepad one
# does not (StreamVrActivity.dispatchKeyEvent). Device log clock runs ~2 s ahead of the host's.
say "keyboard BACK"; a shell input keyevent KEYCODE_BACK; sleep 3
say "gamepad BACK"; a shell input gamepad keyevent KEYCODE_BACK; sleep 3
say "keyboard long BACK (--longpress)"; a shell input keyevent --longpress KEYCODE_BACK; sleep 3
say "menu key (keyboard)"; a shell input keyevent KEYCODE_MENU; sleep 3
shot after-menu-key
# The Go's main log buffer rolls within ~30 s under the cinema's 1 Hz pacing lines: keep the key lines now.
a shell logcat -d -v threadtime | tr -d '\r' | grep -E "GoVrUi|Suggested eye FOV" > "$OUT/keys-log.txt"
if [ -n "$APK" ]; then
	# Screencap layers: the stats overlay is a second VrApi cylinder layer (Panel 1) shown from start.
	sed 's#</map>#    <boolean name="stream_diagnostics_overlay" value="true" />\n</map>#' "$OUT/backup/prefs.xml" | grep -v '"stream_diagnostics_overlay" value="false"' > "$OUT/prefs-stats.xml"
	a push "$OUT/prefs-stats.xml" /data/local/tmp/ple736_prefs.xml >/dev/null
	a shell "chmod 644 /data/local/tmp/ple736_prefs.xml; run-as $PKG cp /data/local/tmp/ple736_prefs.xml shared_prefs/${PKG}_preferences.xml; rm -f /data/local/tmp/ple736_prefs.xml"
	preview stats-on
	shot stats-on
fi
a shell dumpsys input | tr -d '\r' > "$OUT/dumpsys-input.txt"
logs
a shell am force-stop $PKG
a shell setprop debug.pleikkari.vr_full_pose "${old_pose:-0}"
sleep 2
say "after: $(a shell 'dumpsys activity activities | grep mResumedActivity' | tr -d '\r' | sed 's/^ *//')"
say "vr_full_pose restored to '$(a shell getprop debug.pleikkari.vr_full_pose | tr -d '\r')'"
grep -E "Suggested eye FOV|VrUi|Remote Back|Menu (opened|closed)|Panels attached|GoCinema" "$OUT/logcat.txt" | tee "$OUT/key-lines.txt"
python3 $ROOT/scripts/dev/go-latency/screen_check.py "$OUT"/*.png --logcat "$OUT/logcat.txt" 2>&1 | tee -a "$OUT/session.txt"
[ -n "$APK" ] && golive restore
