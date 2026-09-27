#!/usr/bin/env bash
# PLE-761: show each `debug.pleikkari.vr_ui_layers` switch working on the real Oculus Go, in one
# session under the Go lease, with the debug preview (no console). Derived from
# docs/verification/PLE-722/go-ui.sh (same backup, install -r and restore).
#   setup                 back up the prefs and the installed APK
#   ensure <apk>          install -r ours unless it is already installed
#   variant <label> <spec>
#                         the preview in the plain room with the VR menu and the stats overlay on and
#                         vr_ui_layers=<spec> ("-" for none): screencap and a WINDOW s logcat window
#                         with the menu closed, then with it open (tag PLE722 marks, for PLE-722's windows.py)
#   restore               the original APK and prefs back, props cleared, the app stopped
# Run from the worktree:
#   DEADLINE=$(( $(date +%s) + 560 )) scripts/dev/device.py run --resource go PLE-761 -- \
#       bash docs/verification/PLE-761/go-layers.sh <out> setup ensure <apk> variant base - variant quad quad restore
set -uo pipefail
ROOT=/home/wnt/gta6
HERE=$(cd "$(dirname "$0")" && pwd)
A=$ROOT/scripts/dev/device-bin/adb
G=192.168.1.202:5555
PKG=fi.madekivi.pleikkari
PREFS=shared_prefs/${PKG}_preferences.xml
OUT=${1:?out dir}; shift
WINDOW=${WINDOW:-15}
mkdir -p "$OUT"
LOG="$OUT/session.txt"
BACKUP=${BACKUP:-$OUT/backup}
[ -n "${PLEIKKARI_GO_LEASE_TOKEN:-}" ] || { echo "run under scripts/dev/device.py run --resource go" >&2; exit 2; }
a() { echo "$(date -u +%T) adb $*" >>"$OUT/adb.txt"; timeout 120 "$A" -s "$G" "$@"; }
say() { echo "$(date -u +%T) $*" | tee -a "$LOG"; }
mark() { a shell log -t PLE722 "$1"; say "mark: $1"; }
resumed() { a shell 'dumpsys activity activities | grep mResumedActivity' | tr -d '\r' | grep -o "$PKG/[^ ]*" | head -1; }
shot() { a exec-out screencap -p > "$OUT/$1.png"; say "screencap $1: $(wc -c < "$OUT/$1.png") B"; }
dump() {
	local now
	now=$(a shell "date +'%m-%d %H:%M:%S.000'" | tr -d '\r')
	a shell "logcat -d -v threadtime -T '$since'" | tr -d '\r' >> "$1"
	since=$now
}
write_prefs() {
	a push "$1" /data/local/tmp/ple761_prefs.xml >/dev/null
	a shell "chmod 644 /data/local/tmp/ple761_prefs.xml; run-as $PKG cp /data/local/tmp/ple761_prefs.xml $PREFS; rm -f /data/local/tmp/ple761_prefs.xml"
	a exec-out run-as $PKG cat $PREFS > "$OUT/prefs-readback.xml"
	cmp -s "$1" "$OUT/prefs-readback.xml" || { say "prefs readback mismatch for $1"; return 1; }
}
arm_prefs() { # <dst> <key=value,...>
	python3 - "$BACKUP/prefs.xml" "$1" "$2" <<'EOF'
import re, sys
src, dst, spec = sys.argv[1:]
text = open(src).read()
for item in filter(None, spec.split(",")):
    key, value = item.split("=", 1)
    text = re.sub(r'\s*<boolean name="%s" value="[a-z]+" />' % re.escape(key), "", text)
    text = text.replace("</map>", '    <boolean name="%s" value="%s" />\n</map>' % (key, value))
open(dst, "w").write(text)
EOF
}
apk_md5() { a shell md5sum "$(a shell pm path $PKG | tr -d '\r' | sed -n 's/^package://p' | head -1)" | tr -d '\r' | cut -d' ' -f1; }

step_setup() {
	local path
	if [ -s "$BACKUP/base.apk" ]; then say "backup in $BACKUP exists; keeping it"; return 0; fi
	case "$(resumed)" in *Stream*) say "abort: a stream is live, not ours"; exit 2 ;; esac
	mkdir -p "$BACKUP"
	a exec-out run-as $PKG cat $PREFS > "$BACKUP/prefs.xml"
	[ -s "$BACKUP/prefs.xml" ] || { say "abort: empty prefs backup"; exit 2; }
	path=$(a shell pm path $PKG | tr -d '\r' | sed -n 's/^package://p' | head -1)
	a pull "$path" "$BACKUP/base.apk" >/dev/null || { say "abort: cannot pull $path"; exit 2; }
	say "backup: prefs $(wc -c < "$BACKUP/prefs.xml") B, apk $(md5sum < "$BACKUP/base.apk" | cut -c1-16)"
}

