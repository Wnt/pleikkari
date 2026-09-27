#!/usr/bin/env bash
# PLE-385: deliberately provoke (and measure) the PS5 "AvCap failed to initialize
# video: [InitResult:-6/-11]" wedge.
#
# The wedge is console-side: in every prior occurrence the client's session
# request, ctrl login, Senkusha and the streaming Takion all succeed, and ~2 s
# after "StreamConnection successfully received bang" the CONSOLE sends a
# disconnect whose reason is the AvCap init failure (see
# build/captures/ple356-probe/session_logcat.txt:1164). So the only thing a
# client-side harness can do is drive connect/exit sequences and classify what
# the console answers.
#
# One trial = connect, stream for STREAM_SECS, leave by EXIT_MODE, wait
# WAIT_SECS, connect again, classify the second connect. Each trial appends one
# row to $OUT_DIR/results.tsv and keeps both connects' logcat.
#
# Usage:
#   OUT_DIR=build/captures/ple385 wedge-probe.sh <label> <exit-mode> <wait-secs>
#   exit-mode: clean      -- BACK key; StreamActivity does not intercept it and
#                            falls through to finish(), the in-app exit path
#              forcestop  -- am force-stop mid-stream (no Disconnect sent)
#              earlystop  -- force-stop EARLY_SECS (default 5) after the connect
#                            is tapped, i.e. while the console is still setting
#                            the session up and before any video has flowed
#              blackout-handshake -- tap connect, then cut the console-side link
#                            entirely (netem loss 100%) ~2 s in, so the console
#                            is left initialising AvCap with no client at all,
#                            then force-stop the app and restore the link
#              none       -- no first stream at all; just one connect
#                            (used to poll a console that is already wedged)
#
# IMPAIR_PROFILE (optional): a scripts/net/impairctl.py profile applied to the
# console-side link once the first stream is confirmed and held until the exit,
# then cleaned. Every recorded occurrence of the wedge happened inside an
# impairment capture run, so "the session died under heavy loss/delay" is a
# trigger candidate that client-side kills alone cannot test.
#
# Exit status: 0 if the *second* connect streamed, 10 if it hit the AvCap wedge,
# 11 rp_in_use, 12 any other session quit, 13 harness failure.
set -uo pipefail
REPO=/home/wnt/gta6
HERE=$REPO/scripts/dev/ab
ADB=$REPO/scripts/dev/device-bin/adb
PKG=fi.madekivi.pleikkari
PS5_NAME=${PS5_NAME:-PS5-466}
STREAM_SECS=${STREAM_SECS:-25}
CONNECT_TIMEOUT=${CONNECT_TIMEOUT:-70}
OUT=${OUT_DIR:-}
FORCE=1
source "$(dirname "${BASH_SOURCE[0]}")/../lib/capture-guard.sh"
require_out_dir "$OUT" "$FORCE"
source "$HERE/ui.sh"

LABEL=${1:?label}
EXIT_MODE=${2:?exit-mode}
WAIT_SECS=${3:-10}

mkdir -p "$OUT"
RESULTS="$OUT/results.tsv"
[ -s "$RESULTS" ] || printf 'utc\tlabel\texit_mode\twait_s\tconnect1\tconnect2\tdetail\n' > "$RESULTS"

log(){ printf '[%s] %s\n' "$(date -u +%H:%M:%S)" "$*"; }
streaming(){ "$ADB" shell dumpsys activity activities | grep -q "topResumedActivity.*StreamActivity"; }
stats_count(){ grep -c "Feedback stats:" "$1" 2>/dev/null || true; }

start_logcat(){ # $1 = log file
  "$ADB" logcat -c >/dev/null 2>&1 || true
  "$ADB" logcat -v time > "$1" 2>/dev/null &
  LOGCAT_PID=$!
}
stop_logcat(){ [ -z "${LOGCAT_PID:-}" ] || kill "$LOGCAT_PID" 2>/dev/null; LOGCAT_PID=; }

cleanup(){ local rc=$?; trap - EXIT; stop_logcat; exit $rc; }
trap cleanup EXIT

# Leave whatever dialog/activity is up, without force-stopping.
dismiss(){
  local dump="$OUT/${LABEL}_dismiss.xml" coords
  if "$ADB" shell uiautomator dump /data/local/tmp/ple385.xml >/dev/null 2>&1; then
    "$ADB" exec-out cat /data/local/tmp/ple385.xml > "$dump" 2>/dev/null
    "$ADB" shell rm /data/local/tmp/ple385.xml >/dev/null 2>&1
    # "Session has quit" dialog: button1=Reconnect, button2=Quit. Take Quit.
    if coords=$(python3 "$HERE/ui_selector.py" "$dump" "android:id/button2" 2>/dev/null); then
      # shellcheck disable=SC2086
      "$ADB" shell input tap $coords
      sleep 2
    fi
  fi
  local i
  for i in $(seq 1 10); do streaming || return 0; "$ADB" shell input keyevent 4 >/dev/null; sleep 2; done
  streaming && return 1 || return 0
}

