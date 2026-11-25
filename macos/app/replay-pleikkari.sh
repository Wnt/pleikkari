#!/usr/bin/env bash
# Replay a recorded stream in a window, looping, with the log in a fresh run directory:
# the Video menu, neural rendering and the analysis panel with no console.
#
# usage: replay-pleikkari.sh <dump.plkv> [-- <extra Pleikkari options>]
#   extra    passed to `Pleikkari replay`, e.g. -- --keys 2:i   (the analysis panel after 2 s)
# env:
#   APP           Pleikkari.app to run (default <workspace>/build/native-app/app/macos/app/Pleikkari.app)
#   LABEL         names the run directory: <workspace>/build/native-app/runs/<LABEL>-<HHMMSS>/ (default replay)
#   FULLSCREEN=1  start in fullscreen (Cmd-F toggles)
#
# The app goes through LaunchServices (`open`), so it is activated and in front. It ends
# with Cmd-Q or by closing the window. What is picked in the Video menu is remembered, as
# in a stream; add -- --no-saved-choices to leave the remembered choices alone.
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
SRC=$(cd "$HERE/../.." && pwd -P)
case "$SRC" in
	*/wt/*) WORKSPACE=${SRC%%/wt/*} ;;
	*) WORKSPACE=$(dirname "$SRC") ;;
esac
DUMP=${1:?usage: replay-pleikkari.sh <dump.plkv> [-- <extra options>]}
shift
[ "${1:-}" = "--" ] && shift
[ -f "$DUMP" ] || { echo "no such file: $DUMP" >&2; exit 1; }
DUMP=$(cd "$(dirname "$DUMP")" && pwd -P)/$(basename "$DUMP")
APP=${APP:-$WORKSPACE/build/native-app/app/macos/app/Pleikkari.app}
RUN="$WORKSPACE/build/native-app/runs/${LABEL:-replay}-$(date +%H%M%S)"
BIN="$APP/Contents/MacOS/Pleikkari"

if [ ! -x "$BIN" ]; then
	echo "no app at $APP: build it with $HERE/build.sh" >&2
	exit 1
fi
if pgrep -f 'Pleikkari.app/Contents/MacOS/Pleikkari' > /dev/null; then
	echo "a Pleikkari process is already running: end it first" >&2
	pgrep -fl 'Pleikkari.app/Contents/MacOS/Pleikkari' >&2
	exit 1
fi

mkdir -p "$RUN"
ARGS=(replay "$DUMP" --loop --stats-interval 5 --no-frame-log --log "$RUN/session.log")
[ "${FULLSCREEN:-0}" = 1 ] && ARGS+=(--fullscreen)
ARGS+=("$@")
printf '%q ' "$BIN" "${ARGS[@]}" > "$RUN/command"
echo >> "$RUN/command"

open -n -a "$APP" --stdout "$RUN/stdout.log" --stderr "$RUN/stdout.log" --args "${ARGS[@]}"
PID=
for _ in $(seq 1 20); do
	PID=$(pgrep -n -f "$BIN") && break
	perl -e 'select(undef,undef,undef,0.25)'
done
if [ -z "$PID" ]; then
	echo "the app did not start; see $RUN/stdout.log" >&2
	exit 1
fi

echo "app         $APP"
echo "pid         $PID"
echo "session log $RUN/session.log"
echo "keys        N neural rendering, 5-6 network, 7-9 size, X/Z strength, E look, C compare, I analysis, M menu"
echo "stats lines grep -E '\\] \\[I\\] (video|nr|analysis):' \"$RUN/session.log\""
