#!/usr/bin/env bash
# PLE-722: prove the Go VR UI toolkit's in-stream menu on the real Oculus Go, in one session under
# the Go lease, with the debug preview (no console):
#   setup           back up the prefs, the registration database, the app data and the installed APK
#   ensure <apk>    install -r ours unless it is already installed
#   ui <label> <environment>
#                   the preview in that room with the VR menu on and the stats overlay off. Windows of
#                   GO_WINDOW s each, marked in logcat (tag PLE722) for windows.py: menu closed; menu open
#                   and idle; menu open with the debug pointer moving across its widgets. Between them,
#                   screencaps: closed, open, pad focus (injected gamepad keys), pointer hover, the
#                   stats overlay and a room picked with the pad. Then the prefs go back.
#   restore         the original APK and prefs back, the app stopped: the Go shows its VR home again
# Run from the worktree, under the lease:
#   DEADLINE=$(( $(date +%s) + 560 )) scripts/dev/device.py run --resource go PLE-722 -- \
#       bash docs/verification/PLE-722/go-ui.sh <out> setup ensure <apk> ui plain plain ui cinema cinema restore
# Env: GO_WINDOW (s, default 45), DEADLINE (epoch s).
set -uo pipefail
ROOT=/home/wnt/gta6
HERE=$(cd "$(dirname "$0")" && pwd)
A=$ROOT/scripts/dev/device-bin/adb
G=192.168.1.202:5555
PKG=fi.madekivi.pleikkari
PREFS=shared_prefs/${PKG}_preferences.xml
OUT=${1:?out dir}; shift
GO_WINDOW=${GO_WINDOW:-45}
mkdir -p "$OUT"
LOG="$OUT/session.txt"
BACKUP=${BACKUP:-$OUT/backup}
[ -n "${PLEIKKARI_GO_LEASE_TOKEN:-}" ] || { echo "run under scripts/dev/device.py run --resource go" >&2; exit 2; }
a() { echo "$(date -u +%T) adb $*" >>"$OUT/adb.txt"; timeout 120 "$A" -s "$G" "$@"; }
say() { echo "$(date -u +%T) $*" | tee -a "$LOG"; }
mark() { a shell log -t PLE722 "$1"; say "mark: $1"; }
resumed() { a shell 'dumpsys activity activities | grep mResumedActivity' | tr -d '\r' | grep -o "$PKG/[^ ]*" | head -1; }
state() { a shell 'dumpsys activity activities | grep -E "mResumedActivity|mFocusedActivity"; dumpsys window | grep mCurrentFocus' | tr -d '\r' | sed 's/^ */    /'; }
shot() { # <name>
	a exec-out screencap -p > "$OUT/$1.png"
	say "screencap $1: $(wc -c < "$OUT/$1.png") B"
}
# Android 7.1's `input <source> keyevent` counts its arguments wrong and refuses a single key; a
# trailing KEYCODE_UNKNOWN (keycode 0, which the menu ignores) gets the first one through.
pad() { a shell input gamepad keyevent "$1" KEYCODE_UNKNOWN; sleep 0.6; }
pointer() { a shell setprop debug.pleikkari.vr_pointer "'$1'"; sleep 0.5; }
# The Go's logcat ring is small next to VrApi's once-a-second lines from every VR process, so the
# session's log is dumped after every window, each dump from where the last one ended.
dump() { # <file>
	local now
	now=$(a shell "date +'%m-%d %H:%M:%S.000'" | tr -d '\r')
	a shell "logcat -d -v threadtime -T '$since'" | tr -d '\r' >> "$1"
	since=$now
}

write_prefs() { # <file>
	a push "$1" /data/local/tmp/ple722_prefs.xml >/dev/null
	a shell "chmod 644 /data/local/tmp/ple722_prefs.xml; run-as $PKG cp /data/local/tmp/ple722_prefs.xml $PREFS; rm -f /data/local/tmp/ple722_prefs.xml"
	a exec-out run-as $PKG cat $PREFS > "$OUT/prefs-readback.xml"
	cmp -s "$1" "$OUT/prefs-readback.xml" || { say "prefs readback mismatch for $1"; return 1; }
}

arm_prefs() { # <dst> <spec>
	python3 - "$BACKUP/prefs.xml" "$1" "$2" <<'EOF'
import re, sys
src, dst, spec = sys.argv[1:]
text = open(src).read()
for item in filter(None, spec.split(",")):
    key, value = item.split("=", 1)
    text = re.sub(r'\s*<boolean name="%s" value="[a-z]+" />' % re.escape(key), "", text)
    text = re.sub(r'\s*<string name="%s">[^<]*</string>' % re.escape(key), "", text)
    text = text.replace("</map>", '    <boolean name="%s" value="%s" />\n</map>' % (key, value))
open(dst, "w").write(text)
EOF
}

