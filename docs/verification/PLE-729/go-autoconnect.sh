#!/usr/bin/env bash
# PLE-729: does PLE-659's auto_connect_host reach a live PS5-466 stream on the real Oculus Go,
# APK by APK? Run under the Go lease, from this worktree:
#   DEADLINE=$(( $(date +%s) + 570 )) scripts/dev/device.py run --resource go PLE-729 -- \
#       bash docs/verification/PLE-729/go-autoconnect.sh <out> \
#       setup try resident native 30 ensure <apk> try head native 45 restore
# Steps (setup, ensure and restore are PLE-654's go-live.sh):
#  setup                      snapshot prefs, databases, the whole app data and the installed base.apk
#  ensure <apk>               install -r <apk> unless it is installed already (never uninstall or clear)
#  try <label> <path> <secs>  scripts/dev/go-latency/go_stream.sh start --path <path> --timeout <secs>,
#                             with its own state dir. Live: keep it HOLD s (default 30), screencap
#                             half-way (a native start sets debug.pleikkari.vr_full_pose=1, PLE-702)
#                             and run screen_check.py on it. Always go_stream.sh stop, the logcat
#                             since the launch, and the Go back at vrshell.
#  library <label> <secs>     PLE-690's Library VR launch, the MAIN/INFO GoVrLibraryEntry intent
#                             vrshell's sendLaunchIntent() sends (PLE-717's route), with
#                             debug.pleikkari.vr_full_pose=1: its own auto-connect to the last used
#                             console, a screencap half-way and screen_check.py, force-stop, the
#                             property back to 0 and the Go back at vrshell
#  prefs <file>               force-stop and write <file> as the app's prefs (a fresh-prefs run:
#                             no stream_go_vr_enabled key, the app's own default); restore puts
#                             the snapshot back
#  restore                    force-stop, the snapshot APK and prefs back (always runs)
# The session starts only with MIN_SESSION s (default 200) before DEADLINE, and each step only
# if it fits with RESTORE_S s (default 60) still left for restore; the rest is logged "deferred".
# GO_STREAM overrides the go_stream.sh that runs (for a copy under test in a workspace worktree).
# Point such a copy's PLEIKKARI_GO_ADB, PLEIKKARI_GO_SH and PLEIKKARI_GO_KEEPAWAKE at
# /home/wnt/gta6/scripts/dev: its own device-bin/adb resolves device.py, and with it the device
# queue, inside that worktree (build/device-queue there), not the shared Go FIFO.
set -uo pipefail
ROOT=/home/wnt/gta6
WT=$(cd "$(dirname "$0")/../../.." && pwd)
A=$ROOT/scripts/dev/device-bin/adb
G=192.168.1.202:5555
PKG=fi.madekivi.pleikkari
GO_STREAM=${GO_STREAM:-$ROOT/scripts/dev/go-latency/go_stream.sh}
SCREEN_CHECK=${SCREEN_CHECK:-$ROOT/scripts/dev/go-latency/screen_check.py}
HOLD=${HOLD:-30}
OUT=${1:?out dir}; shift
mkdir -p "$OUT"
LOG="$OUT/session.txt"
BACKUP=${BACKUP:-$OUT/backup}
DEADLINE=${DEADLINE:-$(( $(date +%s) + 570 ))}
# A waiter with no Bash cap over it (a detached helper) that queued past its DEADLINE: seconds in
# <out>.session-s set the deadline from the moment the lease arrives instead.
[ -s "$OUT.session-s" ] && DEADLINE=$(( $(date +%s) + $(cat "$OUT.session-s") ))
a() { echo "$(date -u +%T) adb $*" >>"$OUT/adb.txt"; timeout 120 "$A" -s "$G" "$@"; }
say() { echo "$(date -u +%T) $*" | tee -a "$LOG"; }
RESTORE_S=${RESTORE_S:-60}
MIN_SESSION=${MIN_SESSION:-200}
fits() { [ $(( $(date +%s) + $1 + RESTORE_S )) -le "$DEADLINE" ]; }
golive() { DEADLINE=$DEADLINE BACKUP=$BACKUP bash "$WT/docs/verification/PLE-654/go-live.sh" "$OUT" "$@"; }
resumed() { a shell 'dumpsys activity activities | grep mResumedActivity' | tr -d '\r' | sed 's/^ *//'; }

