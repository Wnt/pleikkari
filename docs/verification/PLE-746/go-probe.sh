#!/usr/bin/env bash
# PLE-746: input-to-photon rounds on the real Oculus Go, in the native cinema, against PS5-466.
# Run each session under a short Go lease, from a worktree with an SDK build:
#   BACKUP=<dir kept across sessions> DEADLINE=$(( $(date +%s) + 570 )) \
#     scripts/dev/device.py run --resource go PLE-746 -- \
#       bash docs/verification/PLE-746/go-probe.sh <out> <apk> <mode>
# Modes:
#  look     stream, screencap, then for each PADS token ("down", "cross", "ps:1500", ...) press that
#           DualSense button through the debug pad broadcast and screencap again (STEP_WAIT s later).
#           This is how the PS5 is parked on its stimulus toggle. Pad input reaches the console: use it
#           only in PS5 Settings.
#  rounds   stream, PADS as in look (if any), then ROUNDS rounds of PRESSES Cross presses (PRESS, below),
#           each under its own `atrace --async_start` capture; pulls the probe's presses.csv/frames.csv.
#           A round starts only if it fits before DEADLINE. Keep PRESSES even so the toggle ends where
#           it began.
# Stimulus: Settings > Accessibility > Display and Sound > High Contrast, cursor on its toggle. Each
# Cross flips the whole UI between its normal and high-contrast colours (GPU mean luma about 40 vs 21,
# a fade of about 200 ms). Invert Colours, the ticket's first choice, only changes the console's HDMI
# output: Remote Play's video never shows it (2026-09-27, build/ple746/r1-rounds-130039).
# PLE-803: STIMULUS picks the button a round presses and the probe times (debug.pleikkari.probe_buttons,
# set for the session and cleared at its end); park the console with PADS first:
#  high-contrast (default)  Cross on the High Contrast toggle, above. The fallback.
#  create      Create on any screen: the PS5's Create menu opens (the picture dims under it) and the next
#              press closes it. Keep PRESSES even.
#  home-focus  right and left in turn on the PS5 home screen, focus on a game tile with another beside it:
#              the tile focus and the background art change. Presses alternate, so PRESSES even ends where
#              it began.
#  PRESS=key works only with high-contrast (it sends KEYCODE_BUTTON_A).
#  restore  PLE-654's restore: the snapshot APK and prefs back.
# Every streaming mode: PLE-654's go-live.sh `setup` (prefs, databases, app data and base.apk into
# $BACKUP, once) and `ensure <apk>` (install -r, never uninstall or clear); the snapshot prefs plus
# stream_feedback_stats_log, stream_go_vr_enabled and stream_go_vr_latency_probe, with the A/B keys
# in BASELINE_DROP removed so their defaults apply; the Go Library VR entry (PLE-690/PLE-717, the
# route that works until PLE-729); debug.pleikkari.vr_full_pose=1 so a Go on the table has the
# screen in its screencap. The stream ends with the cinema menu's Disconnect (PLE-739 broadcast:
# back, then right), so the cinema stops and closes the probe's files before the force-stop; the
# snapshot prefs go back and vr_full_pose goes to 0 at the end of every session.
# Since PLE-722 the debug `key` broadcast drives the VR UI (right is D-pad down), so a stream ends
# with a force-stop: presses.csv is flushed per row, frames.csv every 64 rows (under a second).
set -uo pipefail
ROOT=/home/wnt/gta6
WT=$(cd "$(dirname "$0")/../../.." && pwd)
A=$ROOT/scripts/dev/device-bin/adb
G=192.168.1.202:5555
PKG=fi.madekivi.pleikkari
PREFS=shared_prefs/${PKG}_preferences.xml
PROBE_DIR=/sdcard/Android/data/$PKG/files/latency-probe
DEBUG_INPUT=fi.madekivi.pleikkari.DEBUG_GO_VR_INPUT
OUT=${1:?out dir}
APK=${2:?apk}
MODE=${3:?look, rounds or restore}
BACKUP=${BACKUP:?BACKUP: a backup dir kept across the sessions of this ticket}
ROUNDS=${ROUNDS:-3}
PRESSES=${PRESSES:-32}
PRESS_SLEEP=${PRESS_SLEEP:-0.6} # plus the press command's own time: 1-2.5 s for `input`/`am` on the Go
# PRESS=pad (default): the debug pad broadcast holds Cross PRESS_HOLD_MS, and the PS5 takes every press.
# PRESS=key: `input keyevent KEYCODE_BUTTON_A`, the real KeyEvent path (presses.csv gets its event time),
# but its down and up are about 1 ms apart and the PS5 acted on only 36 of 96 (2026-09-27, b1-rounds-132644).
PRESS=${PRESS:-pad}
PRESS_HOLD_MS=${PRESS_HOLD_MS:-100}
STIMULUS=${STIMULUS:-high-contrast}
case "$STIMULUS" in # <pad buttons pressed in turn> <chiaki button mask the probe times>
	high-contrast) STIM_PADS="cross"; STIM_MASK=0x1 ;;
	create) STIM_PADS="create"; STIM_MASK=0x2000 ;;
	home-focus) STIM_PADS="right left"; STIM_MASK=0x30 ;;
	*) echo "unknown STIMULUS $STIMULUS (high-contrast, create, home-focus)" >&2; exit 2 ;;
