#!/usr/bin/env bash
# PLE-829 S22 session: one arm of the stream_pad_input_thread A/B. It installs an APK over a fresh
# app-state backup, sets the arm and PLE-746's probe (stream_input_latency_probe) on top of the
# phone's stored prefs, and streams to PS5-466. Under a Perfetto trace, it presses R3 N times
# (stim.sh, on the device). It pulls presses.csv, the trace and the logcat, leaves the stream with
# Back, and puts the stored prefs back. Run each arm under its own short lease, from the worktree:
#   /home/wnt/gta6/scripts/dev/device.py run PLE-829 -- \
#     bash docs/verification/PLE-829/s22-probe.sh <label> <apk> <on|off> [presses]
# Patterned on PLE-800's session.sh (Connect by tapping Play; no rotation lock, no touch in the
# stream): the only input the console gets is R3.
# DEADLINE (epoch s, optional): the session starts only if NEEDED_S (150) seconds remain once the
# lease is granted, so a queue wait never leaves it cut off half-way with the arm's prefs on the phone.
set -euo pipefail
LABEL=$1; APK=$(realpath "$2"); ARM=$3; PRESSES=${4:-40}
if [ -n "${DEADLINE:-}" ] && [ $(( DEADLINE - $(date +%s) )) -lt "${NEEDED_S:-150}" ]; then
	echo "$LABEL: only $(( DEADLINE - $(date +%s) )) s left before DEADLINE; not starting" >&2
	exit 7
fi
case "$ARM" in on) ARM_VALUE=true ;; off) ARM_VALUE=false ;; *) echo "arm: on or off" >&2; exit 2 ;; esac
REPO=/home/wnt/gta6
WT=$(cd "$(dirname "$0")/../../.." && pwd)
HERE=$REPO/scripts/dev/ab
ADB=$REPO/scripts/dev/device-bin/adb
PKG=fi.madekivi.pleikkari
PREFS=shared_prefs/${PKG}_preferences.xml
OUT=$WT/build/ple829/s22/$LABEL
BACKUP=$WT/build/ple829/s22/stored-prefs.xml
R3_MASK=0x800
# shellcheck source=/home/wnt/gta6/scripts/dev/ab/ui.sh
source "$HERE/ui.sh"
"$REPO/scripts/dev/ps5-hold.sh" check || exit 3
export ANDROID_SERIAL=${ANDROID_SERIAL:-$("$REPO/scripts/dev/phone-resolve.sh")}
mkdir -p "$OUT"
cd "$OUT"
log() { printf '[%s] %s\n' "$(date -u +%H:%M:%S)" "$*" | tee -a session.txt; }
streaming() { "$ADB" shell dumpsys activity activities | grep -q "topResumedActivity.*StreamActivity"; }
leave_stream() {
	if streaming; then
		"$ADB" shell input keyevent 4
		for _ in $(seq 1 10); do sleep 1; streaming || break; done
		sleep 3
	fi
}
write_prefs() { # <file>
	"$ADB" push "$1" /data/local/tmp/ple829_prefs.xml >/dev/null
	"$ADB" shell "chmod 644 /data/local/tmp/ple829_prefs.xml; run-as $PKG cp /data/local/tmp/ple829_prefs.xml $PREFS; rm -f /data/local/tmp/ple829_prefs.xml"
	"$ADB" exec-out run-as $PKG cat $PREFS > prefs-readback.xml
	cmp -s "$1" prefs-readback.xml || { log "prefs readback mismatch for $1"; return 1; }
}
restore() {
	"$ADB" shell setprop debug.pleikkari.probe_buttons "''" || true
	"$ADB" shell am force-stop "$PKG" || true
	[ -s "$BACKUP" ] && write_prefs "$BACKUP" && log "$LABEL: stored prefs back"
}

log "$LABEL: arm $ARM (stream_pad_input_thread=$ARM_VALUE); serial $ANDROID_SERIAL model $("$ADB" shell getprop ro.product.model | tr -d '\r') android $("$ADB" shell getprop ro.build.version.release | tr -d '\r') sdk $("$ADB" shell getprop ro.build.version.sdk | tr -d '\r'); apk sha256 $(sha256sum "$APK" | cut -c1-16)"
"$ADB" shell dumpsys battery | grep -E "^  (level|AC powered|USB powered|temperature):" >> session.txt || true
leave_stream
"$ADB" shell am force-stop "$PKG"
# The arms differ only in a pref: an APK already installed byte for byte is not installed again.
installed=$("$ADB" shell "sha256sum \$(pm path $PKG | head -1 | cut -d: -f2)" 2>/dev/null | cut -c1-64)
if [ "$installed" = "$(sha256sum "$APK" | cut -c1-64)" ]; then
	log "$LABEL: this APK is installed already; no install"
else
	"$REPO/scripts/dev/app-state.sh" backup > backup.txt 2>&1
	log "$LABEL: app state backed up ($(tail -1 backup.txt))"
	sleep 1
	"$ADB" install -r "$APK" > install.txt 2>&1
	log "$LABEL: installed ($(tail -1 install.txt | tr -d '\r'))"
fi

# The phone's own prefs, kept once across the arms, so every arm runs on them and they go back after.
if [ ! -s "$BACKUP" ]; then
	"$ADB" exec-out run-as $PKG cat $PREFS > "$BACKUP"
	grep -q "</map>" "$BACKUP" || { log "no stored prefs to build on"; rm -f "$BACKUP"; exit 5; }