installed_apk() { # "<md5> <has auto_connect_host: yes|no>" of the installed base.apk
	local path md5
	path=$(a shell pm path $PKG | tr -d '\r' | sed -n 's/^package://p' | head -1)
	md5=$(a shell md5sum "$path" | tr -d '\r' | cut -d' ' -f1)
	a pull "$path" "$OUT/installed.apk" >/dev/null 2>&1
	# grep -c, not -q: under pipefail an early match SIGPIPEs unzip and reads as "no" (session s1)
	if [ "$(unzip -p "$OUT/installed.apk" 'classes*.dex' 2>/dev/null | grep -ac auto_connect_host)" -gt 0 ]; then
		echo "$md5 auto_connect_host=yes"
	else
		echo "$md5 auto_connect_host=no"
	fi
	rm -f "$OUT/installed.apk"
}

step_try() { # <label> <path> <timeout>
	local label=$1 path=$2 timeout=$3 dir=$OUT/$1 since rc cur i
	mkdir -p "$dir"
	say "=== try $label: go_stream.sh start --path $path --timeout $timeout; installed APK $(installed_apk)"
	since=$(a shell "date +'%m-%d %H:%M:%S.000'" | tr -d '\r')
	GO_STREAM_CMDLOG="$dir/commands.txt" "$GO_STREAM" start --path "$path" --timeout "$timeout" --state "$dir/state" \
		>"$dir/start.txt" 2>&1
	rc=$?
	say "start rc=$rc: $(tail -1 "$dir/start.txt")"
	if [ "$rc" = 0 ]; then
		sleep $(( HOLD / 2 ))
		a exec-out screencap -p >"$dir/shot.png" 2>/dev/null
		say "screencap $(stat -c %s "$dir/shot.png") B; resumed: $(resumed)"
		sleep $(( HOLD - HOLD / 2 ))
	fi
	a shell "logcat -d -v threadtime -T '$since'" 2>/dev/null | tr -d '\r' >"$dir/logcat.txt"
	"$GO_STREAM" stop --state "$dir/state" >"$dir/stop.txt" 2>&1
	say "stop rc=$?: $(tr '\n' ' ' <"$dir/stop.txt")"
	grep -E 'START u0 .*pleikkari|AutoConnect|auto_connect|VrApi cinema entered|Cinema video:|Feedback stats:|Login message|Session quit|FATAL EXCEPTION' \
		"$dir/logcat.txt" >"$dir/key-lines.txt"
	say "key lines: START $(grep -c 'START u0' "$dir/key-lines.txt"), AutoConnect $(grep -c 'AutoConnect' "$dir/key-lines.txt"), cinema entered $(grep -c 'VrApi cinema entered' "$dir/key-lines.txt"), Cinema video $(grep -c 'Cinema video:' "$dir/key-lines.txt"), Feedback stats $(grep -c 'Feedback stats:' "$dir/key-lines.txt"), fatal $(grep -c 'FATAL EXCEPTION' "$dir/key-lines.txt")"
	if [ -s "$dir/shot.png" ]; then
		python3 "$SCREEN_CHECK" --logcat "$dir/logcat.txt" "$dir/shot.png" >"$dir/screen-check.txt" 2>&1
		say "screen_check rc=$?: $(tr '\n' ' ' <"$dir/screen-check.txt")"
	fi
	for i in 1 2 3 4 5 6; do cur=$(resumed); case "$cur" in *vrshell*) break ;; esac; sleep 2; done
	case "$cur" in
	*vrshell*) say "Go back at vrshell: $cur" ;;
	*) "$ROOT/scripts/dev/go.sh" recover 2>&1 | tail -3 | tee -a "$LOG"; say "after recover: $(resumed)" ;;
	esac
	# Let the console drop a stopped session before the next connect (go_stream.sh's cooldown).
	[ "$rc" != 0 ] || sleep 12
}

if [ $(( DEADLINE - $(date +%s) )) -lt "$MIN_SESSION" ]; then
	say "the lease came with $(( DEADLINE - $(date +%s) ))s left (< $MIN_SESSION): nothing done"; exit 4