esac
[ "$PRESS" = key ] && [ "$STIMULUS" != high-contrast ] && { echo "PRESS=key needs STIMULUS=high-contrast" >&2; exit 2; }
ATRACE_CATS=${ATRACE_CATS:-gfx input view sched freq hal}
ATRACE_KB=${ATRACE_KB:-16384}
PADS=${PADS:-}
STEP_WAIT=${STEP_WAIT:-1.5}
BASELINE_DROP=${BASELINE_DROP:-stream_go_vr_match_60hz stream_go_vr_room_high_gpu stream_go_vr_frame_listener_thread stream_decoder_qcom_vt_low_latency}
DEADLINE=${DEADLINE:-$(( $(date +%s) + 570 ))}
mkdir -p "$OUT"
LOG="$OUT/session.txt"
a() { echo "$(date -u +%T) adb $*" >>"$OUT/adb.txt"; timeout 120 "$A" -s "$G" "$@"; }
say() { echo "$(date -u +%T) $*" | tee -a "$LOG"; }
fits() { [ $(( $(date +%s) + $1 )) -le "$DEADLINE" ]; }
resumed() { a shell 'dumpsys activity activities | grep mResumedActivity' | tr -d '\r' | sed 's/^ *//'; }
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
	a push "$1" /data/local/tmp/ple746_prefs.xml >/dev/null
	a shell "chmod 644 /data/local/tmp/ple746_prefs.xml; run-as $PKG cp /data/local/tmp/ple746_prefs.xml $PREFS; rm -f /data/local/tmp/ple746_prefs.xml"
	a exec-out run-as $PKG cat $PREFS > "$OUT/prefs-readback.xml"
	cmp -s "$1" "$OUT/prefs-readback.xml" || { say "prefs readback mismatch for $1"; return 1; }
}

cleanup_props() {
	a shell setprop debug.pleikkari.vr_full_pose 0
	a shell setprop debug.pleikkari.go_entry_choose 0
	a shell setprop debug.pleikkari.probe_buttons "''"
}

