#!/usr/bin/env bash
# PLE-654: a live PS5 stream in the native VrApi cinema on the real Oculus Go, unattended.
# The real Connect path, no debug extra: stream_go_vr_enabled=true, MainActivity started on
# display 0, uiautomator finds the enabled playButton, input tap, and GoVrSupport.streamIntent
# routes Play to StreamVrActivity (PLE-635's display-0 recipe plus the cinema setting).
# Run under the Go lease, from a worktree with an SDK build (third_party/ovr_sdk_mobile/README.md):
#   DEADLINE=$(( $(date +%s) + 560 )) scripts/dev/device.py run --resource go PLE-N -- \
#       bash docs/verification/PLE-654/go-live.sh <out> setup ensure <apk> \
#       arm0 A-plain "stream_go_vr_enabled=true" restore
#   python3 docs/verification/PLE-654/summarize.py <out>/A-plain
# Steps: setup | ensure <apk> | arm0 <label> <key=value,...> | restore
#  setup    snapshot prefs, databases, the whole app data (run-as tar) and the installed base.apk
#  ensure   install -r <apk> unless it is already installed (never uninstall, never clear)
#  arm0     force-stop, write snapshot prefs + stats log + the arm's keys, am start MainActivity
#           on display 0, tap playButton, wait for StreamVrActivity, stream $SECS s with logcat
#           dumps and screencaps, force-stop (never BACK: the cinema turns it into its menu, and on
#           display 0 it arms vrshell's exit dialog), pull the session log, prefs back
#  restore  force-stop, reinstall the snapshot APK if the installed one differs, snapshot prefs
#           back byte-identically
# Values: key=true|false, key=s:<string>.
# PLE-696: before each arm0 the Go must report mWakefulness=Awake. If it has fallen asleep (off
# head, LED off) the arm and every later arm are skipped with "asleep", restore still runs, and
# the script exits 10 so a caller can tell a sleeping Go from a finished session.
# PLE-708: any arm0 step starts a PS5 stream, so the run exits 3 before touching adb while
# scripts/dev/ps5-hold.sh holds the console (setup/ensure/restore alone stay ungated).
set -uo pipefail
ROOT=/home/wnt/gta6
A=${PLEIKKARI_GO_ADB:-$ROOT/scripts/dev/device-bin/adb}
G=192.168.1.202:5555
PKG=fi.madekivi.pleikkari
PREFS=shared_prefs/${PKG}_preferences.xml
OUT=${1:?out dir}; shift
SECS=${SECS:-60}
case " $* " in *" arm0 "*) "$ROOT/scripts/dev/ps5-hold.sh" check || exit 3 ;; esac
mkdir -p "$OUT"
LOG="$OUT/session.txt"
BACKUP=${BACKUP:-$OUT/backup}
a() { echo "$(date -u +%T) adb $*" >>"$OUT/adb.txt"; timeout 120 "$A" -s "$G" "$@"; }
say() { echo "$(date -u +%T) $*" | tee -a "$LOG"; }
resumed() { a shell 'dumpsys activity activities | grep mResumedActivity' | tr -d '\r' | grep -o "$PKG/[^ ]*" | head -1; }
state() { a shell 'dumpsys activity activities | grep -E "mResumedActivity|mFocusedActivity"; dumpsys window | grep mCurrentFocus' | tr -d '\r' | sed 's/^ */    /'; }
ours_anywhere() { a shell 'dumpsys activity activities' | tr -d '\r' | grep -c "ActivityRecord{[^}]* $PKG/"; }

since=""
dump_log() { # append everything since the last dump to logcat-raw-<label>.txt
	local chunk last
	chunk=$(a shell "logcat -d -v threadtime -T '$since'" 2>/dev/null | tr -d '\r')
	printf '%s\n' "$chunk" >>"$OUT/logcat-raw-$1.txt"
	last=$(printf '%s\n' "$chunk" | grep -E '^[0-9]{2}-[0-9]{2} ' | tail -1 | cut -c1-18)
	[ -n "$last" ] && since=$last
}
mark_since() { since=$(a shell "date +'%m-%d %H:%M:%S.000'" | tr -d '\r'); }

