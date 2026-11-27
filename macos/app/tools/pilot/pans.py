"""Pan the camera about for the warp to learn from: pans.py SOCKET ROUNDS"""
import sys, time
from pilot import Game
g = Game(sys.argv[1])
time.sleep(0.3)
try:
    for k in range(int(sys.argv[2])):
        for rx, sec in ((0.8, 0.9), (0, 0.5), (-0.8, 0.9), (0, 0.5), (0.6, 0.5), (0, 0.4), (-0.6, 0.5), (0, 0.4), (1.0, 0.4), (0, 0.5), (-1.0, 0.4), (0, 0.5)):
            g.set(rx=rx)
            time.sleep(sec)
finally:
    g.set(rx=0)
    time.sleep(0.2)
    g.release()