step_prefs() {
	python3 - "$BACKUP/prefs.xml" "$OUT/prefs-run.xml" "$BASELINE_DROP" <<'EOF' | tee -a "$LOG"
import re, sys
src, dst, drop = sys.argv[1:]
text = open(src).read()
for key in drop.split():
    found = re.search(r'<(boolean|string) name="%s"[^\n]*' % re.escape(key), text)
    if found:
        print("baseline: dropped %s (was: %s)" % (key, found.group(0).strip()))
    text = re.sub(r'\s*<boolean name="%s" value="[a-z]+" />' % re.escape(key), "", text)
    text = re.sub(r'\s*<string name="%s">[^<]*</string>' % re.escape(key), "", text)
for key in ("stream_feedback_stats_log", "stream_go_vr_enabled", "stream_go_vr_latency_probe"):
    text = re.sub(r'\s*<boolean name="%s" value="[a-z]+" />' % key, "", text)
    text = text.replace("</map>", '    <boolean name="%s" value="true" />\n</map>' % key)
open(dst, "w").write(text)
# The stream's own settings, for the summary's setup section.
for line in text.splitlines():
    if re.search(r'name="(stream_|preferences_video|video_|resolution|fps|codec|vr_)', line) or "stream" in line.lower() and "name=" in line:
        print("pref: " + line.strip())
EOF
	write_prefs "$OUT/prefs-run.xml"
}

step_launch() {
	local out cur i entry
	a shell am force-stop $PKG
	sleep 2
	# A first launch after install starts the process, and ChiakiApplication enables the Library entry.
	# No -W: this activity finishes in onCreate, and `am start -W` then waited out adb's 120 s.
	a shell "run-as $PKG am start --user 0 -n $PKG/.stream.StreamVrActivity" >/dev/null 2>&1
	sleep 3
	a shell am force-stop $PKG
	# The ActivityInfo's name, not the ApplicationInfo's (.ChiakiApplication) further down.
	entry=$(a shell "cmd package resolve-activity -a android.intent.action.MAIN -c android.intent.category.INFO $PKG" 2>&1 | tr -d '\r' | grep -o 'name=fi\.madekivi\.pleikkari\.stream\.GoVrLibraryEntry' | head -1 | sed 's/^name=fi\.madekivi\.pleikkari//')
	[ -n "$entry" ] || { say "abort: MAIN/INFO does not resolve to the Library entry (.stream.GoVrLibraryEntry)"; return 5; }
	a shell setprop debug.pleikkari.vr_full_pose 1
	a shell setprop debug.pleikkari.probe_buttons $STIM_MASK # read when the cinema starts the probe
	"$ROOT/scripts/dev/go-keepawake.sh" wake 2>&1 | tail -1 | tee -a "$LOG"
	mark_since
	echo "== launch $since" >> "$OUT/markers.txt"
	out=$("$ROOT/scripts/dev/go.sh" am-start -a android.intent.action.MAIN -c android.intent.category.INFO \
		-n "$PKG/$entry" -f 0x10010000 2>&1 | tr -d '\r')
	echo "$out" >> "$LOG"
	case "$out" in
		*"kept for the user"*|*refus*)
			# PLE-717: a stale secure vrshell layer can refuse go.sh am-start after a force-stop; the plain
			# start is the same intent.
			say "go.sh am-start refused; the same intent with a plain am start"
			a shell am start -a android.intent.action.MAIN -c android.intent.category.INFO -n "$PKG/$entry" -f 0x10010000 2>&1 | tr -d '\r' >> "$LOG" ;;
	esac
	for i in $(seq 1 15); do
		sleep 1
		cur=$(resumed)
		case "$cur" in *"$PKG/"*) break ;; esac
	done
	say "resumed after ${i}s: ${cur:-none}"
	case "$cur" in *"$PKG/"*) ;; *) dump_log; return 6 ;; esac
	# Streaming means frames latched, not an activity on top (capture-wait-for-stats-line).
	for i in $(seq 1 12); do
		sleep 3
		dump_log
		if grep -E 'GoCinema: Cinema video: [1-9][0-9]* decoder frames latched' "$OUT/logcat-raw.txt" >/dev/null; then
			say "streaming after $(( i * 3 ))s: $(grep -E 'GoCinema: Cinema video:' "$OUT/logcat-raw.txt" | tail -1 | sed 's/.*Cinema video: //')"
			grep -E 'Latency probe|VrApi cinema entered|Clock levels|Environment ' "$OUT/logcat-raw.txt" | sed 's/^/  /' | tee -a "$LOG"
			return 0
		fi
	done
	say "no latched video after 36 s"
	return 7
}

