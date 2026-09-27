#!/usr/bin/env bash
# PLE-715: map VrApi's frame scheduler on the real Oculus Go with the cinema's pacing trace, in one
# session under the Go lease, from a worktree with an SDK build (third_party/ovr_sdk_mobile/README.md):
#   DEADLINE=$(( $(date +%s) + 560 )) scripts/dev/device.py run --resource go PLE-N -- \
#       bash docs/verification/PLE-715/go-pacing.sh <out> setup ensure <apk> \
#       preview p72-base "" "trace" preview p72-sweep "" "trace,sweep=500/144/13500" restore
# Steps: setup | ensure <apk> | restore (PLE-654's go-live.sh, one backup for the session);
#  preview <label> <prefs> <pacing>  the debug preview (no console) with the snapshot prefs, the stats
#           log and <prefs> (key=true|false,...), and debug.pleikkari.vr_pacing=<pacing> (see
#           vr-frame-pacing.h). The app is started as its own uid on --user 0; logcat streams to the
#           host for $SECS s (the per-frame trace outruns the Go's 256 KB buffer), then force-stop.
#  live <label> <prefs> <pacing>     the same around a live PS5-466 stream started the way the Go
#           Library starts the app (PLE-690's MAIN/INFO GoVrLibraryEntry through vrshell's desktop;
#           the entry streams the last used console). Refused while ps5-hold.sh holds the PS5.
# The property is put back to its old value and the prefs back to the snapshot after every arm.
# Env: SECS (window, default 30; a label <name>@<s> sets one arm's), ENVIRONMENT (preview room, default plain), DEADLINE (epoch s).
set -uo pipefail
ROOT=/home/wnt/gta6
WT=$(cd "$(dirname "$0")/../../.." && pwd)
A=$ROOT/scripts/dev/device-bin/adb
G=192.168.1.202:5555
PKG=fi.madekivi.pleikkari
PREFS=shared_prefs/${PKG}_preferences.xml
PROP=debug.pleikkari.vr_pacing
OUT=${1:?out dir}; shift
SECS=${SECS:-30}
SECS_DEFAULT=$SECS
ENVIRONMENT=${ENVIRONMENT:-plain}
mkdir -p "$OUT"
LOG="$OUT/session.txt"
BACKUP=${BACKUP:-$OUT/backup}
[ -n "${PLEIKKARI_GO_LEASE_TOKEN:-}" ] || { echo "run under scripts/dev/device.py run --resource go" >&2; exit 2; }
a() { echo "$(date -u +%T) adb $*" >>"$OUT/adb.txt"; timeout 120 "$A" -s "$G" "$@"; }
say() { echo "$(date -u +%T) $*" | tee -a "$LOG"; }
resumed() { a shell 'dumpsys activity activities | grep mResumedActivity' | tr -d '\r' | sed 's/^ *//'; }
ours_anywhere() { a shell 'dumpsys activity activities' | tr -d '\r' | grep -c "ActivityRecord{[^}]* $PKG/"; }
golive() { DEADLINE=$DEADLINE BACKUP=$BACKUP bash "$WT/docs/verification/PLE-654/go-live.sh" "$OUT" "$@"; }
DEADLINE=${DEADLINE:-$(( $(date +%s) + 3600 ))}
fits() { [ $(( $(date +%s) + $1 )) -le "$DEADLINE" ]; }
awake() { [ "$(a shell 'dumpsys power' | tr -d '\r' | sed -n 's/^ *mWakefulness=//p' | head -1)" = Awake ]; }

write_prefs() { # <file>
	a push "$1" /data/local/tmp/ple715_prefs.xml >/dev/null
	a shell "chmod 644 /data/local/tmp/ple715_prefs.xml; run-as $PKG cp /data/local/tmp/ple715_prefs.xml $PREFS; rm -f /data/local/tmp/ple715_prefs.xml"
	a exec-out run-as $PKG cat $PREFS > "$OUT/prefs-readback.xml"
	cmp -s "$1" "$OUT/prefs-readback.xml" || { say "prefs readback mismatch for $1"; return 1; }
}

arm_prefs() { # <label> <spec> -> $OUT/prefs-<label>.xml; the Library entry needs stream_go_vr_enabled
	python3 - "$BACKUP/prefs.xml" "$OUT/prefs-$1.xml" "stream_feedback_stats_log=true,stream_go_vr_enabled=true,$2" <<'EOF'
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

set_prop() { # <value>: an empty value clears it
	a shell "setprop $PROP '$1'"
	[ "$(a shell getprop $PROP | tr -d '\r')" = "$1" ] || { say "abort: $PROP did not take '$1'"; return 1; }
}

old_prop=""
arm_start() { # <label> <prefs> <pacing>
	local label=$1 n
	case "$(resumed)" in *Stream*) say "abort: a stream is live, not ours"; exit 2 ;; esac
	arm_prefs "$label" "$2"
	a shell am force-stop $PKG
	sleep 2
	n=$(ours_anywhere)
	[ "$n" = 0 ] || { say "abort: $n activity records of ours still exist"; exit 2; }
	write_prefs "$OUT/prefs-$label.xml" || exit 3
	old_prop=$(a shell getprop $PROP | tr -d '\r')
	set_prop "$3" || exit 3
	mkdir -p "$OUT/$label"
	since=$(a shell "date +'%m-%d %H:%M:%S.000'" | tr -d '\r')
	echo "$since" > "$OUT/$label/since.txt"
}

