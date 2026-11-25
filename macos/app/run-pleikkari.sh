#!/usr/bin/env bash
# Start a Pleikkari stream with telemetry into a fresh run directory, and say where it is.
#
# usage: run-pleikkari.sh <label> [-- <extra Pleikkari options>]
#   label    names the run directory: <workspace>/build/native-app/runs/<label>-<HHMMSS>/
#   extra    passed to `Pleikkari stream`, e.g. -- --codec h265-hdr --bitrate 25000
# env:
#   APP         Pleikkari.app to run (default <workspace>/build/native-app/app/macos/app/Pleikkari.app)
#   HOST_NICK   registered console nickname (default PS5-466)
#   HOST_IP     console address (default 192.168.1.164)
#   FULLSCREEN=1  start in fullscreen (Game Mode; Cmd-F toggles)
#   DUMP=1      also record the video samples to <run>/video.plkv (not for latency runs). A
#               stream that starts later, as after H in the Video menu, goes to a file of
#               its own beside it: video-2-sdr.plkv, video-3-hdr.plkv and so on. Each file
#               replays by itself (replay-pleikkari.sh). About 110 to 190 MB a minute at
#               15000 to 25000 kbit/s
#   COLLECT=1   also run build/mac/profile/collect.sh (top, ping, nettop, GPU) for SECONDS
#   SECONDS_TO_COLLECT  collect.sh duration (default 300)
#   DIRECT=1    exec the binary from this shell instead of `open` (by default the app goes
#               through LaunchServices, so it is activated, frontmost, and asks for the
#               local network permission under its own name)
#
# The run directory gets session.log (the app's log: lib, [frame] lines, stats) and
# stdout.log. The stream ends from the app (Cmd-Q, closing the window) or the console.
# The app itself retries a session the console refuses for up to 20 s, as the Qt app does.
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
SRC=$(cd "$HERE/../.." && pwd -P)
case "$SRC" in
	*/wt/*) WORKSPACE=${SRC%%/wt/*} ;;
	*) WORKSPACE=$(dirname "$SRC") ;;
esac
LABEL=${1:?usage: run-pleikkari.sh <label> [-- <extra options>]}
shift
[ "${1:-}" = "--" ] && shift
APP=${APP:-$WORKSPACE/build/native-app/app/macos/app/Pleikkari.app}
HOST_NICK=${HOST_NICK:-PS5-466}
HOST_IP=${HOST_IP:-192.168.1.164}
PROFILE_DIR=$WORKSPACE/build/mac/profile
RUN="$WORKSPACE/build/native-app/runs/$LABEL-$(date +%H%M%S)"
BIN="$APP/Contents/MacOS/Pleikkari"

if [ ! -x "$BIN" ]; then
	echo "no app at $APP: build it with $HERE/build.sh" >&2
	exit 1
fi
if pgrep -f 'Pleikkari.app/Contents/MacOS/Pleikkari' > /dev/null; then
	echo "a Pleikkari process is already running: end its stream first" >&2
	pgrep -fl 'Pleikkari.app/Contents/MacOS/Pleikkari' >&2
	exit 1
fi
if pgrep -f 'chiaki.app/Contents/MacOS/chiaki' > /dev/null; then
	echo "note: the Qt chiaki app is running; the console takes one Remote Play session at a time" >&2
fi

mkdir -p "$RUN"
# Every trigger effect message of the console goes to the log ("Trigger effects packet"),
# with what the app applied to the controller next to it.
export CHIAKI_TRIGGER_LOG=1
ARGS=(stream "$HOST_NICK" "$HOST_IP" --log "$RUN/session.log")
[ "${FULLSCREEN:-0}" = 1 ] && ARGS+=(--fullscreen)
[ "${DUMP:-0}" = 1 ] && ARGS+=(--dump "$RUN/video.plkv")
ARGS+=("$@")
printf '%q ' "$BIN" "${ARGS[@]}" > "$RUN/command"
echo >> "$RUN/command"

if [ "${DIRECT:-0}" = 1 ]; then
	nohup "$BIN" "${ARGS[@]}" > "$RUN/stdout.log" 2>&1 &
	PID=$!
	disown
else
	open -n -a "$APP" --env CHIAKI_TRIGGER_LOG=1 --stdout "$RUN/stdout.log" --stderr "$RUN/stdout.log" \
		--args "${ARGS[@]}"
	PID=
	for _ in $(seq 1 20); do
		PID=$(pgrep -n -f "$BIN") && break
		perl -e 'select(undef,undef,undef,0.25)'
	done
	if [ -z "$PID" ]; then
		echo "the app did not start; see $RUN/stdout.log" >&2
		exit 1
	fi
fi
echo "$PID" > "$RUN/pid"

# Up within 25 s (the app's own retry window is 20 s) means frames are arriving.
STATE="no frames yet"
for _ in $(seq 1 50); do
	perl -e 'select(undef,undef,undef,0.5)'
	if ! kill -0 "$PID" 2> /dev/null; then
		STATE="process exited"
		break
	fi
	if grep -q '\[frame\] idx=' "$RUN/session.log" 2> /dev/null; then
		STATE="streaming"
		break
	fi
done

echo "app         $APP"
echo "pid         $PID ($STATE)"
echo "run dir     $RUN"
echo "session log $RUN/session.log"
[ "${DUMP:-0}" = 1 ] && echo "recording   $RUN/video.plkv, and video-<n>-<hdr|sdr>.plkv for each stream that starts after it (H)"
if [ "$STATE" = "process exited" ]; then
	echo "the stream did not start; the end of the log:" >&2
	tail -n 15 "$RUN/session.log" >&2 2> /dev/null || tail -n 15 "$RUN/stdout.log" >&2
	exit 1
fi

if [ "${COLLECT:-0}" = 1 ] && [ -x "$PROFILE_DIR/collect.sh" ]; then
	RUNLOG="$RUN/session.log" nohup "$PROFILE_DIR/collect.sh" "$PID" "$HOST_IP" "$RUN" "${SECONDS_TO_COLLECT:-300}" \
		> "$RUN/collect.out" 2>&1 &
	disown
	echo "collecting  ${SECONDS_TO_COLLECT:-300} s of top/ping/nettop/GPU into $RUN"
fi
echo "per-frame latency: $PROFILE_DIR/frames.py \"$RUN/session.log\" 20"
echo "trigger effects:   $PROFILE_DIR/triggers.py \"$RUN/session.log\""
echo "stats lines:       grep -E '\\] \\[I\\] (video|audio|controller|frames):' \"$RUN/session.log\""
