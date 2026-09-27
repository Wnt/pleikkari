#!/usr/bin/env bash
# PLE-788 attempt 2: install, start stream, inject Opus failure, capture logcat.
set -u
cd /home/wnt/gta6/build/captures/ple788
ADB=/home/wnt/gta6/scripts/dev/device-bin/adb
PKG=fi.madekivi.pleikkari
PLAY_RESOURCE_ID=$PKG:id/playButton
HERE=/home/wnt/gta6/scripts/dev/ab
. /home/wnt/gta6/scripts/dev/ab/ui.sh
log() { echo "[$(date +%T)] $*"; }
$ADB install -r /home/wnt/gta6/wt/ple-788/android/app/build/outputs/apk/debug/app-debug.apk | tail -1
$ADB shell input keyevent KEYCODE_WAKEUP; sleep 1
$ADB shell input swipe 540 1800 540 600 300; sleep 1
$ADB shell svc power stayon true
$ADB shell setprop debug.chiaki.audio_fail 0
$ADB shell am force-stop $PKG; sleep 1
$ADB logcat -c
$ADB shell am start -n $PKG/.main.MainActivity >/dev/null; sleep 5
ui_tap_resource_id "$PLAY_RESOURCE_ID" main_ui.xml PS5-466 || { log "tap failed"; $ADB exec-out screencap -p > fail.png; }
for i in $(seq 1 40); do
  sleep 2
  $ADB logcat -d -v threadtime > logcat2.txt
  n=$(grep -c 'Takion received init ack' logcat2.txt)
  [ "$n" -ge 2 ] && break
done
log "init acks: $n"
sleep 15
$ADB exec-out screencap -p > before.png
log "inject 1"; $ADB shell setprop debug.chiaki.audio_fail 1
sleep 15
log "inject 2"; $ADB shell setprop debug.chiaki.audio_fail 2
sleep 15
$ADB exec-out screencap -p > after.png
$ADB shell dumpsys media.audio_flinger > audioflinger.txt
$ADB logcat -d -v threadtime > logcat2.txt
$ADB shell setprop debug.chiaki.audio_fail 0
$ADB shell input keyevent KEYCODE_BACK; sleep 2
$ADB shell am force-stop $PKG
$ADB shell svc power stayon false
grep -n -i "audio decoder\|Injecting\|Re-creating\|AudioOutput\|oboe" logcat2.txt | head -60
