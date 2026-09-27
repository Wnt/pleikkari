#!/usr/bin/env bash
# PLE-829: run one arm of the S22 A/B, joining the Samsung queue only when nobody else waits (behind
# the holder at most), with a wait budget that leaves the session its time inside one 10-minute tool
# call; s22-probe.sh refuses to start past DEADLINE.  run-arm.sh <label> <on|off> [presses]
set -uo pipefail
LABEL=$1; ARM=$2; PRESSES=${3:-40}
WT=$(cd "$(dirname "$0")/../../.." && pwd)
DEV=/home/wnt/gta6/scripts/dev/device.py
END=$(( $(date +%s) + 585 ))
NEEDED=${NEEDED:-120}
POLL_MAX=${POLL_MAX:-240}
POLL_END=$(( $(date +%s) + POLL_MAX ))
cd "$WT"
while :; do
	left=$(( END - $(date +%s) ))
	[ "$(date +%s)" -lt "$POLL_END" ] || { echo "$(date -u +%T) the queue never emptied in ${POLL_MAX}s: $("$DEV" status 2>&1 | tr '\n' ' ')"; exit 9; }
	st=$("$DEV" status 2>&1)
	waiters=$(printf '%s\n' "$st" | grep -cE '^ +[0-9]+\. ')
	held=$(printf '%s\n' "$st" | sed -n 's/^holder .*, \([0-9]*\) min).*/\1/p' | head -1)
	# Behind the holder only: with other rounds cycling, a waiter ahead costs a whole session of theirs.
	if [ "$waiters" -eq 0 ]; then
		budget=$(( left - NEEDED - 10 ))
		echo "$(date -u +%T) queue empty ($(printf '%s\n' "$st" | head -1)); joining with a ${budget}s wait"
		DEADLINE=$END NEEDED_S=$NEEDED "$DEV" run --timeout-sec "$budget" PLE-829 -- \
			bash docs/verification/PLE-829/s22-probe.sh "$LABEL" android/app/build/outputs/apk/debug/app-debug.apk "$ARM" "$PRESSES"
		exit $?
	fi
	sleep 2
done
