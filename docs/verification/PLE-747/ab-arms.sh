#!/usr/bin/env bash
# PLE-815: PLE-747's input-lag A/B on the Oculus Go in one command. It drives PLE-746's go-probe.sh
# `rounds` (PRESS=pad, STIMULUS passed through for PLE-803) over the arm matrix, one Go lease per batch.
#
#   APK=<gate arm64 debug apk> BACKUP=<dir kept across sessions> \
#     bash docs/verification/PLE-747/ab-arms.sh run <run-dir>
#   bash docs/verification/PLE-747/ab-arms.sh plan            the plan, no device (same env)
#   bash docs/verification/PLE-747/ab-arms.sh summarize <run-dir>   the summary table only
#
# A batch is one arm's streaming session: under `device.py run --resource go`, `go.sh ready` (PLE-687)
# and `go-keepawake.sh alert-text` (PLE-790) must pass, then go-probe.sh rounds runs with the arm's
# ARM_PREFS/ARM_PROPS. A failed check stops the whole run. Between batches: COOLDOWN_S without a lease,
# and LONG_COOLDOWN_S once HOT_AFTER_S of streaming has piled up (the Go overheats after about an hour).
# The arms are interleaved (ABAB: every repetition runs the whole list), so drift averages out.
# go-probe.sh restores the snapshot prefs byte-identically after every session; `finish` (the last
# lease) checks them against $BACKUP/prefs.xml once more and leaves the Go on its VR home.
#
# Env: ARMS (names from the table below; default all), REPS (2), COOLDOWN_S (300),
# LONG_COOLDOWN_S (1200), HOT_AFTER_S (2400), SESSION_S (570: one batch's go-probe DEADLINE),
# ROUNDS/PRESSES/STIMULUS/PADS as go-probe.sh, TICKET (PLE-747).
set -uo pipefail
ROOT=/home/wnt/gta6
HERE=$(cd "$(dirname "$0")" && pwd)
WT=$(cd "$HERE/../../.." && pwd)
PROBE=$WT/docs/verification/PLE-746/go-probe.sh
G=192.168.1.202:5555
PKG=fi.madekivi.pleikkari
TICKET=${TICKET:-PLE-747}
REPS=${REPS:-2}
COOLDOWN_S=${COOLDOWN_S:-300}
LONG_COOLDOWN_S=${LONG_COOLDOWN_S:-1200}
HOT_AFTER_S=${HOT_AFTER_S:-2400}
SESSION_S=${SESSION_S:-570}
# Everything the arms touch, dropped from the snapshot so control is every switch at its default.
export BASELINE_DROP=${BASELINE_DROP:-stream_go_vr_match_60hz stream_go_vr_room_high_gpu stream_go_vr_frame_listener_thread stream_decoder_qcom_vt_low_latency stream_go_vr_late_start stream_go_vr_latch_on_signal stream_go_vr_warm_up stream_go_vr_input_thread}
export PRESS=${PRESS:-pad}

# name | ARM_PREFS | ARM_PROPS (debug.pleikkari.vr_pacing needs a debug build: µs, see vr-frame-pacing.c)
ARM_TABLE='
control||
late72|stream_go_vr_late_start=true|
late60|stream_go_vr_late_start=true stream_go_vr_match_60hz=true|
late_budget75|stream_go_vr_late_start=true|debug.pleikkari.vr_pacing=late,budget=7500
late_budget90|stream_go_vr_late_start=true|debug.pleikkari.vr_pacing=late,budget=9000
listener_thread|stream_go_vr_frame_listener_thread=true|
signal|stream_go_vr_latch_on_signal=true|
signal_late|stream_go_vr_latch_on_signal=true stream_go_vr_late_start=true|
qcom_ll|stream_decoder_qcom_vt_low_latency=true|
res720|stream_resolution=720p|
h264|stream_codec=h264|
h265|stream_codec=h265|
input_thread|stream_go_vr_input_thread=true|
'
# input_thread is PLE-802's switch (landed).
ARMS=${ARMS:-control late72 late60 late_budget75 late_budget90 listener_thread signal signal_late qcom_ll res720 h264 h265 input_thread}

arm_field() { # <name> <field 2|3>
	printf '%s\n' "$ARM_TABLE" | awk -F'|' -v n="$1" -v f="$2" '$1 == n { print $f; found = 1 } END { exit !found }'
}

check_arms() {
	local arm
	for arm in $ARMS; do arm_field "$arm" 2 >/dev/null || { echo "unknown arm: $arm" >&2; return 2; }; done
}

# The batch order: every repetition runs the whole list (ABAB).
batches() {
	local r arm n=0
	for r in $(seq 1 "$REPS"); do
		for arm in $ARMS; do n=$((n + 1)); printf '%02d %s %s\n' "$n" "$r" "$arm"; done
	done
}

plan() {
	check_arms || return 2
	local total streamed=0 n r arm
	total=$(batches | wc -l)
	echo "PLE-815 arm plan: $total batches ($REPS reps x $(echo $ARMS | wc -w) arms), PRESS=$PRESS STIMULUS=${STIMULUS:-default} ROUNDS=${ROUNDS:-3} PRESSES=${PRESSES:-32}"
	echo "baseline drop: $BASELINE_DROP"
	while read -r n r arm; do
		echo "batch $n rep $r $arm: prefs [$(arm_field "$arm" 2)] props [$(arm_field "$arm" 3)]"
		echo "  lease: device.py run --resource go $TICKET -- go.sh ready; go-keepawake.sh alert-text; go-probe.sh rounds (${SESSION_S}s)"
		streamed=$((streamed + SESSION_S))
		if [ "$n" -lt "$total" ]; then
			if [ "$streamed" -ge "$HOT_AFTER_S" ]; then echo "  cooldown ${LONG_COOLDOWN_S}s (hot: ${streamed}s streamed)"; streamed=0
			else echo "  cooldown ${COOLDOWN_S}s"; fi
		fi
	done < <(batches)
	echo "finish: one lease, prefs cmp against \$BACKUP/prefs.xml, Go on its VR home"
	echo "summarize: input_to_photon.py analyze per arm, ab_summary.py -> summary.md"
}