step_setup() {
	local cur path
	if [ -s "$BACKUP/base.apk" ]; then say "backup in $BACKUP exists; keeping the first snapshot"; return 0; fi
	cur=$(resumed)
	case "$cur" in *Stream*) say "abort: a stream is live ($cur), not ours"; exit 2 ;; esac
	mkdir -p "$BACKUP/databases"
	a exec-out run-as $PKG cat $PREFS > "$BACKUP/prefs.xml"
	[ -s "$BACKUP/prefs.xml" ] || { say "abort: empty prefs backup"; exit 2; }
	for f in chiaki chiaki-wal chiaki-shm; do
		a exec-out run-as $PKG cat databases/$f > "$BACKUP/databases/$f" 2>/dev/null
		[ -s "$BACKUP/databases/$f" ] || rm -f "$BACKUP/databases/$f"
	done
	a exec-out run-as $PKG tar -cf - . > "$BACKUP/appdata.tar"
	path=$(a shell pm path $PKG | tr -d '\r' | sed -n 's/^package://p' | head -1)
	a pull "$path" "$BACKUP/base.apk" >/dev/null || { say "abort: cannot pull $path"; exit 2; }
	a shell dumpsys package $PKG | tr -d '\r' | grep -E "versionCode|lastUpdateTime" > "$BACKUP/package.txt"
	say "backup: prefs $(wc -c < "$BACKUP/prefs.xml") B, db $(ls "$BACKUP/databases" | tr '\n' ' '), appdata $(wc -c < "$BACKUP/appdata.tar") B, apk $(sha256sum "$BACKUP/base.apk" | cut -c1-16)"
}

step_ensure() { # <apk>
	local want have path
	want=$(md5sum < "$1" | cut -d' ' -f1)
	path=$(a shell pm path $PKG | tr -d '\r' | sed -n 's/^package://p' | head -1)
	have=$(a shell md5sum "$path" | tr -d '\r' | cut -d' ' -f1)
	if [ "$want" = "$have" ]; then say "installed APK is ours ($want)"; return 0; fi
	say "installed APK $have is not ours ($want): install -r"
	a shell am force-stop $PKG
	a install -r "$1" 2>&1 | tail -1 | tee -a "$LOG"
}

step_ui() { # <label> <environment>
	local label=$1 environment=$2 cur full_pose pid i
	say "=== ui $label, room $environment"
	cur=$(resumed)
	case "$cur" in *Stream*) say "abort: a stream is live ($cur)"; exit 2 ;; esac
	arm_prefs "$OUT/prefs-$label.xml" "stream_go_vr_ui=true,stream_diagnostics_overlay=false,stream_feedback_stats_log=false"
	a shell am force-stop $PKG
	sleep 2
	write_prefs "$OUT/prefs-$label.xml" || exit 3
	full_pose=$(a shell getprop debug.pleikkari.vr_full_pose | tr -d '\r')
	a shell setprop debug.pleikkari.vr_full_pose 1
	a shell setprop debug.pleikkari.vr_pointer off
	since=$(a shell "date +'%m-%d %H:%M:%S.000'" | tr -d '\r')
	: > "$OUT/$label-logcat.txt"
	a shell run-as $PKG am start --user 0 -n $PKG/.stream.StreamVrActivity \
		--ez vr_cinema_preview true --es environment "$environment" 2>&1 | tr -d '\r' >> "$LOG"
	for _ in $(seq 1 15); do cur=$(resumed); case "$cur" in *StreamVr*) break ;; esac; sleep 1; done
	say "resumed: ${cur:-none}"
	case "$cur" in *StreamVr*) ;; *) state | tee -a "$LOG"; a shell am force-stop $PKG
		a shell setprop debug.pleikkari.vr_full_pose "${full_pose:-0}"; write_prefs "$BACKUP/prefs.xml"; return 7 ;; esac
	pid=$(a shell pidof $PKG | tr -d '\r')
	echo "$pid" > "$OUT/$label-pid.txt"
	sleep 6
	mark "$label closed start"
	shot "$label-1-closed"
	sleep "$GO_WINDOW"
	mark "$label closed end"
	dump "$OUT/$label-logcat.txt"
	# The pad's Menu key opens the menu (a keyboard-source key, as a remote-less test can send).
	a shell input keyevent KEYCODE_MENU
	sleep 1.5
	shot "$label-2-open"
	mark "$label open-idle start"
	sleep "$GO_WINDOW"
	mark "$label open-idle end"
	dump "$OUT/$label-logcat.txt"
	# §11: does an injected gamepad key reach the app as SOURCE_GAMEPAD on the Go's Android 7.1?
	pad KEYCODE_DPAD_DOWN
	shot "$label-3-pad-focus"
	# The debug ray: Recentre in the left column, then across the widgets while a window runs.
	pointer "-17,3"
	sleep 0.5
	shot "$label-4-pointer-recentre"
	mark "$label open-pointer start"
	local targets=("-17,7.5" "-17,3" "-4,6" "6,6" "16,6" "7,-6" "7,-11" "-17,-11" "20,0" "0,-2")
	i=0
	local end=$(( $(date +%s) + GO_WINDOW ))
	while [ "$(date +%s)" -lt "$end" ]; do
		a shell setprop debug.pleikkari.vr_pointer "'${targets[$(( i % ${#targets[@]} ))]}'"
		i=$(( i + 1 ))
		sleep 0.4
	done
	mark "$label open-pointer end"
	dump "$OUT/$label-logcat.txt"
	pointer "7,-6"
	shot "$label-5-pointer-row"
	a shell setprop debug.pleikkari.vr_pointer off
	# The pad: focus Resume, right to the first room, down through the settings to Stats overlay, A.
	pad KEYCODE_DPAD_UP
	pad KEYCODE_DPAD_RIGHT
	pad KEYCODE_DPAD_DOWN
	pad KEYCODE_DPAD_DOWN
	pad KEYCODE_DPAD_DOWN
	pad KEYCODE_BUTTON_A
	sleep 1
	shot "$label-6-stats-on"
	# Up to the first room line, right to the second room, A: the room changes under the menu.
	pad KEYCODE_DPAD_UP
	pad KEYCODE_DPAD_UP
	pad KEYCODE_DPAD_UP
	pad KEYCODE_DPAD_RIGHT
	pad KEYCODE_BUTTON_A
	sleep 2
	shot "$label-7-room-picked"
	pad KEYCODE_BUTTON_B
	sleep 1
	shot "$label-8-closed-with-stats"
	dump "$OUT/$label-logcat.txt"
	a shell am force-stop $PKG
	a shell setprop debug.pleikkari.vr_full_pose "${full_pose:-0}"
	a shell setprop debug.pleikkari.vr_pointer off
	sleep 2
	write_prefs "$BACKUP/prefs.xml" && say "original prefs back"
	grep -E 'FATAL EXCEPTION|AndroidRuntime' -A12 "$OUT/$label-logcat.txt" | head -40 > "$OUT/$label-crashes.txt"
	say "GoVrUi lines: $(grep -c ' GoVrUi' "$OUT/$label-logcat.txt"); crashes: $(grep -c 'FATAL' "$OUT/$label-crashes.txt")"
	python3 "$HERE/windows.py" "$OUT/$label-logcat.txt" "$pid" | tee -a "$LOG"
	python3 "$ROOT/scripts/dev/go-latency/screen_check.py" "$OUT/$label"-*.png --logcat "$OUT/$label-logcat.txt" > "$OUT/$label-screen-check.txt" 2>&1
	say "screen_check rc=$?"
}