shot() { # <name>
	a exec-out screencap -p > "$OUT/$1.png" 2>/dev/null
	say "screencap $1: $(stat -c %s "$OUT/$1.png") B; $(python3 "$ROOT/scripts/dev/go-latency/screen_check.py" "$OUT/$1.png" 2>&1 | tail -1)"
}

step_look() {
	local i=0 token name hold
	shot shot-00-start
	for token in $PADS; do
		i=$((i + 1))
		name=${token%%:*}
		hold=120
		case "$token" in *:*) hold=${token#*:} ;; esac
		fits 60 || { say "look: out of time before $token"; break; }
		a shell am broadcast -a $DEBUG_INPUT --es pad "$name" --ei hold_ms "$hold" >/dev/null
		sleep "$STEP_WAIT"
		shot "$(printf 'shot-%02d-%s' "$i" "$name")"
	done
	dump_log
}

step_rounds() {
	local r
	a shell atrace --list_categories 2>&1 | tr -d '\r' > "$OUT/atrace-categories.txt"
	sleep 5 # past the stream's start-up: IDR, bitrate ramp, the cinema's first placement
	# PADS first, if any: park the cursor on the stimulus toggle (shot-before-rounds shows where it is).
	[ -n "$PADS" ] && step_look
	shot shot-before-rounds
	for r in $(seq 1 "$ROUNDS"); do
		fits $(( PRESSES * 2 + 60 )) || { say "round $r: out of time"; break; }
		dump_log
		echo "== round $r $(a shell "date +'%m-%d %H:%M:%S.000'" | tr -d '\r')" >> "$OUT/markers.txt"
		a shell "atrace --async_start -b $ATRACE_KB -a $PKG $ATRACE_CATS" 2>&1 | tr -d '\r' | tail -2 | sed "s/^/round $r atrace start: /" | tee -a "$LOG"
		sleep 1
		if [ "$PRESS" = key ]; then
			say "round $r: $PRESSES presses of KEYCODE_BUTTON_A, one each $PRESS_SLEEP s plus the input command's own time"
			a shell "i=0; while [ \$i -lt $PRESSES ]; do input keyevent KEYCODE_BUTTON_A; sleep $PRESS_SLEEP; i=\$((i+1)); done"
		else
			say "round $r: $PRESSES $STIMULUS presses ($STIM_PADS in turn) held $PRESS_HOLD_MS ms (debug pad broadcast), one each $PRESS_SLEEP s plus the am command's own time"
			a shell "i=0; while [ \$i -lt $PRESSES ]; do for b in $STIM_PADS; do [ \$i -lt $PRESSES ] || break; am broadcast -a $DEBUG_INPUT --es pad \$b --ei hold_ms $PRESS_HOLD_MS >/dev/null; sleep $PRESS_SLEEP; i=\$((i+1)); done; done"
		fi
		sleep 2
		a exec-out "atrace --async_stop -z -b $ATRACE_KB -a $PKG $ATRACE_CATS" > "$OUT/round-$r.atrace" 2>/dev/null
		say "round $r done: atrace $(stat -c %s "$OUT/round-$r.atrace") B"
		dump_log
		sleep 2
	done
	shot shot-after-rounds
}

