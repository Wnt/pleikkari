#!/usr/bin/env bash
# PLE-823 S22 session: install an APK over a fresh app-state backup, set motion off, stream to
# PS5-466, send N left-stick sweeps (stim.sh, on the device), pull logcat, leave the stream,
# put the phone's own prefs back. Run it under the lease:
#   scripts/dev/device.py run --resource samsung PLE-823 -- bash build/ple823/session.sh <label> <apk> [sweeps]
# Patterned on PLE-800's docs/verification/PLE-800/session.sh. Motion off because with motion
# on the sensors change the controller state every ~1.3 ms, and each change wakes the sender,
# which hides the stale deadline. No touch in the stream: the only input the console gets is
# the stick sweeps.
set -euo pipefail
LABEL=$1; APK=$(realpath "$2"); SWEEPS=${3:-40}
REPO=/home/wnt/gta6
HERE=$REPO/scripts/dev/ab
ADB=$REPO/scripts/dev/device-bin/adb
PKG=fi.madekivi.pleikkari
PREFS=shared_prefs/${PKG}_preferences.xml
OUT=$REPO/build/ple823/$LABEL
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
push_prefs() {
	"$ADB" push "$1" /data/local/tmp/ple823_prefs.xml >/dev/null
	"$ADB" shell "run-as $PKG sh -c 'cp /data/local/tmp/ple823_prefs.xml $PREFS && chmod 660 $PREFS'"
	"$ADB" shell "echo > /data/local/tmp/ple823_prefs.xml"
}

log "$LABEL: serial $ANDROID_SERIAL model $("$ADB" shell getprop ro.product.model | tr -d '\r'), apk $APK sha256 $(sha256sum "$APK" | cut -c1-16)"
"$ADB" shell dumpsys battery | grep -E "^  (level|AC powered|USB powered|temperature):" >> session.txt || true
"$ADB" shell dumpsys package "$PKG" | grep -E "versionName|lastUpdateTime" > installed_before.txt || true
if [ ! -s "$REPO/build/ple823/installed-before.apk" ]; then
	"$ADB" pull "$("$ADB" shell pm path "$PKG" | sed -n 's/^package://p' | tr -d '\r' | head -1)" "$REPO/build/ple823/installed-before.apk" > /dev/null
	cp installed_before.txt "$REPO/build/ple823/installed-before.txt"
	log "$LABEL: pulled the installed APK, sha256 $(sha256sum "$REPO/build/ple823/installed-before.apk" | cut -c1-16)"
fi
"$REPO/scripts/dev/app-state.sh" backup > backup.txt 2>&1
log "$LABEL: app state backed up ($(tail -1 backup.txt))"

leave_stream
"$ADB" shell am force-stop "$PKG"
sleep 2
umask 077 # the prefs hold psn_account_id: keep them out of git and out of logs
"$ADB" exec-out run-as "$PKG" cat "$PREFS" > prefs_orig.xml
grep -q "</map>" prefs_orig.xml || { log "$LABEL: could not read the prefs"; exit 5; }
log "$LABEL: prefs read; motion_enabled was: $(grep -o 'name="motion_enabled" value="[a-z]*"' prefs_orig.xml || echo 'unset (default true)')"
"$ADB" install -r "$APK" > install.txt 2>&1
log "$LABEL: installed ($(tail -1 install.txt | tr -d '\r'))"
"$ADB" shell dumpsys package "$PKG" | grep -E "versionName|lastUpdateTime" > installed_after.txt || true
# Change only that one entry: mkprefs.py re-renders the file and drops multi-line <set>s.
python3 - prefs_orig.xml prefs_motion_off.xml <<'PY'
import re, sys
xml = open(sys.argv[1]).read()
entry = '<boolean name="motion_enabled" value="false" />'
xml, n = re.subn(r'<boolean name="motion_enabled" value="[a-z]*" */>', entry, xml)
if not n:
    xml = xml.replace("</map>", "    " + entry + "\n</map>", 1)