write_prefs() { # <file>
	a push "$1" /data/local/tmp/ple654_prefs.xml >/dev/null
	a shell "chmod 644 /data/local/tmp/ple654_prefs.xml; run-as $PKG cp /data/local/tmp/ple654_prefs.xml $PREFS; rm -f /data/local/tmp/ple654_prefs.xml"
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

step_arm0() { # <label> <spec>
	local label=$1 spec=$2 cur n out tapped=0 i node xy shots=0 t0
	say "=== arm $label on display 0 ($spec)"
	cur=$(resumed)
	case "$cur" in *Stream*) say "abort: a stream is live ($cur)"; exit 2 ;; esac
	case "$(a shell 'dumpsys window | grep mCurrentFocus' | tr -d '\r')" in
		*"Application Error"*|*"Application Not Responding"*) say "abort: a system error dialog holds focus; run go.sh recover"; exit 2 ;;
	esac
	arm_prefs "$label" "$spec"
	a shell am force-stop $PKG
	sleep 2
	n=$(ours_anywhere)
	[ "$n" = 0 ] || { say "abort: $n activity records of ours still exist; not starting on display 0"; exit 2; }
	write_prefs "$OUT/prefs-$label.xml" || exit 3
	mark_since
	echo "== $label start $since" >> "$OUT/markers.txt"
	out=$(a shell am start -W -n $PKG/.main.MainActivity 2>&1 | tr -d '\r')
	echo "$out" >> "$LOG"
	case "$out" in *"kept for the user"*|*Error*) say "am start refused: $(echo "$out" | tail -2 | tr '\n' ' ')"; return 4 ;; esac
	for i in $(seq 1 25); do
		sleep 1
		a shell uiautomator dump /sdcard/ple654-ui.xml >/dev/null 2>&1
		a exec-out cat /sdcard/ple654-ui.xml > "$OUT/ui-$label.xml" 2>/dev/null
		node=$(grep -o '<node [^>]*resource-id="'$PKG':id/playButton"[^>]*>' "$OUT/ui-$label.xml" | head -1)
		[ -n "$node" ] || continue
		echo "$node" | grep -q 'enabled="true"' || continue
		xy=$(echo "$node" | sed -n 's/.*bounds="\[\([0-9]*\),\([0-9]*\)\]\[\([0-9]*\),\([0-9]*\)\]".*/\1 \2 \3 \4/p' | awk '{print int(($1+$3)/2), int(($2+$4)/2)}')
		[ -n "$xy" ] || continue
		say "playButton at $xy ($(echo "$node" | grep -o 'text="[^"]*"')) after ${i}s; tap"
		a shell input tap $xy
		tapped=1
		break
	done
	a shell rm -f /sdcard/ple654-ui.xml
	if [ "$tapped" != 1 ]; then
		say "no enabled playButton on display 0; $(state | tr '\n' ' ')"
		dump_log "$label"; a shell am force-stop $PKG; return 5
	fi
	for _ in $(seq 1 30); do cur=$(resumed); case "$cur" in *StreamVr*) break ;; esac; sleep 1; done
	say "resumed after tap: ${cur:-none}"
	state | tee -a "$LOG"
	case "$cur" in *StreamVr*) ;; *) dump_log "$label"; a shell am force-stop $PKG; return 7 ;; esac
	a shell 'dumpsys activity activities' | tr -d '\r' > "$OUT/activities-$label.txt"
	# No input at all while the stream is up (AGENTS.md rule 12).
	t0=$(date +%s)
	while [ $(( $(date +%s) - t0 )) -lt "$SECS" ]; do
		sleep 5
		dump_log "$label"
		if [ $(( $(date +%s) - t0 )) -ge $(( 15 + shots * 20 )) ] && [ "$shots" -lt 2 ]; then
			shots=$((shots + 1))
			a exec-out screencap -p > "$OUT/shot-$label-$shots.png" 2>/dev/null
			say "screencap $shots: $(stat -c %s "$OUT/shot-$label-$shots.png") B"
		fi
	done
	cur=$(resumed)
	say "after ${SECS}s: ${cur:-none}; Cinema video lines: $(grep -c 'Cinema video:' "$OUT/logcat-raw-$label.txt"); Feedback stats lines: $(grep -c 'Feedback stats' "$OUT/logcat-raw-$label.txt")"
	a shell am force-stop $PKG
	sleep 3
	dump_log "$label"
	state | tee -a "$LOG"
	local sess
	sess=$(a shell "run-as $PKG ls -t files/session_logs" | tr -d '\r' | head -1)
	a exec-out run-as $PKG cat "files/session_logs/$sess" > "$OUT/session-$label.log"
	say "session log $sess -> session-$label.log ($(wc -c < "$OUT/session-$label.log") B)"
	awk '!s[$0]++' "$OUT/logcat-raw-$label.txt" > "$OUT/logcat-$label.txt"
	grep -E ' (GoCinema|GoVrSupport): ' "$OUT/logcat-$label.txt" > "$OUT/gocinema-$label.txt"
	grep -E ' VrApi ' "$OUT/logcat-$label.txt" | grep 'FPS=' > "$OUT/vrapi-fps-$label.txt"
	grep -E 'FATAL EXCEPTION' -A12 "$OUT/logcat-$label.txt" | head -60 > "$OUT/crashes-$label.txt"
	say "GoCinema lines $(wc -l < "$OUT/gocinema-$label.txt"), VrApi FPS lines $(wc -l < "$OUT/vrapi-fps-$label.txt"), fatal $(grep -c 'FATAL EXCEPTION' "$OUT/crashes-$label.txt")"
	# The app is stopped: put the owner's prefs back between sessions.
	write_prefs "$BACKUP/prefs.xml" && say "original prefs back"
	# Let the console notice the dropped client before the next arm connects.
	sleep 12
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
	a shell run-as $PKG ls -la databases | tr -d '\r' | tee -a "$LOG"
	state | tee -a "$LOG"
}