step_ensure() {
	local want
	want=$(md5sum < "$1" | cut -d' ' -f1)
	if [ "$want" = "$(apk_md5)" ]; then say "installed APK is ours ($want)"; return 0; fi
	a shell am force-stop $PKG
	a install -r "$1" 2>&1 | tail -1 | tee -a "$LOG"
}

step_variant() { # <label> <spec>
	local label=$1 spec=$2 cur pid
	[ "$spec" = - ] && spec=""
	say "=== variant $label: vr_ui_layers='$spec'"
	arm_prefs "$OUT/prefs-$label.xml" "stream_go_vr_ui=true,stream_diagnostics_overlay=true,stream_feedback_stats_log=false"
	a shell am force-stop $PKG
	sleep 2
	write_prefs "$OUT/prefs-$label.xml" || exit 3
	a shell setprop debug.pleikkari.vr_full_pose 1
	a shell setprop debug.pleikkari.vr_pointer off
	a shell setprop debug.pleikkari.vr_ui_layers "'${spec:- }'"
	since=$(a shell "date +'%m-%d %H:%M:%S.000'" | tr -d '\r')
	: > "$OUT/$label-logcat.txt"
	a shell run-as $PKG am start --user 0 -n $PKG/.stream.StreamVrActivity \
		--ez vr_cinema_preview true --es environment plain 2>&1 | tr -d '\r' >> "$LOG"
	for _ in $(seq 1 15); do cur=$(resumed); case "$cur" in *StreamVr*) break ;; esac; sleep 1; done
	say "resumed: ${cur:-none}"
	case "$cur" in *StreamVr*) ;; *) a shell am force-stop $PKG; return 7 ;; esac
	pid=$(a shell pidof $PKG | tr -d '\r')
	echo "$pid" > "$OUT/$label-pid.txt"
	sleep 6
	mark "$label closed start"
	shot "$label-1-closed"
	sleep "$WINDOW"
	mark "$label closed end"
	a shell input keyevent KEYCODE_MENU
	sleep 1.5
	a shell setprop debug.pleikkari.vr_pointer "'-17,3'"
	sleep 0.8
	shot "$label-2-open"
	mark "$label open-idle start"
	sleep "$WINDOW"
	mark "$label open-idle end"
	dump "$OUT/$label-logcat.txt"
	a shell am force-stop $PKG
	a shell setprop debug.pleikkari.vr_pointer off
	sleep 1
	grep -c 'FATAL EXCEPTION' "$OUT/$label-logcat.txt" | sed 's/^/fatal: /' | tee -a "$LOG"
	grep -E 'VR UI layer debug|GoVrUi.*Panel [01]:' "$OUT/$label-logcat.txt" | tee -a "$LOG"
	python3 "$HERE/../PLE-722/windows.py" "$OUT/$label-logcat.txt" "$pid" | tee -a "$LOG"
	python3 "$ROOT/scripts/dev/go-latency/screen_check.py" "$OUT/$label"-*.png --logcat "$OUT/$label-logcat.txt" > "$OUT/$label-screen-check.txt" 2>&1
	say "screen_check rc=$?"
}

step_restore() {
	a shell am force-stop $PKG
	[ "$(apk_md5)" = "$(md5sum < "$BACKUP/base.apk" | cut -d' ' -f1)" ] || a install -r "$BACKUP/base.apk" 2>&1 | tail -1 | tee -a "$LOG"
	[ "$(apk_md5)" = "$(md5sum < "$BACKUP/base.apk" | cut -d' ' -f1)" ] && say "original APK back" || say "ORIGINAL APK NOT RESTORED"
	write_prefs "$BACKUP/prefs.xml" && say "prefs restored byte-identically"
	a shell setprop debug.pleikkari.vr_full_pose 0
	a shell setprop debug.pleikkari.vr_pointer off
	a shell setprop debug.pleikkari.vr_ui_layers "' '"
	a shell am force-stop $PKG
	a shell 'dumpsys activity activities | grep mResumedActivity' | tr -d '\r' | tee -a "$LOG"
}

DEADLINE=${DEADLINE:-$(( $(date +%s) + 3600 ))}
fits() { [ $(( $(date +%s) + $1 )) -le "$DEADLINE" ]; }
say "start; $(a shell 'dumpsys power | grep mWakefulness=' | tr -d '\r ')"
a shell getprop ro.serialno | tr -d '\r' > "$OUT/serial.txt"
while [ $# -gt 0 ]; do
	case "$1" in
	setup) shift; step_setup ;;
	ensure) step_ensure "$2"; shift 2 ;;
	variant) if fits $(( 2 * WINDOW + 60 )); then step_variant "$2" "$3" || say "variant $2 failed rc=$?"; else say "deferred variant $2"; fi; shift 3 ;;
	restore) shift; step_restore ;;
	*) say "unknown step $1"; exit 2 ;;
	esac
done
say "done"
