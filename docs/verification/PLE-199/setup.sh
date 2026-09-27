#!/usr/bin/env bash
# PLE-199: one-off setup under the Samsung reservation: back up app state, install the
# worktree APK with install -r, and write the two arms' prefs from the phone's own prefs.
#
#   device.py run PLE-199 -- setup.sh <apk>
set -euo pipefail
apk=$1
: "${CAP:?}"
ADB=/home/wnt/gta6/scripts/dev/device-bin/adb
HERE=$(cd "$(dirname "$0")" && pwd)
MKPREFS=$CAP/harness/scripts/dev/ab/mkprefs.py
export ANDROID_SERIAL=${ANDROID_SERIAL:-$(/home/wnt/gta6/scripts/dev/phone-resolve.sh)}
export AB_CAPTURES=$CAP AB_ADB=$ADB
echo "serial $ANDROID_SERIAL"
"$ADB" shell getprop ro.product.model

/home/wnt/gta6/scripts/dev/app-state.sh backup
"$ADB" install -r "$apk"
"$ADB" shell dumpsys package fi.madekivi.pleikkari | grep -m2 -E 'versionName|lastUpdateTime'

python3 "$MKPREFS" --pull-base "$CAP/base_prefs.xml"
# Every flag but the boost is pinned to its shipped default (the PLE-517 arm-A set),
# so the phone's drifted stored prefs cannot leak into either arm.
pins=(
	stream_feedback_stats_log=boolean:true
	stream_wifi_low_latency_lock=boolean:false
	stream_decoder_operating_rate_auto=boolean:true
	stream_fps=string:60
	stream_decoder_operating_rate=int:0
	stream_codec=string:h265
	stream_bitrate=int:0
	stream_diagnostics_overlay=boolean:false
	stream_resolution=string:720p
	stream_decoder_input_thread=boolean:true
	stream_decoder_operating_rate_default=boolean:true
	stream_real_video_timestamps=boolean:false
	stream_quality_preset=string:low_latency
	stream_performance_mode=boolean:false
	stream_debanding=boolean:false
	log_verbose=boolean:false
)
python3 "$MKPREFS" "$CAP/prefs_off.xml" "${pins[@]}" stream_thread_priority_boost=boolean:false
python3 "$MKPREFS" "$CAP/prefs_on.xml" "${pins[@]}" stream_thread_priority_boost=boolean:true
diff "$CAP/prefs_off.xml" "$CAP/prefs_on.xml" || true
"$ADB" shell dumpsys battery | grep -E 'AC powered|status|level'
"$ADB" shell dumpsys thermalservice | grep -m1 'mName=SKIN'
