#!/usr/bin/env bash
# PLE-730: prove VR Home (console cards and status sheets) on the real Oculus Go, launched the way the
# Library launches it, with nobody in the headset. One session under the Go lease:
#   DEADLINE=$(( $(date +%s) + 560 )) scripts/dev/device.py run --resource go PLE-730 -- \
#       bash docs/verification/PLE-730/go-home.sh <out> <apk>
# Steps: PLE-654's go-live.sh `setup` (prefs, databases, app data, base.apk) and `ensure <apk>` (install
# -r, never uninstall or clear); then
#  entry      the snapshot prefs plus stream_go_vr_enabled, stream_go_vr_ui and the stats log; start the
#             process once so ChiakiApplication enables the Library entry (PLE-690)
#  home       debug.pleikkari.go_entry_choose=1 (the Library launch opens Home's cards, no connect of
#             its own) and vr_full_pose=1 (the panel in front of a Go on a table). Screencaps: the
#             cards; the pad's focus (PLE-739's debug broadcast, whose touchpad thirds VR Home maps to
#             the D-pad and A); the debug pointer on a card and on its Play pill; Back opening the menu
#             over Home, and Back again. With LIVE=1 (the default unless build/dispatch/PS5-HOLD
#             exists): A plays the focused card, screencaps of the status sheet and of the stream once
#             `GoVrUi: Home closed` shows, then force-stop (never BACK)
#  noconsole  debug.pleikkari.go_entry_no_console=1 (PLE-739): the no-console sheet
# and go-live.sh `restore`. Every debug property goes back to 0/off, and the Go must end on vrshell.
# Output: logcat.txt, home.txt (GoVrUi/GoVrEntry/GoCinema lines), vrapi-fps.txt, shot-*.png and
# screen-check.txt (scripts/dev/go-latency/screen_check.py).
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
if [ -e "$HOLD" ]; then LIVE=0; else LIVE=${LIVE:-1}; fi
DEADLINE=${DEADLINE:-$(( $(date +%s) + 560 ))}
[ -n "${PLEIKKARI_GO_LEASE_TOKEN:-}" ] || { echo "run under scripts/dev/device.py run --resource go" >&2; exit 2; }
mkdir -p "$OUT"
LOG="$OUT/session.txt"
BACKUP=$OUT/backup
ENTRY=""
a() { echo "$(date -u +%T) adb $*" >>"$OUT/adb.txt"; timeout 120 "$A" -s "$G" "$@"; }
say() { echo "$(date -u +%T) $*" | tee -a "$LOG"; }
fits() { [ $(( $(date +%s) + $1 )) -le "$DEADLINE" ]; }
resumed() { a shell 'dumpsys activity activities | grep mResumedActivity' | tr -d '\r' | sed 's/^ *//'; }
ours_anywhere() { a shell 'dumpsys activity activities' | tr -d '\r' | grep -c "ActivityRecord{[^}]* $PKG/"; }
golive() { DEADLINE=$DEADLINE BACKUP=$BACKUP bash "$WT/docs/verification/PLE-654/go-live.sh" "$OUT" "$@"; }
props_off() {
	a shell setprop debug.pleikkari.vr_full_pose 0
	a shell setprop debug.pleikkari.go_entry_choose 0
	a shell setprop debug.pleikkari.go_entry_no_console 0
	a shell setprop debug.pleikkari.vr_pointer off
}

since=""
mark_since() { since=$(a shell "date +'%m-%d %H:%M:%S.000'" | tr -d '\r'); }
dump_log() {
	local chunk last
	chunk=$(a shell "logcat -d -v threadtime -T '$since'" 2>/dev/null | tr -d '\r')
	printf '%s\n' "$chunk" >>"$OUT/logcat-raw.txt"
	last=$(printf '%s\n' "$chunk" | grep -E '^[0-9]{2}-[0-9]{2} ' | tail -1 | cut -c1-18)
	[ -n "$last" ] && since=$last
}
shot() { # <name>
	a exec-out screencap -p > "$OUT/shot-$1.png" 2>/dev/null
	say "screencap $1: $(stat -c %s "$OUT/shot-$1.png") B"
}
# PLE-739's debug broadcast: back toggles the menu (or pops a Home sheet); left/right/centre are the
# touchpad thirds, which VR Home takes as the pad's D-pad up/down and A.
key() { a shell am broadcast -a fi.madekivi.pleikkari.DEBUG_GO_VR_INPUT --es key "$1" >/dev/null; say "key $1"; sleep 1; }
pointer() { a shell setprop debug.pleikkari.vr_pointer "'$1'"; say "pointer $1"; sleep 1; }