# One batch, inside the lease: the checks, then the probe. Exit 10/11 = a check failed.
batch() { # <out> <arm>
	local out=$1 arm=$2 alert
	mkdir -p "$out"
	"$ROOT/scripts/dev/go.sh" ready > "$out/ready.txt" 2>&1 || { cat "$out/ready.txt"; return 10; }
	alert=$("$ROOT/scripts/dev/go-keepawake.sh" alert-text 2>&1 | tr -d '\r')
	printf '%s\n' "$alert" > "$out/alert-text.txt"
	[ -z "$alert" ] || { echo "alert on the Go: $alert"; return 11; }
	ARM_PREFS=$(arm_field "$arm" 2) ARM_PROPS=$(arm_field "$arm" 3) DEADLINE=$(( $(date +%s) + SESSION_S )) \
		bash "$PROBE" "$out" "$APK" rounds
}

finish() { # inside the last lease
	local A=$ROOT/scripts/dev/device-bin/adb cur
	"$A" -s "$G" exec-out run-as $PKG cat shared_prefs/${PKG}_preferences.xml > "$RUN/prefs-final.xml"
	if cmp -s "$BACKUP/prefs.xml" "$RUN/prefs-final.xml"; then echo "prefs: byte-identical to the snapshot"
	else echo "prefs: DIFFER from the snapshot"; return 12; fi
	cur=$("$A" -s "$G" shell 'dumpsys activity activities | grep mResumedActivity' | tr -d '\r')
	case "$cur" in
		*vrshell*) ;;
		*)
			"$ROOT/scripts/dev/go.sh" recover 2>&1 | tail -3
			cur=$("$A" -s "$G" shell 'dumpsys activity activities | grep mResumedActivity' | tr -d '\r')
			;;
	esac
	echo "Go at the end: $cur"
	case "$cur" in *vrshell*) return 0 ;; *) return 13 ;; esac
}

summarize() { # <run-dir>
	local run=$1 arm dirs d
	mkdir -p "$run/arms"
	for arm in $(ls "$run" | sed -n 's/^[0-9][0-9]-r[0-9]*-//p' | sort -u); do
		dirs=()
		for d in "$run"/[0-9][0-9]-r*-"$arm"; do [ -s "$d/probe/presses.csv" ] && dirs+=("$d"); done
		[ ${#dirs[@]} -gt 0 ] || { echo "$arm: no probe data"; continue; }
		python3 "$ROOT/scripts/dev/go-latency/input_to_photon.py" analyze "${dirs[@]}" --out "$run/arms/$arm" > /dev/null
		cat "${dirs[@]/%//logcat.txt}" > "$run/arms/$arm/logcat.txt" 2>/dev/null
	done
	python3 "$HERE/ab_summary.py" "$run/arms" | tee "$run/summary.md"
}

run() {
	RUN=${1:?run dir}
	APK=${APK:?APK: the arm64 debug build from gate.sh}
	export BACKUP=${BACKUP:?BACKUP: a backup dir kept across the sessions}
	check_arms || return 2
	mkdir -p "$RUN"
	plan > "$RUN/plan.txt"
	local n r arm total streamed=0 rc
	total=$(batches | wc -l)
	while read -r n r arm; do
		echo "$(date -u +%T) batch $n/$total rep $r $arm" | tee -a "$RUN/run.txt"
		# A child of this script, so the lease covers this batch only; stdin kept off the batch list.
		"$ROOT/scripts/dev/device.py" run --resource go "$TICKET" -- \
			env APK="$APK" BACKUP="$BACKUP" bash "$0" _batch "$RUN/$n-r$r-$arm" "$arm" < /dev/null 2>&1 | tail -5 | tee -a "$RUN/run.txt"
		rc=${PIPESTATUS[0]}
		[ "$rc" -eq 0 ] || { echo "$(date -u +%T) stop: batch $n rc=$rc" | tee -a "$RUN/run.txt"; break; }
		streamed=$((streamed + SESSION_S))
		[ "$n" -lt "$total" ] || break
		if [ "$streamed" -ge "$HOT_AFTER_S" ]; then sleep "$LONG_COOLDOWN_S"; streamed=0; else sleep "$COOLDOWN_S"; fi
	done < <(batches)
	"$ROOT/scripts/dev/device.py" run --resource go "$TICKET" -- \
		env BACKUP="$BACKUP" bash "$0" _finish "$RUN" < /dev/null 2>&1 | tee -a "$RUN/run.txt"
	summarize "$RUN"
}

case "${1:-}" in
	plan) plan ;;
	run) shift; run "$@" ;;
	summarize) shift; summarize "${1:?run dir}" ;;
	_batch) shift; batch "$@" ;;
	_finish) RUN=${2:?}; finish ;;
	*) sed -n '2,20p' "$0"; exit 2 ;;
esac
