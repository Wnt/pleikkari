#!/usr/bin/env bash
# PLE-698: the Go cinema's per-frame latency on the real Oculus Go, in one session under the lease.
#  preview  the debug preview (no console). The arm writes the snapshot prefs plus the stats log
#           and its keys, and starts StreamVrActivity with vr_cinema_preview as the app's uid on
#           --user 0 (the VrApi cinema, never the 2D app on display 0). Then
#           scripts/dev/go-latency/run.sh captures and summarises the window (--allow-no-stream,
#           and --no-session-log, because the newest session log is an older stream's). Last it
#           force-stops the app and puts the prefs back.
#  live     a live PS5-466 stream in the cinema, started the PLE-654 way by run.sh --start-stream
#           (go_stream.sh: stats log on, prefs put back after). Skipped while the PS5 hold
#           (build/dispatch/PS5-HOLD) exists; go_stream.sh refuses then too (PLE-706).
# Run under the Go lease, from a worktree with an SDK build (third_party/ovr_sdk_mobile/README.md):
#   DEADLINE=$(( $(date +%s) + 560 )) scripts/dev/device.py run --resource go PLE-N -- \
#       bash docs/verification/PLE-698/go-session.sh <out> setup ensure <apk> \
#       preview p72 "" preview p60 "stream_go_vr_match_60hz=true" restore
#   ... live l72 "" live l60 "stream_go_vr_match_60hz=true" restore   (after the PS5 hold)
# Steps: setup | ensure <apk> | preview|live <label> <key=value,...> | restore  (see PLE-654/go-live.sh)
# Env: GO_LATENCY (default the workspace's scripts/dev/go-latency), SECS (window, default 30),
#      ENVIRONMENT (preview room, default plain), DEADLINE (epoch s), MIN_SESSION (s).
set -uo pipefail
ROOT=/home/wnt/gta6
A=$ROOT/scripts/dev/device-bin/adb
G=192.168.1.202:5555
PKG=fi.madekivi.pleikkari
PREFS=shared_prefs/${PKG}_preferences.xml
GO_LATENCY=${GO_LATENCY:-$ROOT/scripts/dev/go-latency}
OUT=${1:?out dir}; shift
SECS=${SECS:-30}
ENVIRONMENT=${ENVIRONMENT:-plain}
mkdir -p "$OUT"
LOG="$OUT/session.txt"
BACKUP=${BACKUP:-$OUT/backup}
[ -n "${PLEIKKARI_GO_LEASE_TOKEN:-}" ] || { echo "run under scripts/dev/device.py run --resource go" >&2; exit 2; }
a() { echo "$(date -u +%T) adb $*" >>"$OUT/adb.txt"; timeout 120 "$A" -s "$G" "$@"; }
say() { echo "$(date -u +%T) $*" | tee -a "$LOG"; }
resumed() { a shell 'dumpsys activity activities | grep mResumedActivity' | tr -d '\r' | grep -o "$PKG/[^ ]*" | head -1; }
state() { a shell 'dumpsys activity activities | grep -E "mResumedActivity|mFocusedActivity"; dumpsys window | grep mCurrentFocus' | tr -d '\r' | sed 's/^ */    /'; }
ours_anywhere() { a shell 'dumpsys activity activities' | tr -d '\r' | grep -c "ActivityRecord{[^}]* $PKG/"; }

write_prefs() { # <file>
	a push "$1" /data/local/tmp/ple698_prefs.xml >/dev/null
	a shell "chmod 644 /data/local/tmp/ple698_prefs.xml; run-as $PKG cp /data/local/tmp/ple698_prefs.xml $PREFS; rm -f /data/local/tmp/ple698_prefs.xml"
	a exec-out run-as $PKG cat $PREFS > "$OUT/prefs-readback.xml"
	cmp -s "$1" "$OUT/prefs-readback.xml" || { say "prefs readback mismatch for $1"; return 1; }
}

arm_prefs() { # <label> <spec> -> $OUT/prefs-<label>.xml
	python3 - "$BACKUP/prefs.xml" "$OUT/prefs-$1.xml" "stream_feedback_stats_log=true,$2" <<'EOF'
import re, sys
src, dst, spec = sys.argv[1:]
text = open(src).read()
for item in filter(None, spec.split(",")):
    key, value = item.split("=", 1)
    text = re.sub(r'\s*<boolean name="%s" value="[a-z]+" />' % re.escape(key), "", text)
    text = re.sub(r'\s*<string name="%s">[^<]*</string>' % re.escape(key), "", text)
    if value.startswith("s:"):
        text = text.replace("</map>", '    <string name="%s">%s</string>\n</map>' % (key, value[2:]))
    else:
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
	say "backup: prefs $(wc -c < "$BACKUP/prefs.xml") B, db $(ls "$BACKUP/databases" | tr '\n' ' '), appdata $(wc -c < "$BACKUP/appdata.tar") B, apk $(sha256sum "$BACKUP/base.apk" | cut -c1-16); $(tr '\n' ' ' < "$BACKUP/package.txt")"
}