open(sys.argv[2], "w").write(xml)
PY
push_prefs prefs_motion_off.xml
"$ADB" exec-out run-as "$PKG" cat "$PREFS" > prefs_on_device.xml
grep -q 'name="motion_enabled" value="false"' prefs_on_device.xml || { log "$LABEL: motion_enabled=false did not stick"; push_prefs prefs_orig.xml; exit 5; }
log "$LABEL: motion_enabled=false written"

"$ADB" logcat -c || true
"$ADB" shell am start -n "$PKG/.main.MainActivity" >/dev/null
sleep 3
# The S22 sits in Samsung's always-on display on battery (30 s screen timeout). Wake it and
# swipe the non-secure keyguard away (PLE-800).
"$ADB" shell input keyevent 224
sleep 1
"$ADB" shell input swipe 540 1900 540 600 250
sleep 1
"$ADB" shell wm dismiss-keyguard
sleep 2
"$ADB" shell dumpsys window | grep -E "mCurrentFocus" >> session.txt || true
"$ADB" exec-out screencap -p > hosts.png
ui_tap_resource_id "$PKG:id/playButton" hosts_ui.xml PS5-466
ok=0
for _ in $(seq 1 20); do sleep 2; if streaming; then ok=1; break; fi; done
[ "$ok" = 1 ] || { "$ADB" exec-out screencap -p > fail.png; log "$LABEL: StreamActivity not resumed"; "$ADB" shell am force-stop "$PKG"; push_prefs prefs_orig.xml; exit 3; }
attempt=0
while :; do
	for _ in $(seq 1 15); do
		[ "$("$ADB" logcat -d -v threadtime | grep -c 'Takion received init ack')" -ge 2 ] && break 2
		"$ADB" logcat -d | grep -q "Remote is already in use\|Session has quit" && break
		sleep 2
	done
	attempt=$((attempt+1))
	[ $attempt -le 4 ] || { "$ADB" exec-out screencap -p > fail.png; log "$LABEL: no Takion handshake after $attempt attempts"; leave_stream; "$ADB" shell am force-stop "$PKG"; push_prefs prefs_orig.xml; exit 6; }
	log "$LABEL: console refused or session quit, reconnecting (attempt $attempt)"
	sleep 8
	"$ADB" logcat -c || true
	ui_tap_resource_id "android:id/button1" "reconnect_${attempt}_ui.xml"
done
sleep 10
streaming || { "$ADB" exec-out screencap -p > fail.png; log "$LABEL: stream dropped right after the handshake"; "$ADB" shell am force-stop "$PKG"; push_prefs prefs_orig.xml; exit 4; }
"$ADB" exec-out screencap -p > stream_start.png
log "$LABEL: streaming; sending $SWEEPS stick sweeps"

"$ADB" push "$REPO/build/ple823/stim.sh" /data/local/tmp/ple823_stim.sh >/dev/null
"$ADB" shell sh /data/local/tmp/ple823_stim.sh "$SWEEPS"
"$ADB" shell rm /data/local/tmp/ple823_stim.sh
sleep 1
"$ADB" exec-out screencap -p > stream_end.png
still=no
streaming && still=yes
"$ADB" logcat -d -v threadtime > logcat.txt
log "$LABEL: sweeps done; still streaming: $still; PLE823 lines $(grep -c 'PLE823 state' logcat.txt); 'Session has quit' $(grep -c 'Session has quit' logcat.txt)"
leave_stream
"$ADB" shell am force-stop "$PKG"
sleep 1
push_prefs prefs_orig.xml
"$ADB" exec-out run-as "$PKG" cat "$PREFS" > prefs_restored.xml
if cmp -s prefs_orig.xml prefs_restored.xml; then log "$LABEL: left the stream; prefs restored byte for byte"; else log "$LABEL: left the stream; PREFS RESTORE MISMATCH"; exit 7; fi
