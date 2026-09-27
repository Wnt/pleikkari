#!/usr/bin/env bash
# PLE-800 S22 session: install an APK over a fresh app-state backup, stream to PS5-466,
# press R3 N times (stim.sh, on the device), pull logcat, leave the stream with BACK.
# Run it under the lease:
#   scripts/dev/device.py run --resource samsung PLE-800 -- bash build/ple800/session.sh <label> <apk> [presses]
# Patterned on scripts/dev/ab/soak.sh start/collect. No rotation lock, no prefs write, no
# touch in the stream: the only input the console gets is the R3 presses.
set -euo pipefail
LABEL=$1; APK=$(realpath "$2"); PRESSES=${3:-100}
REPO=/home/wnt/gta6
HERE=$REPO/scripts/dev/ab
ADB=$REPO/scripts/dev/device-bin/adb
PKG=fi.madekivi.pleikkari
OUT=$REPO/build/ple800/$LABEL
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

log "$LABEL: serial $ANDROID_SERIAL model $("$ADB" shell getprop ro.product.model | tr -d '\r'), apk $APK sha256 $(sha256sum "$APK" | cut -c1-16)"
"$ADB" shell dumpsys battery | grep -E "^  (level|AC powered|USB powered|temperature):" >> session.txt || true
"$ADB" shell dumpsys package "$PKG" | grep -E "versionName|lastUpdateTime" > installed_before.txt || true
"$REPO/scripts/dev/app-state.sh" backup > backup.txt 2>&1
log "$LABEL: app state backed up ($(tail -1 backup.txt))"

leave_stream
"$ADB" shell am force-stop "$PKG"
sleep 2
"$ADB" install -r "$APK" > install.txt 2>&1
log "$LABEL: installed ($(tail -1 install.txt | tr -d '\r'))"
"$ADB" shell dumpsys package "$PKG" | grep -E "versionName|lastUpdateTime" > installed_after.txt || true

"$ADB" logcat -c || true
"$ADB" shell am start -n "$PKG/.main.MainActivity" >/dev/null
sleep 3
# The S22 sits in Samsung's always-on display on battery (30 s screen timeout). Wake it and
# swipe the non-secure keyguard away: after a bare `wm dismiss-keyguard` the NotificationShade
# keeps the focus and uiautomator dumps it instead of the app.
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
[ "$ok" = 1 ] || { "$ADB" exec-out screencap -p > fail.png; log "$LABEL: StreamActivity not resumed"; exit 3; }
attempt=0
while :; do
	for _ in $(seq 1 15); do
		[ "$("$ADB" logcat -d -v threadtime | grep -c 'Takion received init ack')" -ge 2 ] && break 2
		"$ADB" logcat -d | grep -q "Remote is already in use\|Session has quit" && break
		sleep 2
	done
	attempt=$((attempt+1))
	[ $attempt -le 4 ] || { "$ADB" exec-out screencap -p > fail.png; log "$LABEL: no Takion handshake after $attempt attempts"; exit 6; }
	log "$LABEL: console refused or session quit, reconnecting (attempt $attempt)"
	sleep 8
	"$ADB" logcat -c || true
	ui_tap_resource_id "android:id/button1" "reconnect_${attempt}_ui.xml"
done
sleep 10
streaming || { "$ADB" exec-out screencap -p > fail.png; log "$LABEL: stream dropped right after the handshake"; exit 4; }
"$ADB" exec-out screencap -p > stream_start.png
log "$LABEL: streaming; pressing R3 $PRESSES times"

"$ADB" push "$REPO/build/ple800/stim.sh" /data/local/tmp/ple800_stim.sh >/dev/null
"$ADB" shell sh /data/local/tmp/ple800_stim.sh "$PRESSES"
"$ADB" shell rm /data/local/tmp/ple800_stim.sh
"$ADB" exec-out screencap -p > stream_end.png
still=no
streaming && still=yes
"$ADB" logcat -d -v threadtime > logcat.txt
log "$LABEL: presses done; still streaming: $still; PLE800 lines $(grep -c 'PLE800 history' logcat.txt); 'Session has quit' $(grep -c 'Session has quit' logcat.txt)"
leave_stream
log "$LABEL: left the stream"
