#!/bin/zsh
# restart.sh RUNDIR-NAME [extra app options]: install the current build and start a recording, controllable stream
W=${PLK_WORKSPACE:-$HOME/Documents/pleikkari-android}
pkill -TERM -f 'Pleikkari.app/Contents/MacOS/Pleikkari'
for i in $(seq 1 40); do pgrep -f 'Pleikkari.app/Contents/MacOS/Pleikkari' >/dev/null || break; perl -e 'select(undef,undef,undef,0.5)'; done
(cd $W/wt/native-nr && JOBS=12 macos/app/build.sh ../../build/native-app/app --no-tests 2>&1 | grep -E ' error')
D=$W/build/native-app/runs/$1; mkdir -p $D; shift
perl -e 'select(undef,undef,undef,8)'
open -n ${PLK_OPEN_ENV:+--env} ${PLK_OPEN_ENV} -a $W/build/native-app/app/macos/app/Pleikkari.app --args stream PS5-466 192.168.1.164 --nr off --frame-generation off --camera-warp on --hdr on --fps 60 --resolution 1080p --bitrate 50000 --no-frame-log --retry-window 40 --record-input $D/rec --control /tmp/plk-control.sock --log $D/session.log "$@"
perl -e 'select(undef,undef,undef,14)'
grep -E 'control:|\[E\]' $D/session.log | tail -2 | cut -c1-120