# DEADLINE (epoch s): a step starts only if its worst case fits before it.
DEADLINE=${DEADLINE:-$(( $(date +%s) + 3600 ))}
fits() { [ $(( $(date +%s) + $1 )) -le "$DEADLINE" ]; }
say "start; $(( DEADLINE - $(date +%s) ))s before the deadline; $(a shell 'dumpsys power | grep mWakefulness=' | tr -d '\r ')"
state | tee -a "$LOG"
a shell getprop ro.build.fingerprint | tr -d '\r' > "$OUT/fingerprint.txt"
a shell getprop ro.serialno | tr -d '\r' > "$OUT/serial.txt"
did_setup=0
asleep=0
awake() { [ "$(a shell 'dumpsys power' | tr -d '\r' | sed -n 's/^ *mWakefulness=//p' | head -1)" = Awake ]; }
while [ $# -gt 0 ]; do
	case "$1" in
	setup) shift; fits 150 || { say "deferred: no time for setup"; exit 9; }; step_setup; did_setup=1 ;;
	ensure) if fits 150; then step_ensure "$2"; else say "deferred ensure"; fi; shift 2 ;;
	arm0) if [ "$asleep" = 1 ]; then say "skipped arm $2: the Go is asleep"
		elif ! awake; then asleep=1; say "stop: the Go is asleep before arm $2 ($(a shell 'dumpsys power | grep mWakefulness=' | tr -d '\r ')); skipping it and every later arm"
		elif fits $(( SECS + 150 )); then step_arm0 "$2" "$3" || say "arm $2 failed rc=$?"; else say "deferred arm $2"; fi; shift 3 ;;
	restore) shift; step_restore ;;
	*) say "unknown step $1"; exit 2 ;;
	esac
done
[ "$asleep" = 1 ] && { say "done early: the Go fell asleep; arms after that were not run"; exit 10; }
say "done"