# connect <logfile> <tag>; echoes classification, returns 0 only when streaming.
connect(){
  local LOG=$1 tag=$2 i n cls rc
  "$ADB" shell input keyevent 224 >/dev/null 2>&1   # WAKEUP
  "$ADB" shell wm dismiss-keyguard >/dev/null 2>&1 || true
  "$ADB" shell am start -n "$PKG/fi.madekivi.pleikkari.main.MainActivity" >/dev/null 2>&1
  sleep 3
  # After a kill -9 the app can take a few seconds to repopulate the console
  # list, so the tile is not necessarily there on the first dump.
  local tapped=0
  for i in $(seq 1 6); do
    if ui_tap_resource_id "$PKG:id/playButton" "$OUT/${LABEL}_${tag}_main.xml" "$PS5_NAME" 2>/dev/null; then tapped=1; break; fi
    sleep 3
  done
  if [ "$tapped" != 1 ]; then
    echo "HARNESS:no_tile"; return 13
  fi
  for i in $(seq 1 $((CONNECT_TIMEOUT/2))); do
    sleep 2
    # Same contract as this script's exit status: 10 wedge, 11 rp_in_use,
    # 12 other quit; 13 (UNKNOWN) means no failure signature yet.
    cls=$(classify_connect_failure "$LOG"); rc=$?
    if [ $rc -ne 13 ]; then echo "$cls"; return $rc; fi
    n=$(stats_count "$LOG")
    [ "${n:-0}" -ge 3 ] && { echo "OK"; return 0; }
  done
  echo "TIMEOUT"; return 13
}

leave(){
  case "$EXIT_MODE" in
    clean)
      log "leaving via BACK (in-app exit -> finish())"
      "$ADB" shell input keyevent 4 >/dev/null
      sleep 3
      dismiss || log "WARNING: StreamActivity still topmost after clean exit"
      ;;
    forcestop)
      log "force-stopping mid-stream"
      "$ADB" shell am force-stop "$PKG" >/dev/null
      sleep 2
      ;;
    none) : ;;
    *) log "unknown exit mode $EXIT_MODE"; exit 13;;
  esac
}

EARLY_SECS=${EARLY_SECS:-5}

C1=skipped
if [ "$EXIT_MODE" = blackout-handshake ]; then
  LOG1="$OUT/${LABEL}_connect1.log"
  start_logcat "$LOG1"
  log "trial $LABEL: connect, then blackout the console link mid-handshake"
  "$ADB" shell input keyevent 224 >/dev/null 2>&1
  "$ADB" shell wm dismiss-keyguard >/dev/null 2>&1 || true
  "$ADB" shell am start -n "$PKG/fi.madekivi.pleikkari.main.MainActivity" >/dev/null 2>&1
  sleep 3
  if ! ui_tap_resource_id "$PKG:id/playButton" "$OUT/${LABEL}_c1_main.xml" "$PS5_NAME"; then
    log "no tile"; exit 13
  fi
  # BLACKOUT_DELAY tunes where the cut lands. The ssh round-trip into the netem
  # guest is ~2 s on its own; with delay 0 the link goes down while the session
  # request is still outstanding (the console never starts AvCap at all), so use
  # ~2 s to land it after Senkusha/bang, i.e. while the console is initialising
  # AvCap and about to send streaminfo.
  sleep "${BLACKOUT_DELAY:-2}"
  "$REPO/scripts/net/impairctl.py" --commit --ttl 10m --loss 100% profile custom > "$OUT/${LABEL}_impair_apply.txt" 2>&1
  sleep "${EARLY_SECS:-6}"
  C1="blackout_video_started=$(grep -c 'Feedback stats:' "$LOG1")"
  "$ADB" shell am force-stop "$PKG" >/dev/null
  sleep 2
  stop_logcat
  log "restoring the link"
  "$REPO/scripts/net/impairctl.py" --commit clean > "$OUT/${LABEL}_impair_clean.txt" 2>&1 || log "WARNING: impairment clean failed"
