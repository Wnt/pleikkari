#!/usr/bin/env bash
# PLE-690: launch Pleikkari on the real Oculus Go the way the Library launches a VR app, with
# nobody in the headset, and record the VR entry, the connect, `VrApi cinema entered`, the live
# stream and screencaps. Run under the Go lease, from a worktree with an SDK build:
#   DEADLINE=$(( $(date +%s) + 580 )) scripts/dev/device.py run --resource go PLE-690 -- \
#       bash docs/verification/PLE-690/go-library.sh <out> <apk>
# Steps: PLE-654's go-live.sh `setup` (prefs, databases, app data, base.apk) and `ensure <apk>`
# (install -r, never uninstall or clear); then
#  entry    force-stop; the snapshot prefs plus stream_feedback_stats_log and stream_go_vr_enabled
#           (the Go's default since PLE-689); start the app process once, the way a first launch
#           after install does, so ChiakiApplication enables the Library entry; read the package
#           manager's view of it
#  launch   `go.sh am-start` the MAIN/INFO activity the package resolves to, with NEW_TASK|NO_ANIMATION:
#           the intent vrshell's sendLaunchIntent() sends for a VR package (PackageUtil.isVrApp) on a
#           Library click. The Go hands it to vrshell's desktop, which starts the entry. Observe
#           $SECS s with logcat dumps and screencaps; force-stop (never BACK); check vrshell is
#           resumed again
# and go-live.sh `restore` (the snapshot APK and prefs back). debug.pleikkari.vr_full_pose=1
# (PLE-675) keeps the screen in front of a Go lying on the table so the screencap shows it.
#
# MODE=choose (the default while build/dispatch/PS5-HOLD exists; MODE=stream is refused then) sets
# debug.pleikkari.go_entry_choose=1: the Library launch logs the console it would stream and opens
# the in-VR chooser instead, and nobody clicks it, so nothing connects. Should any GoVrEntry or
# StreamSession connect line appear anyway, the app is force-stopped at once.
set -uo pipefail
ROOT=/home/wnt/gta6
WT=$(cd "$(dirname "$0")/../../.." && pwd)
A=$ROOT/scripts/dev/device-bin/adb
G=192.168.1.202:5555
PKG=fi.madekivi.pleikkari
PREFS=shared_prefs/${PKG}_preferences.xml
OUT=${1:?out dir}
APK=${2:?apk}
HOLD=$ROOT/build/dispatch/PS5-HOLD
ENTRY=""
if [ -e "$HOLD" ]; then MODE=${MODE:-choose}; else MODE=${MODE:-stream}; fi
case "$MODE" in
	choose) SECS=${SECS:-30}; SHOT_FIRST=8; SHOT_EVERY=10 ;;
	stream) SECS=${SECS:-60}; SHOT_FIRST=20; SHOT_EVERY=20 ;;
	*) echo "MODE is choose or stream" >&2; exit 2 ;;
esac
if [ "$MODE" = stream ] && [ -e "$HOLD" ]; then
	echo "refused: $HOLD exists, never connect to the PS5 until the operator lifts it (MODE=choose proves the entry)" >&2
	exit 8
fi
DEADLINE=${DEADLINE:-$(( $(date +%s) + 580 ))}
mkdir -p "$OUT"
LOG="$OUT/session.txt"
BACKUP=$OUT/backup
a() { echo "$(date -u +%T) adb $*" >>"$OUT/adb.txt"; timeout 120 "$A" -s "$G" "$@"; }
say() { echo "$(date -u +%T) $*" | tee -a "$LOG"; }
fits() { [ $(( $(date +%s) + $1 )) -le "$DEADLINE" ]; }
resumed() { a shell 'dumpsys activity activities | grep mResumedActivity' | tr -d '\r' | sed 's/^ *//'; }
ours_anywhere() { a shell 'dumpsys activity activities' | tr -d '\r' | grep -c "ActivityRecord{[^}]* $PKG/"; }
golive() { DEADLINE=$DEADLINE BACKUP=$BACKUP bash "$WT/docs/verification/PLE-654/go-live.sh" "$OUT" "$@"; }