fi
step_library() { # <label> <secs>
	local label=$1 secs=$2 dir=$OUT/$1 since cur i
	mkdir -p "$dir"
	say "=== library $label: MAIN/INFO .stream.GoVrLibraryEntry through go.sh am-start, ${secs}s; installed APK $(installed_apk)"
	a shell am force-stop $PKG >/dev/null
	a shell setprop debug.pleikkari.vr_full_pose 1
	since=$(a shell "date +'%m-%d %H:%M:%S.000'" | tr -d '\r')
	"$ROOT/scripts/dev/go.sh" am-start -a android.intent.action.MAIN -c android.intent.category.INFO \
		-n "$PKG/.stream.GoVrLibraryEntry" -f 0x10010000 >"$dir/launch.txt" 2>&1
	say "launch: $(tr '\n' ' ' <"$dir/launch.txt")"
	sleep $(( secs / 2 ))
	a exec-out screencap -p >"$dir/shot.png" 2>/dev/null
	say "screencap $(stat -c %s "$dir/shot.png") B; resumed: $(resumed)"
	sleep $(( secs - secs / 2 ))
	a shell "logcat -d -v threadtime -T '$since'" 2>/dev/null | tr -d '\r' >"$dir/logcat.txt"
	a shell am force-stop $PKG >/dev/null
	a shell setprop debug.pleikkari.vr_full_pose 0
	grep -E 'START u0 .*pleikkari|GoVrEntry|AutoConnect|VrApi cinema entered|Cinema video:|Feedback stats:|Session quit|FATAL EXCEPTION' \
		"$dir/logcat.txt" >"$dir/key-lines.txt"
	say "key lines: Library VR launch $(grep -c 'Library VR launch' "$dir/key-lines.txt"), GoVrEntry $(grep -c 'GoVrEntry' "$dir/key-lines.txt"), cinema entered $(grep -c 'VrApi cinema entered' "$dir/key-lines.txt"), Cinema video $(grep -c 'Cinema video:' "$dir/key-lines.txt"), Feedback stats $(grep -c 'Feedback stats:' "$dir/key-lines.txt"), fatal $(grep -c 'FATAL EXCEPTION' "$dir/key-lines.txt")"
	if [ -s "$dir/shot.png" ]; then
		python3 "$SCREEN_CHECK" --logcat "$dir/logcat.txt" "$dir/shot.png" >"$dir/screen-check.txt" 2>&1
		say "screen_check rc=$?: $(tr '\n' ' ' <"$dir/screen-check.txt")"
	fi
	for i in 1 2 3 4 5 6; do cur=$(resumed); case "$cur" in *vrshell*) break ;; esac; sleep 2; done
	case "$cur" in
	*vrshell*) say "Go back at vrshell: $cur" ;;
	*) "$ROOT/scripts/dev/go.sh" recover 2>&1 | tail -3 | tee -a "$LOG"; say "after recover: $(resumed)" ;;
	esac
	sleep 12
}

say "start; $(( DEADLINE - $(date +%s) ))s before the deadline; serial $(a shell getprop ro.serialno | tr -d '\r'); $(a shell 'dumpsys power | grep mWakefulness=' | tr -d '\r ')"
a shell getprop ro.serialno | tr -d '\r' >"$OUT/serial.txt"
while [ $# -gt 0 ]; do
	case "$1" in
	setup) shift; golive setup || { say "setup failed"; exit 3; } ;;
	ensure) if fits 150; then golive ensure "$2"; else say "deferred ensure"; fi; shift 2 ;;
	try) if fits $(( $4 + HOLD + 60 )); then step_try "$2" "$3" "$4"; else say "deferred try $2"; fi; shift 4 ;;
	library) if fits $(( $3 + 50 )); then step_library "$2" "$3"; else say "deferred library $2"; fi; shift 3 ;;
	prefs) shift
		a shell am force-stop $PKG >/dev/null
		a push "$1" /data/local/tmp/ple729_prefs.xml >/dev/null
		a shell "chmod 644 /data/local/tmp/ple729_prefs.xml; run-as $PKG cp /data/local/tmp/ple729_prefs.xml shared_prefs/${PKG}_preferences.xml; rm -f /data/local/tmp/ple729_prefs.xml"
		a exec-out run-as $PKG cat shared_prefs/${PKG}_preferences.xml >"$OUT/prefs-written.xml"
		if cmp -s "$1" "$OUT/prefs-written.xml"; then say "prefs written from $1"; else say "prefs write of $1 did not read back"; fi
		shift ;;
	restore) shift; golive restore ;;
	*) say "unknown step $1"; exit 2 ;;
	esac
done
say "done"