elif [ "$EXIT_MODE" = earlystop ]; then
  LOG1="$OUT/${LABEL}_connect1.log"
  start_logcat "$LOG1"
  log "trial $LABEL: tapping connect, force-stopping ${EARLY_SECS}s later (mid-handshake)"
  "$ADB" shell input keyevent 224 >/dev/null 2>&1
  "$ADB" shell wm dismiss-keyguard >/dev/null 2>&1 || true
  "$ADB" shell am start -n "$PKG/fi.madekivi.pleikkari.main.MainActivity" >/dev/null 2>&1
  sleep 3
  if ! ui_tap_resource_id "$PKG:id/playButton" "$OUT/${LABEL}_c1_main.xml" "$PS5_NAME"; then
    log "no tile"; exit 13
  fi
  sleep "$EARLY_SECS"
  "$ADB" shell am force-stop "$PKG" >/dev/null
  C1="killed_after_${EARLY_SECS}s"
  grep -q "Feedback stats:" "$LOG1" && C1="${C1}_video_had_started"
  sleep 2
  stop_logcat
elif [ "$EXIT_MODE" != none ]; then
  LOG1="$OUT/${LABEL}_connect1.log"
  start_logcat "$LOG1"
  log "trial $LABEL: first connect"
  C1=$(connect "$LOG1" c1); rc1=$?
  log "first connect: $C1"
  if [ $rc1 -ne 0 ]; then
    stop_logcat; dismiss
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$(date -u +%H:%M:%SZ)" "$LABEL" "$EXIT_MODE" "$WAIT_SECS" "$C1" "-" "first connect already failed" >> "$RESULTS"
    exit $rc1
  fi
  if [ -n "${IMPAIR_PROFILE:-}" ]; then
    log "applying impairment profile $IMPAIR_PROFILE to the console-side link"
    # shellcheck disable=SC2086  -- IMPAIR_ARGS is a deliberate argument list
    "$REPO/scripts/net/impairctl.py" --commit --ttl 10m ${IMPAIR_ARGS:-} profile "$IMPAIR_PROFILE" > "$OUT/${LABEL}_impair_apply.txt" 2>&1 \
      || { log "impairctl failed; see ${LABEL}_impair_apply.txt"; exit 13; }
  fi
  log "streaming ${STREAM_SECS}s"
  sleep "$STREAM_SECS"
  "$ADB" exec-out screencap -p > "$OUT/${LABEL}_streaming.png" 2>/dev/null
  leave
  stop_logcat
  if [ -n "${IMPAIR_PROFILE:-}" ]; then
    log "cleaning impairment"
    "$REPO/scripts/net/impairctl.py" --commit clean > "$OUT/${LABEL}_impair_clean.txt" 2>&1 || log "WARNING: impairment clean failed"
  fi
fi

# SWITCH_CODEC=h264|h265 (with SWITCH_FPS) flips the client's codec between the
# two connects, so the console has to re-initialise AvCap for a different codec
# than the session it just lost -- the one plausible AvCap-init trigger that a
# same-settings reconnect cannot exercise. The app is not running at this point
# (force-stopped, or finished), so editing its prefs file is safe.
if [ -n "${SWITCH_CODEC:-}" ]; then
  PREFS=/data/data/$PKG/shared_prefs/${PKG}_preferences.xml
  log "switching client codec to $SWITCH_CODEC @ ${SWITCH_FPS:-60} fps before the reconnect"
  "$ADB" shell am force-stop "$PKG" >/dev/null
  "$ADB" shell "run-as $PKG sed -i 's#>h26[45]<#>$SWITCH_CODEC<#; s#\"stream_fps\">[0-9]*<#\"stream_fps\">${SWITCH_FPS:-60}<#' $PREFS"
  "$ADB" shell "run-as $PKG grep -E 'stream_codec|stream_fps' $PREFS" > "$OUT/${LABEL}_prefs_after_switch.txt"
fi

log "waiting ${WAIT_SECS}s before reconnect"
sleep "$WAIT_SECS"

LOG2="$OUT/${LABEL}_connect2.log"
start_logcat "$LOG2"
C2=$(connect "$LOG2" c2); rc2=$?
log "second connect: $C2"
if [ $rc2 -eq 0 ]; then
  "$ADB" exec-out screencap -p > "$OUT/${LABEL}_reconnected.png" 2>/dev/null
  sleep 3
  "$ADB" shell input keyevent 4 >/dev/null
  sleep 3
fi
stop_logcat
dismiss || log "WARNING: could not leave cleanly after trial"

printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$(date -u +%H:%M:%SZ)" "$LABEL" "$EXIT_MODE" "$WAIT_SECS" "$C1" "$C2" "" >> "$RESULTS"
tail -1 "$RESULTS"
exit $rc2