write_prefs() { # <file>
	a push "$1" /data/local/tmp/ple730_prefs.xml >/dev/null
	a shell "chmod 644 /data/local/tmp/ple730_prefs.xml; run-as $PKG cp /data/local/tmp/ple730_prefs.xml $PREFS; rm -f /data/local/tmp/ple730_prefs.xml"
	a exec-out run-as $PKG cat $PREFS > "$OUT/prefs-readback.xml"
	cmp -s "$1" "$OUT/prefs-readback.xml" || { say "prefs readback mismatch for $1"; return 1; }
}

step_entry() {
	local battery n
	battery=$(a shell dumpsys battery | tr -d '\r' | sed -n 's/^ *level: //p')
	say "battery ${battery}%"
	[ "${battery:-0}" -ge 15 ] || { say "abort: battery below 15%"; return 2; }
	a shell am force-stop $PKG
	sleep 2
	n=$(ours_anywhere)
	[ "$n" = 0 ] || { say "abort: $n activity records of ours still exist"; return 2; }
	python3 - "$BACKUP/prefs.xml" "$OUT/prefs-run.xml" <<'EOF'
import re, sys
src, dst = sys.argv[1:]
text = open(src).read()
for key in ("stream_feedback_stats_log", "stream_go_vr_enabled", "stream_go_vr_ui"):
    text = re.sub(r'\s*<boolean name="%s" value="[a-z]+" />' % key, "", text)
    text = text.replace("</map>", '    <boolean name="%s" value="true" />\n</map>' % key)
open(dst, "w").write(text)
EOF
	write_prefs "$OUT/prefs-run.xml" || return 3
	say "prefs: snapshot + stream_feedback_stats_log, stream_go_vr_enabled, stream_go_vr_ui = true; last_console_mac: $(grep -c last_console_mac "$OUT/prefs-run.xml")"
	mark_since
	a shell "run-as $PKG am start --user 0 -W -n $PKG/.stream.StreamVrActivity" 2>&1 | tr -d '\r' >> "$LOG"
	sleep 3
	a shell am force-stop $PKG
	dump_log
	a shell dumpsys package $PKG | tr -d '\r' | grep -A3 -E 'enabledComponents|disabledComponents' > "$OUT/components.txt"
	grep -q GoVrLibraryEntry "$OUT/components.txt" || { say "the Library entry is not enabled"; return 4; }
	ENTRY=$(a shell "cmd package resolve-activity -a android.intent.action.MAIN -c android.intent.category.INFO $PKG" 2>&1 |
		tr -d '\r' | sed -n 's/^ *name=fi\.madekivi\.pleikkari\(\..*\)$/\1/p' | head -1)
	say "MAIN/INFO resolves to ${ENTRY:-nothing}"
	[ -n "$ENTRY" ]
}

# The intent vrshell's sendLaunchIntent() sends on a Library click; the Go hands it to vrshell's desktop.
launch() { # <label>
	local out cur i
	"$ROOT/scripts/dev/go-keepawake.sh" wake 2>&1 | tail -1 >> "$LOG"
	mark_since
	echo "== $1 $since" >> "$OUT/markers.txt"
	out=$("$ROOT/scripts/dev/go.sh" am-start -a android.intent.action.MAIN -c android.intent.category.INFO \
		-n "$PKG/$ENTRY" -f 0x10010000 2>&1 | tr -d '\r')
	echo "$out" >> "$LOG"
	case "$out" in *"kept for the user"*|*refus*) say "$1: go.sh am-start refused"; return 5 ;; esac
	for i in $(seq 1 15); do
		sleep 1
		cur=$(resumed)
		case "$cur" in *"$PKG/"*) break ;; esac
	done
	say "$1: resumed after ${i}s: ${cur:-none}"
	case "$cur" in *"$PKG/"*) return 0 ;; esac
	return 6
}

stop_app() {
	local cur i
	a shell am force-stop $PKG
	sleep 3
	for i in 1 2 3 4 5; do cur=$(resumed); case "$cur" in *vrshell*) break ;; esac; sleep 2; done
	say "after force-stop: ${cur:-none}"
	case "$cur" in *vrshell*) ;; *) "$ROOT/scripts/dev/go.sh" recover 2>&1 | tail -3 >> "$LOG"; say "after recover: $(resumed)" ;; esac
}