step_restore() {
	local now
	a shell am force-stop $PKG
	now=$(a shell md5sum "$(a shell pm path $PKG | tr -d '\r' | sed -n 's/^package://p' | head -1)" | tr -d '\r' | cut -d' ' -f1)
	if [ "$now" != "$(md5sum < "$BACKUP/base.apk" | cut -d' ' -f1)" ]; then
		a install -r "$BACKUP/base.apk" 2>&1 | tail -1 | tee -a "$LOG"
	fi
	now=$(a shell md5sum "$(a shell pm path $PKG | tr -d '\r' | sed -n 's/^package://p' | head -1)" | tr -d '\r' | cut -d' ' -f1)
	[ "$now" = "$(md5sum < "$BACKUP/base.apk" | cut -d' ' -f1)" ] && say "original APK back" || say "ORIGINAL APK NOT RESTORED ($now)"
	write_prefs "$BACKUP/prefs.xml" && say "prefs restored byte-identically"
	a shell setprop debug.pleikkari.vr_full_pose 0
	a shell setprop debug.pleikkari.vr_pointer off
	a shell am force-stop $PKG
	state | tee -a "$LOG"
}

DEADLINE=${DEADLINE:-$(( $(date +%s) + 3600 ))}
fits() { [ $(( $(date +%s) + $1 )) -le "$DEADLINE" ]; }
awake() { [ "$(a shell 'dumpsys power' | tr -d '\r' | sed -n 's/^ *mWakefulness=//p' | head -1)" = Awake ]; }
say "start; $(( DEADLINE - $(date +%s) ))s before the deadline; $(a shell 'dumpsys power | grep mWakefulness=' | tr -d '\r ')"
state | tee -a "$LOG"
a shell getprop ro.serialno | tr -d '\r' > "$OUT/serial.txt"
while [ $# -gt 0 ]; do
	case "$1" in
	setup) shift; fits 150 || { say "deferred: no time for setup"; exit 9; }; step_setup ;;
	ensure) if fits 150; then step_ensure "$2"; else say "deferred ensure"; fi; shift 2 ;;
	ui) if ! awake; then say "skipped ui $2: the Go is asleep"
		elif fits $(( 3 * GO_WINDOW + 110 )); then step_ui "$2" "$3" || say "ui $2 failed rc=$?"; else say "deferred ui $2"; fi; shift 3 ;;
	restore) shift; step_restore ;;
	*) say "unknown step $1"; exit 2 ;;
	esac
done
say "done"