step_ensure() { # <apk>
	local want have path
	want=$(md5sum < "$1" | cut -d' ' -f1)
	path=$(a shell pm path $PKG | tr -d '\r' | sed -n 's/^package://p' | head -1)
	have=$(a shell md5sum "$path" | tr -d '\r' | cut -d' ' -f1)
	if [ "$want" = "$have" ]; then say "installed APK is ours ($want)"; return 0; fi
	say "installed APK $have is not ours ($want, $(sha256sum "$1" | cut -c1-16)): install -r"
	a shell am force-stop $PKG
	a install -r "$1" 2>&1 | tail -1 | tee -a "$LOG"
	a shell dumpsys package $PKG | tr -d '\r' | grep -E "versionCode|lastUpdateTime" | tee -a "$LOG"
}

step_preview() { # <label> <spec>
	local label=$1 spec=$2 cur n out
	say "=== preview $label ($spec), environment $ENVIRONMENT"
	[ -e "$ROOT/build/dispatch/PS5-HOLD" ] && say "PS5 hold is on; the preview connects to nothing"
	cur=$(resumed)
	case "$cur" in *Stream*) say "abort: a stream is live ($cur)"; exit 2 ;; esac
	arm_prefs "$label" "$spec"
	a shell am force-stop $PKG
	sleep 2
	n=$(ours_anywhere)
	[ "$n" = 0 ] || { say "abort: $n activity records of ours still exist"; exit 2; }
	write_prefs "$OUT/prefs-$label.xml" || exit 3
	# As go_stream.sh does for a stream (PLE-675, PLE-702): place the screen along the full pose, so
	# a Go on the table has the picture in view and run.sh's near-black screencap check can pass.
	local full_pose
	full_pose=$(a shell getprop debug.pleikkari.vr_full_pose | tr -d '\r')
	a shell setprop debug.pleikkari.vr_full_pose 1
	since=$(a shell "date +'%m-%d %H:%M:%S.000'" | tr -d '\r')
	out=$(a shell run-as $PKG am start --user 0 -n $PKG/.stream.StreamVrActivity \
		--ez vr_cinema_preview true --es environment "$ENVIRONMENT" 2>&1 | tr -d '\r')
	echo "$out" >> "$LOG"
	case "$out" in *Error*|*"not exported"*|*Exception*) say "am start refused: $(echo "$out" | tail -2 | tr '\n' ' ')"
		a shell setprop debug.pleikkari.vr_full_pose "${full_pose:-0}"; write_prefs "$BACKUP/prefs.xml"; return 4 ;; esac
	for _ in $(seq 1 15); do cur=$(resumed); case "$cur" in *StreamVr*) break ;; esac; sleep 1; done
	say "resumed: ${cur:-none}"
	case "$cur" in *StreamVr*) ;; *) state | tee -a "$LOG"; a shell am force-stop $PKG
		a shell setprop debug.pleikkari.vr_full_pose "${full_pose:-0}"; write_prefs "$BACKUP/prefs.xml"; return 7 ;; esac
	sleep 6 # the first latency window starts with the cinema; let frames flow
	mkdir -p "$OUT/$label/stream"
	# The cinema's start lines (refresh rate, clock check) come before run.sh's window. go_stats.py
	# and the screen check read them where go_stream.sh leaves a started stream's.
	a shell "logcat -d -v threadtime -T '$since' -s GoCinema:I Chiaki:I" | tr -d '\r' > "$OUT/$label-start-logcat.txt"
	cp "$OUT/$label-start-logcat.txt" "$OUT/$label/stream/start-logcat.txt"
	PLEIKKARI_DEVICE_TICKET=PLE-698 timeout $(( SECS + 120 )) "$GO_LATENCY/run.sh" "native-preview-$label" \
		--allow-no-stream --no-session-log --seconds "$SECS" --out "$OUT/$label" > "$OUT/$label-run.txt" 2>&1
	say "run.sh rc=$? (1 with a summary: see $label/screen-check.txt)"
	cur=$(resumed)
	say "after the window: ${cur:-none}"
	a shell am force-stop $PKG
	a shell setprop debug.pleikkari.vr_full_pose "${full_pose:-0}"
	sleep 2
	say "Cinema latency lines: $(grep -c 'Cinema latency:' "$OUT/$label/go-logcat.txt"); $(grep -m1 'VrApi clock minus' "$OUT/$label-start-logcat.txt" | sed 's/.*GoCinema: //')"
	grep -E 'FATAL EXCEPTION' -A12 "$OUT/$label/go-logcat.txt" | head -40 > "$OUT/$label-crashes.txt"
	write_prefs "$BACKUP/prefs.xml" && say "original prefs back"
}