step_home() {
	local i
	a shell setprop debug.pleikkari.vr_full_pose 1
	a shell setprop debug.pleikkari.go_entry_choose 1
	a shell setprop debug.pleikkari.vr_pointer off
	launch home || { dump_log; stop_app; return 6; }
	sleep 6
	shot 1-cards
	key right          # the pad's first move: focus on the last-played card
	shot 2-pad-focus
	# The panel is 50 x 28 degrees; the first card's middle is about 6.9 degrees above the panel's.
	pointer "-12,6.9"
	shot 3-pointer-card
	pointer "19,6.9"
	shot 4-pointer-play
	pointer off
	key back           # the menu over Home
	shot 5-menu-over-home
	key back           # Home again
	dump_log
	if [ "$LIVE" = 1 ] && fits 90; then
		key right      # the menu reset the pad's focus: on the last-played card again
		key centre     # A on it: Play
		sleep 0.5
		shot 6-status-sheet
		# Home closes on StreamStateConnected.
		for i in $(seq 1 30); do
			sleep 1
			dump_log
			grep -qE 'GoVrUi *: Home closed' "$OUT/logcat-raw.txt" && break
		done
		say "Home closed after ${i}s: $(grep -cE 'GoVrUi *: Home closed' "$OUT/logcat-raw.txt")"
		sleep 8
		shot 7-stream
		dump_log
	else
		say "no Play: LIVE=$LIVE"
	fi
	stop_app
	a shell setprop debug.pleikkari.go_entry_choose 0
	dump_log
}

step_noconsole() {
	a shell setprop debug.pleikkari.go_entry_no_console 1
	launch noconsole || { dump_log; stop_app; a shell setprop debug.pleikkari.go_entry_no_console 0; return 6; }
	sleep 6
	shot 8-no-console
	key right
	shot 9-no-console-focus
	stop_app
	a shell setprop debug.pleikkari.go_entry_no_console 0
	dump_log
}

say "PLE-730 start, LIVE=$LIVE; $(( DEADLINE - $(date +%s) ))s before the deadline; serial $(a shell getprop ro.serialno | tr -d '\r'); $(a shell 'dumpsys power | grep mWakefulness=' | tr -d '\r ')"
a shell getprop ro.serialno | tr -d '\r' > "$OUT/serial.txt"
fits 300 || { say "deferred: under 300 s left for setup, install, Home and restore"; exit 9; }
golive setup ensure "$APK" || { say "setup/ensure failed"; exit 3; }
if step_entry; then
	if fits 170; then step_home || say "home failed rc=$?"; else say "home deferred: no time left"; fi
	if fits 80; then step_noconsole || say "noconsole failed rc=$?"; else say "noconsole deferred: no time left"; fi
else
	say "entry failed rc=$?"
fi
props_off
write_prefs "$BACKUP/prefs.xml" && say "original prefs back"
golive restore
say "debug properties: full_pose=$(a shell getprop debug.pleikkari.vr_full_pose | tr -d '\r') choose=$(a shell getprop debug.pleikkari.go_entry_choose | tr -d '\r') no_console=$(a shell getprop debug.pleikkari.go_entry_no_console | tr -d '\r') pointer=$(a shell getprop debug.pleikkari.vr_pointer | tr -d '\r')"
say "resumed at the end: $(resumed)"
awk '!s[$0]++' "$OUT/logcat-raw.txt" > "$OUT/logcat.txt"
# logcat pads a short tag: "GoVrUi  : ".
grep -E ' (GoVrUi|GoVrEntry|GoCinema) *: ' "$OUT/logcat.txt" | grep -v 'Frame pacing' > "$OUT/home.txt"
grep -E ' VrApi ' "$OUT/logcat.txt" | grep 'FPS=' > "$OUT/vrapi-fps.txt"
grep -E 'FATAL EXCEPTION' -A12 "$OUT/logcat.txt" | head -60 > "$OUT/crashes.txt"
python3 "$ROOT/scripts/dev/go-latency/screen_check.py" "$OUT"/shot-*.png --logcat "$OUT/logcat.txt" > "$OUT/screen-check.txt" 2>&1
rc=$?
say "GoVrUi lines $(grep -cE ' GoVrUi *: ' "$OUT/home.txt"), fatal $(grep -c 'FATAL EXCEPTION' "$OUT/crashes.txt"), screen_check rc=$rc"
say "done"