since=""
mark_since() { since=$(a shell "date +'%m-%d %H:%M:%S.000'" | tr -d '\r'); }
dump_log() { # append everything since the last dump to logcat-raw.txt
	local chunk last
	chunk=$(a shell "logcat -d -v threadtime -T '$since'" 2>/dev/null | tr -d '\r')
	printf '%s\n' "$chunk" >>"$OUT/logcat-raw.txt"
	last=$(printf '%s\n' "$chunk" | grep -E '^[0-9]{2}-[0-9]{2} ' | tail -1 | cut -c1-18)
	[ -n "$last" ] && since=$last
}

write_prefs() { # <file>
	a push "$1" /data/local/tmp/ple690_prefs.xml >/dev/null
	a shell "chmod 644 /data/local/tmp/ple690_prefs.xml; run-as $PKG cp /data/local/tmp/ple690_prefs.xml $PREFS; rm -f /data/local/tmp/ple690_prefs.xml"
	a exec-out run-as $PKG cat $PREFS > "$OUT/prefs-readback.xml"
	cmp -s "$1" "$OUT/prefs-readback.xml" || { say "prefs readback mismatch for $1"; return 1; }
}

step_entry() {
	local battery n
	battery=$(a shell dumpsys battery | tr -d '\r' | sed -n 's/^ *level: //p')
	say "battery ${battery}%"
	[ "${battery:-0}" -ge 15 ] || { say "abort: battery below 15%"; return 2; }
	case "$(a shell 'dumpsys window | grep mCurrentFocus' | tr -d '\r')" in
		*"Application Error"*|*"Application Not Responding"*) say "abort: a system error dialog holds focus; run go.sh recover"; return 2 ;;
	esac
	a shell am force-stop $PKG
	sleep 2
	n=$(ours_anywhere)
	[ "$n" = 0 ] || { say "abort: $n activity records of ours still exist"; return 2; }
	python3 - "$BACKUP/prefs.xml" "$OUT/prefs-run.xml" <<'EOF'
import re, sys
src, dst = sys.argv[1:]
text = open(src).read()
for key in ("stream_feedback_stats_log", "stream_go_vr_enabled"):
    text = re.sub(r'\s*<boolean name="%s" value="[a-z]+" />' % key, "", text)
    text = text.replace("</map>", '    <boolean name="%s" value="true" />\n</map>' % key)
open(dst, "w").write(text)
EOF
	write_prefs "$OUT/prefs-run.xml" || return 3
	say "prefs: snapshot + stream_feedback_stats_log=true + stream_go_vr_enabled=true; last_console_mac: $(grep -c last_console_mac "$OUT/prefs-run.xml")"
	a shell "run-as $PKG sh -c 'ls databases; echo'" | tr -d '\r' | tr '\n' ' ' | sed 's/^/databases: /' | tee -a "$LOG"; echo | tee -a "$LOG"
	mark_since
	echo "== entry $since" >> "$OUT/markers.txt"
	# A first launch after install starts the process, and ChiakiApplication enables the entry. Start
	# the process the quietest way: StreamVrActivity with no extras finishes in onCreate.
	a shell "run-as $PKG am start --user 0 -W -n $PKG/.stream.StreamVrActivity" 2>&1 | tr -d '\r' | tee -a "$LOG"
	sleep 3
	a shell am force-stop $PKG
	dump_log
	grep -E ' (GoVrSupport|GoVrEntry|AndroidRuntime): ' "$OUT/logcat-raw.txt" | tee -a "$LOG"
	a shell dumpsys package $PKG | tr -d '\r' | grep -A3 -E 'enabledComponents|disabledComponents' | tee "$OUT/components.txt" | tee -a "$LOG"
	grep -q GoVrLibraryEntry "$OUT/components.txt" || { say "the Library entry is not enabled"; return 4; }
	a shell "cmd package resolve-activity -a android.intent.action.MAIN -c android.intent.category.INFO $PKG" 2>&1 | tr -d '\r' | grep -E 'name=|packageName=|Activity' | head -4 | sed 's/^/resolve INFO: /' | tee -a "$LOG"
	ENTRY=$(sed -n 's/^resolve INFO: *name=fi\.madekivi\.pleikkari\(\..*\)$/\1/p' "$LOG" | tail -1)
	say "MAIN/INFO resolves to ${ENTRY:-nothing}"
	a shell "cmd package query-activities -a android.intent.action.MAIN -c com.oculus.intent.category.VR $PKG" 2>&1 | tr -d '\r' | grep -E 'name=' | head -4 | sed 's/^/query VR: /' | tee -a "$LOG"
}

