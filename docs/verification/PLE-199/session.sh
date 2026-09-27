#!/usr/bin/env bash
# PLE-199: one battery/thermal soak session for the stream_thread_priority_boost A/B.
# Replicates PLE-71's run_session.sh, sized to fit one 10-minute Bash call so the whole
# session runs under a single Samsung reservation:
#
#   device.py run PLE-199 -- session.sh <name> <prefs.xml>
#
# start (~40 s) + 9 x (50 s drive + sample) + collect (~20 s) ~= 9 min.
#
# Environment: CAP (captures dir, required), ANDROID_SERIAL (required), HARNESS (a frozen
# copy of scripts/dev/ab, default $CAP/harness/scripts/dev/ab).
set -euo pipefail

name=$1; prefs=$2
: "${CAP:?}" "${ANDROID_SERIAL:?}"
HARNESS=${HARNESS:-$CAP/harness/scripts/dev/ab}
ADB=/home/wnt/gta6/scripts/dev/device-bin/adb
PKG=fi.madekivi.pleikkari
CHUNKS=${CHUNKS:-5}
CHUNK_S=${CHUNK_S:-50}
export AB_CAPTURES=$CAP AB_ADB=$ADB AB_NET_PROFILE=clean
export AB_IMPAIRCTL=/home/wnt/gta6/scripts/net/impairctl.py
SOAK=$HARNESS/soak.sh

out=$CAP/${name}_thermal_samples.txt
: > "$out"
log() { printf '[%s] %s: %s\n' "$(date -u +%H:%M:%S)" "$name" "$*"; }

sample() {
	{
		echo "== $(date -u +%H:%M:%S) =="
		echo "-- thermalservice --"
		"$ADB" shell dumpsys thermalservice 2>/dev/null | grep -E 'Temperature\{' | sort -u || echo n/a
		echo "-- scaling_cur_freq --"
		"$ADB" shell 'cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq' 2>/dev/null || echo n/a
	} >> "$out"
}

# utime+stime of the whole app process, in clock ticks (fields 14 and 15 of /proc/pid/stat;
# the comm field has no spaces for this package).
proc_cpu() {
	local tag=$1 pid
	pid=$("$ADB" shell pidof "$PKG" | tr -d '\r')
	{
		echo "$tag $(date -u +%s) pid=$pid"
		"$ADB" shell "cat /proc/$pid/stat" | tr -d '\r'
		"$ADB" shell getconf CLK_TCK 2>/dev/null | tr -d '\r' | sed 's/^/clk_tck=/'
	} >> "$CAP/${name}_proc_cpu.txt"
}

# PLE-199: per-thread utime+stime (clock ticks) of every app thread: tid, comm, utime, stime.
# comm can contain spaces ("Chiaki Takion"), so the numeric fields are read after the ')'.
threads() {
	local tag=$1 pid
	pid=$("$ADB" shell pidof "$PKG" | tr -d '\r')
	echo "$tag $(date -u +%s) pid=$pid" >> "$CAP/${name}_threads.txt"
	"$ADB" shell "run-as $PKG sh -c 'for t in /proc/$pid/task/*; do s=\$(cat \$t/stat); r=\${s##*) }; set -- \$r; printf \"%s\\t%s\\t%s\\t%s\\n\" \$(basename \$t) \"\$(cat \$t/comm)\" \${12} \${13}; done'" \
		| tr -d '\r' >> "$CAP/${name}_threads.txt"
}

"$ADB" shell dumpsys battery | grep -E '^  (AC powered|USB powered|status|level|temperature):' > "$CAP/${name}_battery.txt"
log "battery: $(tr '\n' ' ' < "$CAP/${name}_battery.txt")"
sample

log "batterystats --reset"
"$ADB" shell dumpsys batterystats --reset >/dev/null
: > "$CAP/${name}_proc_cpu.txt"; : > "$CAP/${name}_threads.txt"

# On battery the S22 dozes into AOD and the lock screen hides the app: wake and swipe up.
"$ADB" shell input keyevent KEYCODE_WAKEUP
sleep 1
"$ADB" shell input swipe 540 1800 540 600 300
sleep 1
"$SOAK" start "$name" "$prefs"

# A resumed StreamActivity is not a live stream (PLE-356): wait for three stats lines.
ok=0
for _ in $(seq 1 30); do
	"$ADB" logcat -d | grep -q "Session has quit" && { log "session quit before stats"; exit 5; }
	[ "$("$ADB" logcat -d | grep -c 'Feedback stats')" -ge 3 ] && { ok=1; break; }
	sleep 1
done
[ "$ok" = 1 ] || { log "no Feedback stats lines"; exit 5; }

proc_cpu start
threads start
pid=$("$ADB" shell pidof "$PKG" | tr -d '\r')
# Which CPUs each thread may run on: proves the affinity applied (ON) or did not (OFF).
"$ADB" shell "run-as $PKG sh -c 'for t in /proc/$pid/task/*; do printf \"%s\t%s\t%s\n\" \$(basename \$t) \"\$(cat \$t/comm)\" \"\$(grep Cpus_allowed_list \$t/status | cut -f2)\"; done'" \
	| tr -d '\r' > "$CAP/${name}_affinity.txt"
log "threads pinned to 4-7: $(awk -F'\t' '$3=="4-7"' "$CAP/${name}_affinity.txt" | cut -f2 | tr '\n' ' ')"

sample
for _ in $(seq 1 "$CHUNKS"); do
	"$SOAK" drive "$name" "$CHUNK_S"
	sample
done
proc_cpu end
threads end

"$SOAK" collect "$name"
"$ADB" shell dumpsys batterystats --charged "$PKG" > "$CAP/${name}_batterystats.txt" 2>&1 || true
log "done"