step_stop() {
	local dir
	sleep 2 # frames.csv's last flush
	dump_log
	a shell am force-stop $PKG
	cleanup_props
	sleep 2
	dump_log
	# Every cinema of this session wrote its own directory (the Disconnect's chooser starts another);
	# pull them all and keep the stream's, the one with the most frames, as probe/*.csv.
	local best="" most=-1 n
	for dir in $(grep -o 'Latency probe: writing [^ ]*' "$OUT/logcat-raw.txt" | sed 's/.*latency-probe\///' | sort -u); do
		mkdir -p "$OUT/probe/$dir"
		a pull "$PROBE_DIR/$dir/presses.csv" "$OUT/probe/$dir/presses.csv" >/dev/null 2>&1
		a pull "$PROBE_DIR/$dir/frames.csv" "$OUT/probe/$dir/frames.csv" >/dev/null 2>&1
		n=$(( $(cat "$OUT/probe/$dir/frames.csv" 2>/dev/null | wc -l) - 1 ))
		say "probe $dir: $(( $(cat "$OUT/probe/$dir/presses.csv" 2>/dev/null | wc -l) - 1 )) presses, $n frames"
		[ "$n" -gt "$most" ] && { most=$n; best=$dir; }
	done
	if [ -n "$best" ]; then
		cp "$OUT/probe/$best/presses.csv" "$OUT/probe/$best/frames.csv" "$OUT/probe/"
		echo "$best" > "$OUT/probe/dir.txt"
	else
		say "no probe directory written this session"
	fi
	awk '!s[$0]++' "$OUT/logcat-raw.txt" > "$OUT/logcat.txt"
	grep -E ' (GoCinema|GoVrEntry|Chiaki|VrApi) *: ' "$OUT/logcat.txt" | grep -E 'Latency probe|Cinema video|Cinema latency|Debug pad|Debug input|FPS=|Screen placed|entered' > "$OUT/probe-lines.txt"
	grep -E 'FATAL EXCEPTION' -A12 "$OUT/logcat.txt" | head -60 > "$OUT/crashes.txt"
	say "fatal $(grep -c 'FATAL EXCEPTION' "$OUT/crashes.txt"), Cinema latency lines $(grep -c 'Cinema latency' "$OUT/probe-lines.txt")"
	write_prefs "$BACKUP/prefs.xml" && say "snapshot prefs back"
	local cur
	for _ in 1 2 3 4 5; do cur=$(resumed); case "$cur" in *vrshell*) break ;; esac; sleep 2; done
	say "after stop: ${cur:-none}"
	case "$cur" in *vrshell*) ;; *) "$ROOT/scripts/dev/go.sh" recover 2>&1 | tail -3 | tee -a "$LOG"; say "after recover: $(resumed)" ;; esac
}

say "PLE-746 $MODE (STIMULUS=$STIMULUS); $(( DEADLINE - $(date +%s) ))s before the deadline; serial $(a shell getprop ro.serialno | tr -d '\r'); $(a shell 'dumpsys power | grep mWakefulness=' | tr -d '\r ')"
a shell getprop ro.serialno | tr -d '\r' > "$OUT/serial.txt"
a shell getprop ro.build.fingerprint | tr -d '\r' > "$OUT/fingerprint.txt"
if [ "$MODE" = restore ]; then
	golive restore
	cleanup_props
	say "after restore: $(resumed)"
	exit 0
fi
"$ROOT/scripts/dev/ps5-hold.sh" check || { say "refused: the PS5 is held (build/dispatch/PS5-HOLD)"; exit 8; }
MIN_SESSION=${MIN_SESSION:-240}
fits "$MIN_SESSION" || { say "deferred: under $MIN_SESSION s left"; exit 9; }
# The Go queue is FIFO and leases are short: the last holder can leave its StreamVrActivity up. Under
# this lease nobody else uses the Go, so log what it was and stop it (force-stop keeps app data).
leftover=$(resumed)
case "$leftover" in *"$PKG/"*)
	say "leftover from an earlier lease: $leftover; last cinema lines:"
	a shell "logcat -d -v time -t 400" | tr -d '\r' | grep -E 'GoCinema|GoVrEntry' | tail -3 | sed 's/^/  /' | tee -a "$LOG"
	a shell am force-stop $PKG
	sleep 2
	say "after force-stop: $(resumed)" ;;
esac
golive setup ensure "$APK" || { say "setup/ensure failed"; exit 3; }
step_prefs || { say "prefs failed"; golive restore; exit 3; }
if step_launch; then
	case "$MODE" in
		look) step_look ;;
		rounds) step_rounds ;;
		*) say "unknown mode $MODE" ;;
	esac
else
	say "launch failed rc=$?"
fi
step_stop
say "done"