fi
trap restore EXIT
trap 'exit 143' TERM INT
python3 - "$BACKUP" prefs-run.xml "$ARM_VALUE" <<'EOF' | tee -a session.txt
import re, sys
src, dst, arm = sys.argv[1:]
text = open(src).read()
for key, value in (("stream_pad_input_thread", arm), ("stream_input_latency_probe", "true"),
                   ("stream_feedback_stats_log", "true")):
    text = re.sub(r'\s*<boolean name="%s" value="[a-z]+" />' % key, "", text)
    text = text.replace("</map>", '    <boolean name="%s" value="%s" />\n</map>' % (key, value))
    print("pref: %s=%s" % (key, value))
for line in text.splitlines():
    if re.search(r'name="(stream_motion|mapping_r3|stream_gamepad|stream_controller_input|stream_pad)', line):
        print("pref: " + line.strip())
open(dst, "w").write(text)
EOF
write_prefs prefs-run.xml
"$ADB" shell setprop debug.pleikkari.probe_buttons $R3_MASK

"$ADB" logcat -c || true
"$ADB" shell am start -n "$PKG/.main.MainActivity" >/dev/null
sleep 3
# The S22 dozes into Samsung's always-on display on battery: wake it and swipe the non-secure
# keyguard away, or uiautomator dumps the NotificationShade (PLE-800).
"$ADB" shell input keyevent 224
sleep 1
"$ADB" shell input swipe 540 1900 540 600 250
sleep 1
"$ADB" shell wm dismiss-keyguard
sleep 2
"$ADB" exec-out screencap -p > hosts.png
ui_tap_resource_id "$PKG:id/playButton" hosts_ui.xml PS5-466
ok=0
for _ in $(seq 1 20); do sleep 2; if streaming; then ok=1; break; fi; done
[ "$ok" = 1 ] || { "$ADB" exec-out screencap -p > fail.png; log "$LABEL: StreamActivity not resumed"; exit 3; }
attempt=0
while :; do
	for _ in $(seq 1 15); do
		[ "$("$ADB" logcat -d -v threadtime | grep -c 'Takion received init ack')" -ge 2 ] && break 2
		"$ADB" logcat -d | grep -q "Remote is already in use\|Session has quit" && break
		sleep 2
	done
	attempt=$((attempt+1))
	[ $attempt -le 3 ] || { "$ADB" exec-out screencap -p > fail.png; log "$LABEL: no Takion handshake after $attempt attempts"; exit 6; }
	log "$LABEL: console refused or session quit, reconnecting (attempt $attempt)"
	sleep 8
	"$ADB" logcat -c || true
	ui_tap_resource_id "android:id/button1" "reconnect_${attempt}_ui.xml"
done
sleep 5
streaming || { "$ADB" exec-out screencap -p > fail.png; log "$LABEL: stream dropped right after the handshake"; exit 4; }
[ "$("$ADB" logcat -d | grep -c 'Feedback stats:')" -ge 2 ] || log "$LABEL: warning: fewer than 2 Feedback stats lines"
"$ADB" exec-out screencap -p > stream_start.png
"$ADB" shell "dumpsys window | grep -E 'mCurrentFocus|mFocusedApp'" | tee -a session.txt
"$ADB" shell dumpsys SurfaceFlinger > surfaceflinger.txt || true
# The HWC table ends at its first blank line (Samsung's dump follows it with its own tables).
log "$LABEL: HWC layer rows naming PadInput: $(awk '/HWC layers:/{f=1; next} f && /^$/{exit} f' surfaceflinger.txt | grep -c PadInput || true)"

log "$LABEL: streaming; pressing R3 $PRESSES times under a Perfetto trace"
"$ADB" push "$WT/docs/verification/PLE-829/stim.sh" /data/local/tmp/ple829_stim.sh >/dev/null
"$ADB" push "$WT/docs/verification/PLE-829/trace.cfg" /data/misc/perfetto-configs/ple829.cfg >/dev/null
"$ADB" shell sh /data/local/tmp/ple829_stim.sh "$PRESSES" /data/misc/perfetto-traces/ple829.pftrace \
	/data/misc/perfetto-configs/ple829.cfg | tee -a session.txt
"$ADB" shell rm -f /data/local/tmp/ple829_stim.sh /data/misc/perfetto-configs/ple829.cfg
"$ADB" exec-out screencap -p > stream_end.png
still=no
streaming && still=yes
log "$LABEL: presses done; still streaming: $still"
"$ADB" pull /data/misc/perfetto-traces/ple829.pftrace trace.pftrace > /dev/null 2>&1 || log "$LABEL: no trace pulled"
"$ADB" shell rm -f /data/misc/perfetto-traces/ple829.pftrace
leave_stream
log "$LABEL: left the stream (Back: $(streaming && echo 'still streaming' || echo 'StreamActivity gone'))"
"$ADB" logcat -d -v threadtime > logcat.txt
dir=$(grep -o 'Latency probe: writing [^ ]*' logcat.txt | tail -1 | sed 's/Latency probe: writing //')
if [ -n "$dir" ]; then
	"$ADB" pull "$dir/presses.csv" presses.csv > /dev/null 2>&1 || "$ADB" exec-out run-as $PKG cat "$dir/presses.csv" > presses.csv
fi
log "$LABEL: presses.csv rows $(( $(wc -l < presses.csv 2>/dev/null || echo 1) - 1 )); PadInput lines $(grep -c ' PadInput *:' logcat.txt || true); ANR lines $(grep -c 'ANR in\|Input dispatching timed out' logcat.txt || true); 'Session has quit' $(grep -c 'Session has quit' logcat.txt || true)"
grep -E ' PadInput *:|Latency probe' logcat.txt | tee -a session.txt || true
