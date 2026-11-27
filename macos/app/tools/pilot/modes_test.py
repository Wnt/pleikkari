"""Exercise the camera warp in several of the game's modes and score it in each.

usage: modes_test.py SOCKET RUNDIR [mode ...]   modes: free, armed, aim, inspect
Each mode gets the same half minute of right-stick work (pans, short taps, slow moves,
diagonals, up and down), twice over; the second time is scored from RUNDIR/rec-motion.csv:
how much of the lag the warp hid.
"""
import io
import subprocess
import sys
import time

import numpy as np

def filled(mo, axis):
    """The picture's motion per pair, with pairs that could not be measured given the speed of
    the measured ones around them (a fast turn blurs, and is then not measured: counted as
    standing still it made the truth too small)."""
    import numpy as _np
    dt = (mo[:, 1] - mo[:, 0]) / 1e6
    ok = mo[:, 4] > 0
    v = mo[:, axis] / _np.maximum(dt, 1e-6)
    if ok.sum() >= 2:
        v = _np.interp(mo[:, 1], mo[ok, 1], v[ok])
    return v * dt


from pilot import Game, R3, enter_inspect, level

L1 = 1 << 8
sock, run = sys.argv[1], sys.argv[2]
modes = sys.argv[3:] or ["free", "armed", "aim"]
g = Game(sock)
now_us = lambda: time.clock_gettime_ns(time.CLOCK_UPTIME_RAW) / 1000.0
PROFILE = [(0.8, 0, 0.9), (0, 0, 0.5), (-0.8, 0, 0.9), (0, 0, 0.5), (1.0, 0, 0.5), (0, 0, 0.5), (-1.0, 0, 0.5), (0, 0, 0.5),
           (0.8, 0, 0.15), (0, 0, 0.5), (-0.8, 0, 0.15), (0, 0, 0.5), (0.6, 0, 0.8), (0, 0, 0.4), (-0.6, 0, 0.8), (0, 0, 0.4),
           (0.7, -0.5, 0.5), (0, 0, 0.4), (-0.7, 0.5, 0.5), (0, 0, 0.4), (0, -0.7, 0.35), (0, 0, 0.4), (0, 0.7, 0.35), (0, 0, 0.4),
           (0.9, 0, 0.3), (-0.9, 0, 0.3), (0, 0, 0.6), (1.0, 0, 0.1), (0, 0, 0.4), (-1.0, 0, 0.1), (0, 0, 0.5)]


def wheel(rx, ry):
    """The weapon wheel: L1 held, the right stick towards the entry, L1 let go."""
    g.set(buttons=L1, rx=0, ry=0)
    time.sleep(0.6)
    g.set(buttons=L1, rx=rx, ry=ry)
    time.sleep(0.5)
    g.set(buttons=0, rx=rx, ry=ry)
    time.sleep(0.15)
    g.set(rx=0, ry=0)
    time.sleep(1.0)


def stick_work():
    for rx, ry, sec in PROFILE:
        g.set(rx=rx, ry=ry)
        time.sleep(sec)
    g.set(rx=0, ry=0)
    time.sleep(0.5)


def score(a, b):
    L = open(run + "/rec-motion.csv").read().split("\n")[1:-1]
    mo = np.loadtxt(io.StringIO("\n".join(l for l in L if l.count(",") == 10)), delimiter=",")
    mo = mo[(mo[:, 1] >= a) & (mo[:, 1] <= b)]
    if len(mo) < 50:
        return "too few pictures (%d)" % len(mo)
    pic, valid = mo[:, 1], mo[:, 4]
    lag = float(np.median(mo[:, 7]))
    out = []
    for axis, name in ((2, "sideways"), (3, "up and down")):
        cum = np.cumsum(filled(mo, axis))
        truth = np.interp(pic + (lag + 8) * 1000, pic, cum) - cum
        shown = mo[:, axis + 3]
        rms = lambda v: float(np.sqrt((v ** 2).mean()) * 100)
        out.append("%s: %.2f %% of the picture to hide, %.2f %% left (%.0f %% removed)" % (name, rms(truth), rms(shown - truth), 100 * (1 - rms(shown - truth) / max(1e-9, rms(truth)))))
    return "delay %.0f ms, measured pictures %.0f %%; " % (lag, 100 * valid.mean()) + "; ".join(out)


try:
    time.sleep(0.3)
    for mode in modes:
        g.set(l2=0, buttons=0, rx=0, ry=0)
        time.sleep(0.8)
        if mode == "free":
            wheel(1.0, 0.0)       # the hands are on the right of the wheel
        elif mode in ("armed", "aim"):
            wheel(0.0, -1.0)      # the pistol at the top
        if mode == "aim":
            g.set(l2=1.0)
            time.sleep(1.0)
        if mode == "inspect":
            wheel(1.0, 0.0)
            if not enter_inspect(g, lambda *a: None):
                print(mode, ": could not enter inspect mode", flush=True)
                continue
        stick_work()
        a = now_us()
        stick_work()
        b = now_us()
        if len(sys.argv) > 3 and "--shots" in sys.argv:
            pass
        print("%-8s %s" % (mode, score(a, b)), flush=True)
finally:
    g.set(l2=0, buttons=0, rx=0, ry=0)
    time.sleep(0.2)
    g.release()
