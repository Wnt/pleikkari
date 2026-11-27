#!/bin/zsh
# capture_pan.sh OUTDIR: pan right and left and tap, capturing the app's window meanwhile
W=${PLK_WORKSPACE:-$HOME/Documents/pleikkari-android}; A=$W/build/native-app/runs/auto; S=$1; mkdir -p "$S"
WID=$(swift $A/winid.swift 2>/dev/null | tail -1)
(cd $A && $W/build/mac/venv/bin/python - <<'PY' &
import time
from pilot import Game
g=Game('/tmp/plk-control.sock')
time.sleep(0.5)
for rx,sec in ((0.8,1.0),(0,0.5),(-0.8,1.0),(0,0.5),(0.8,0.25),(0,0.5),(-0.8,0.25),(0,0.5)):
    g.set(rx=rx); time.sleep(sec)
g.set(rx=0); time.sleep(0.2); g.release()
PY
)
perl -e 'select(undef,undef,undef,0.55)'
for i in $(seq 1 16); do screencapture -x -o -l $WID -t jpg "$S/p$i.jpg"; done
wait
cd "$S" && $W/build/mac/venv/bin/python - <<'PY'
import cv2,numpy as np
ims=[cv2.resize(cv2.imread('p%d.jpg'%i),(840,490)) for i in range(1,17)]
cv2.imwrite('pan-a.jpg',np.vstack([np.hstack(ims[r*2:r*2+2]) for r in range(4)]),[cv2.IMWRITE_JPEG_QUALITY,82])
cv2.imwrite('pan-b.jpg',np.vstack([np.hstack(ims[8+r*2:8+r*2+2]) for r in range(4)]),[cv2.IMWRITE_JPEG_QUALITY,82])
PY