step_live() { # <label> <spec>
	local label=$1 spec=$2 item cur
	local -a prefs=()
	say "=== live $label ($spec)"
	if [ -e "$ROOT/build/dispatch/PS5-HOLD" ]; then say "skipped live $label: the PS5 is on hold ($(head -1 "$ROOT/build/dispatch/PS5-HOLD"))"; return 0; fi
	cur=$(resumed)
	case "$cur" in *Stream*) say "abort: a stream is live ($cur)"; exit 2 ;; esac
	for item in ${spec//,/ }; do prefs+=(--pref "$item"); done
	PLEIKKARI_DEVICE_TICKET=PLE-698 timeout $(( SECS + 240 )) "$GO_LATENCY/run.sh" "native-$label" --start-stream \
		"${prefs[@]}" --seconds "$SECS" --out "$OUT/$label" > "$OUT/$label-run.txt" 2>&1
	say "run.sh rc=$?"
	a shell am force-stop $PKG
	say "Cinema latency lines: $(grep -c 'Cinema latency:' "$OUT/$label/go-logcat.txt" 2>/dev/null)"
	sed -n '/Cinema per-frame latency/,/^$/p' "$OUT/$label/summary.txt" 2>/dev/null | tee -a "$LOG"
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
	a shell dumpsys package $PKG | tr -d '\r' | grep -E "versionCode|lastUpdateTime" | tee -a "$LOG"
	a shell am force-stop $PKG
	state | tee -a "$LOG"
}

# DEADLINE (epoch s): a step starts only if its worst case fits before it.
DEADLINE=${DEADLINE:-$(( $(date +%s) + 3600 ))}
fits() { [ $(( $(date +%s) + $1 )) -le "$DEADLINE" ]; }
awake() { [ "$(a shell 'dumpsys power' | tr -d '\r' | sed -n 's/^ *mWakefulness=//p' | head -1)" = Awake ]; }
# MIN_SESSION (s): when the lease came too late for the whole session, touch nothing and exit 9.
fits "${MIN_SESSION:-0}" || { echo "$(date -u +%T) lease came with $(( DEADLINE - $(date +%s) ))s left; not starting" | tee -a "$LOG"; exit 9; }
say "start; $(( DEADLINE - $(date +%s) ))s before the deadline; $(a shell 'dumpsys power | grep mWakefulness=' | tr -d '\r ')"
state | tee -a "$LOG"
a shell getprop ro.build.fingerprint | tr -d '\r' > "$OUT/fingerprint.txt"
a shell getprop ro.serialno | tr -d '\r' > "$OUT/serial.txt"
asleep=0
while [ $# -gt 0 ]; do
	case "$1" in
	setup) shift; fits 150 || { say "deferred: no time for setup"; exit 9; }; step_setup ;;
	ensure) if fits 150; then step_ensure "$2"; else say "deferred ensure"; fi; shift 2 ;;
	preview) if [ "$asleep" = 1 ]; then say "skipped preview $2: the Go is asleep"
		elif ! awake; then asleep=1; say "stop: the Go is asleep before preview $2; skipping it and every later one"
		elif fits $(( SECS + 150 )); then step_preview "$2" "$3" || say "preview $2 failed rc=$?"; else say "deferred preview $2"; fi; shift 3 ;;
	live) if [ "$asleep" = 1 ]; then say "skipped live $2: the Go is asleep"
		elif ! awake; then asleep=1; say "stop: the Go is asleep before live $2; skipping it and every later one"
		elif fits $(( SECS + 240 )); then step_live "$2" "$3" || say "live $2 failed rc=$?"; else say "deferred live $2"; fi; shift 3 ;;
	restore) shift; step_restore ;;
	*) say "unknown step $1"; exit 2 ;;
	esac
done
[ "$asleep" = 1 ] && { say "done early: the Go fell asleep; later previews were not run"; exit 10; }
say "done"