arm_capture() { # <label>: stream logcat since the arm's start for $SECS s
	local label=$1
	timeout "$SECS" "$A" -s "$G" logcat -v threadtime -T "$since" -s GoPacing:I GoCinema:I VrApi:I Chiaki:I GoVrEntry:I AndroidRuntime:E \
		| tr -d '\r' > "$OUT/$label/logcat.txt"
	say "$label: $(grep -c ' GoPacing: F ' "$OUT/$label/logcat.txt") trace lines, $(grep -c 'Frame pacing (' "$OUT/$label/logcat.txt") pacing windows, $(grep -c 'FPS=' "$OUT/$label/logcat.txt") VrApi lines, $(grep -c 'FATAL' "$OUT/$label/logcat.txt") fatal"
}

arm_end() { # <label>
	a shell am force-stop $PKG
	set_prop "$old_prop" || say "WARNING: $PROP left set"
	sleep 2
	write_prefs "$BACKUP/prefs.xml" && say "$1: original prefs and $PROP='$old_prop' back"
}

step_preview() { # <label> <prefs> <pacing>
	local label=$1 cur out i
	say "=== preview $label prefs '$2' pacing '$3' environment $ENVIRONMENT"
	arm_start "$@"
	out=$(a shell run-as $PKG am start --user 0 -n $PKG/.stream.StreamVrActivity \
		--ez vr_cinema_preview true --es environment "$ENVIRONMENT" 2>&1 | tr -d '\r')
	echo "$out" >> "$LOG"
	case "$out" in *Error*|*"not exported"*|*Exception*) say "am start refused"; arm_end "$label"; return 4 ;; esac
	for i in $(seq 1 15); do cur=$(resumed); case "$cur" in *StreamVr*) break ;; esac; sleep 1; done
	say "resumed: ${cur:-none}"
	case "$cur" in *StreamVr*) arm_capture "$label" ;; *) arm_end "$label"; return 7 ;; esac
	arm_end "$label"
}

step_live() { # <label> <prefs> <pacing>
	local label=$1 cur out i
	say "=== live $label prefs '$2' pacing '$3'"
	"$ROOT/scripts/dev/ps5-hold.sh" check >/dev/null 2>&1 || { say "skipped live $label: the PS5 is on hold"; return 0; }
	arm_start "$@"
	"$ROOT/scripts/dev/go-keepawake.sh" wake 2>&1 | tail -1 | tee -a "$LOG"
	# The intent vrshell's sendLaunchIntent() sends for a VR package on a Library click (PLE-690).
	out=$("$ROOT/scripts/dev/go.sh" am-start -a android.intent.action.MAIN -c android.intent.category.INFO \
		-n "$PKG/.stream.GoVrLibraryEntry" -f 0x10010000 2>&1 | tr -d '\r')
	echo "$out" >> "$LOG"
	case "$out" in *"kept for the user"*|*refus*) say "am start refused: $(echo "$out" | tail -1)"; arm_end "$label"; return 5 ;; esac
	for i in $(seq 1 15); do cur=$(resumed); case "$cur" in *"$PKG/"*) break ;; esac; sleep 1; done
	say "resumed after ${i}s: ${cur:-none}"
	case "$cur" in *"$PKG/"*) arm_capture "$label" ;; *) arm_end "$label"; return 6 ;; esac
	# No input at all while the stream is up (AGENTS.md rule 12); end it with force-stop, never BACK.
	arm_end "$label"
	for i in 1 2 3 4 5; do cur=$(resumed); case "$cur" in *vrshell*) break ;; esac; sleep 2; done
	case "$cur" in *vrshell*) ;; *) "$ROOT/scripts/dev/go.sh" recover 2>&1 | tail -2 | tee -a "$LOG" ;; esac
	say "$label: after force-stop $(resumed)"
}

say "start; $(( DEADLINE - $(date +%s) ))s before the deadline; serial $(a shell getprop ro.serialno | tr -d '\r'); $(a shell 'dumpsys power | grep mWakefulness=' | tr -d '\r ')"
a shell getprop ro.build.fingerprint | tr -d '\r' > "$OUT/fingerprint.txt"
a shell getprop ro.serialno | tr -d '\r' > "$OUT/serial.txt"
while [ $# -gt 0 ]; do
	case "$1" in
	setup) shift; fits 150 || { say "deferred: no time for setup"; exit 9; }; golive setup ;;
	ensure) if fits 150; then golive ensure "$2"; else say "deferred ensure"; fi; shift 2 ;;
	preview|live)
		# <label>@<s> gives that arm its own window.
		label=${2%@*}; SECS=$SECS_DEFAULT; case "$2" in *@*) SECS=${2##*@} ;; esac
		if ! awake; then say "stop: the Go is asleep before $1 $label"; shift 4; continue; fi
		if fits $(( SECS + 60 )); then "step_$1" "$label" "$3" "$4" || say "$1 $label failed rc=$?"; else say "deferred $1 $label"; fi
		shift 4 ;;
	restore) shift; golive restore ;;
	*) say "unknown step $1"; exit 2 ;;
	esac
done
say "done"