connected() { # a connect started (it must not in MODE=choose)
	grep -E ' GoVrEntry: Connecting to | StreamSession: |Starting session request|: Wakeup sent| GoVrEntry: .*wake-up sent' "$OUT/logcat-raw.txt" | head -3
}

step_launch() {
	local out cur i shots=0 t0 sess leak
	a shell setprop debug.pleikkari.vr_full_pose 1
	if [ "$MODE" = choose ]; then
		a shell setprop debug.pleikkari.go_entry_choose 1
		[ "$(a shell getprop debug.pleikkari.go_entry_choose | tr -d '\r')" = 1 ] || { say "abort: debug.pleikkari.go_entry_choose did not stick"; a shell setprop debug.pleikkari.vr_full_pose 0; return 7; }
		say "MODE=choose: debug.pleikkari.go_entry_choose=1, the entry opens its chooser and never connects on its own"
	fi
	"$ROOT/scripts/dev/go-keepawake.sh" wake 2>&1 | tail -2 | tee -a "$LOG"
	mark_since
	echo "== launch $since" >> "$OUT/markers.txt"
	# vrshell's sendLaunchIntent() starts getLaunchIntentForPackage(): the MAIN/INFO activity the
	# package manager resolves, with NEW_TASK|NO_ANIMATION. The Go's ActivityManager does not start
	# it directly: it hands the start to vrshell's desktop (apk://com.oculus.vrshell.desktop with
	# uri=vrdesktop://<pkg>/<activity>), which starts the entry itself as uid 1000 (PLE-690, 08:29 UTC).
	# A bare `apk://<pkg>` deep link to vrshell launches nothing.
	[ -n "$ENTRY" ] || { say "abort: MAIN/INFO resolves to nothing"; a shell setprop debug.pleikkari.vr_full_pose 0; a shell setprop debug.pleikkari.go_entry_choose 0; return 5; }
	say "launch: MAIN/INFO $ENTRY with NEW_TASK|NO_ANIMATION, the intent sendLaunchIntent() sends"
	out=$("$ROOT/scripts/dev/go.sh" am-start -a android.intent.action.MAIN -c android.intent.category.INFO \
		-n "$PKG/$ENTRY" -f 0x10010000 2>&1 | tr -d '\r')
	echo "$out" | tee -a "$LOG"
	case "$out" in *"kept for the user"*|*refus*) say "am start refused"; a shell setprop debug.pleikkari.vr_full_pose 0; a shell setprop debug.pleikkari.go_entry_choose 0; return 5 ;; esac
	for i in $(seq 1 15); do
		sleep 1
		cur=$(resumed)
		case "$cur" in *"$PKG/"*) break ;; esac
	done
	say "resumed after ${i}s: ${cur:-none}"
	dump_log
	if grep -q "uri = vrdesktop://$PKG/" "$OUT/logcat-raw.txt" && grep -qE "START u0 .*cmp=$PKG/[^ ]*GoVrLibraryEntry.* from uid 1000 " "$OUT/logcat-raw.txt"; then
		echo "vrshell-desktop" > "$OUT/launch-path.txt"
		say "launch path: the Go handed the start to vrshell's desktop, which started the entry as uid 1000"
	else
		echo "direct" > "$OUT/launch-path.txt"
		say "launch path: started directly (no vrshell desktop hand-off in logcat)"
	fi
	a shell 'dumpsys activity activities' | tr -d '\r' > "$OUT/activities.txt"
	case "$cur" in *"$PKG/"*) ;; *) dump_log; a shell am force-stop $PKG; a shell setprop debug.pleikkari.vr_full_pose 0; a shell setprop debug.pleikkari.go_entry_choose 0; return 6 ;; esac
	# No input at all while the stream is up (AGENTS.md rule 12).
	t0=$(date +%s)
	while [ $(( $(date +%s) - t0 )) -lt "$SECS" ]; do
		sleep 5
		dump_log
		if [ "$MODE" = choose ]; then
			leak=$(connected)
			if [ -n "$leak" ]; then
				a shell am force-stop $PKG
				say "ABORT: a connect started in MODE=choose; force-stopped: $leak"
				break
			fi
		fi
		if [ $(( $(date +%s) - t0 )) -ge $(( SHOT_FIRST + shots * SHOT_EVERY )) ] && [ "$shots" -lt 2 ]; then
			shots=$((shots + 1))
			a exec-out screencap -p > "$OUT/shot-$shots.png" 2>/dev/null
			say "screencap $shots: $(stat -c %s "$OUT/shot-$shots.png") B"
		fi
	done
	cur=$(resumed)
	say "after ${SECS}s: ${cur:-none}"
	a shell am force-stop $PKG
	a shell setprop debug.pleikkari.vr_full_pose 0
	a shell setprop debug.pleikkari.go_entry_choose 0
	sleep 3
	dump_log
	for i in 1 2 3 4 5; do cur=$(resumed); case "$cur" in *vrshell*) break ;; esac; sleep 2; done
	say "after force-stop: ${cur:-none}"
	case "$cur" in *vrshell*) ;; *) "$ROOT/scripts/dev/go.sh" recover 2>&1 | tail -3 | tee -a "$LOG"; say "after recover: $(resumed)" ;; esac
	sess=$(a shell "run-as $PKG ls -t files/session_logs" | tr -d '\r' | head -1)
	a exec-out run-as $PKG cat "files/session_logs/$sess" > "$OUT/session.log"
	say "session log $sess ($(wc -c < "$OUT/session.log") B)"
	awk '!s[$0]++' "$OUT/logcat-raw.txt" > "$OUT/logcat.txt"
	grep -E ' (GoCinema|GoVrSupport|GoVrEntry): ' "$OUT/logcat.txt" > "$OUT/entry-cinema.txt"
	grep -E 'sendLaunchIntent|LaunchOrRestore|isVrApp|START u0 .*pleikkari' "$OUT/logcat.txt" > "$OUT/vrshell-launch.txt"
	grep -E ' VrApi ' "$OUT/logcat.txt" | grep 'FPS=' > "$OUT/vrapi-fps.txt"
	grep -E 'FATAL EXCEPTION' -A12 "$OUT/logcat.txt" | head -60 > "$OUT/crashes.txt"
	[ "$MODE" = choose ] && say "MODE=choose connect lines (must be none): $(connected | wc -l)"
	say "entry/cinema lines $(wc -l < "$OUT/entry-cinema.txt"), vrshell launch lines $(wc -l < "$OUT/vrshell-launch.txt"), Cinema video lines $(grep -c 'Cinema video:' "$OUT/entry-cinema.txt"), Feedback stats lines $(grep -c 'Feedback stats' "$OUT/logcat.txt"), fatal $(grep -c 'FATAL EXCEPTION' "$OUT/crashes.txt")"
	write_prefs "$BACKUP/prefs.xml" && say "original prefs back"
}

say "PLE-690 start, MODE=$MODE; $(( DEADLINE - $(date +%s) ))s before the deadline; serial $(a shell getprop ro.serialno | tr -d '\r'); $(a shell 'dumpsys power | grep mWakefulness=' | tr -d '\r ')"
fits 400 || { say "deferred: under 400 s left for setup, install, launch and restore"; exit 9; }
golive setup ensure "$APK" || { say "setup/ensure failed"; exit 3; }
if step_entry; then
	if fits $(( SECS + 130 )); then step_launch || say "launch failed rc=$?"; else say "launch deferred: no time left"; fi
else
	say "entry failed rc=$?"
fi
golive restore
say "done"
