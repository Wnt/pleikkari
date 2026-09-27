#!/usr/bin/env python3
"""PLE-753: how much frame fits VrApi's lead 0 on the Go, from the arms' per-second figures.

For every second VrApi logged (FPS=, Prd=, Stale=, App=) the arm's debug trace gives that
second's frames: frame start to submit (u, the CPU before the eye buffers are flushed to the
GPU). Only seconds at lead 0 (Prd 31-33 ms, Early=0) after the warm-up are used. The seconds
are binned by mean u + App (the frame's CPU lead-in plus its GPU time) and each bin prints the
share of frames VrApi showed stale.

    budget.py <arm dir>... [--skip S] [--bin MS]
"""

from __future__ import annotations

import argparse
import os
import re
import statistics
import sys
from collections import defaultdict

VRAPI = re.compile(r"^\S+ (\S+)\s+(\d+)\s+\d+ I VrApi\s*: FPS=(\d+),Prd=(\d+)ms,Tear=\d+,Early=(\d+),Stale=(\d+),.*?"
                   r"CPU\d/GPU=\d+/\d+,\d+/(\d+)MHz,.*?App=([\d.]+)ms")
TRACE = re.compile(r"^\S+ (\S+)\s+(\d+)\s+\d+ I GoPacing: F \d+ s=-?\d+ z=-?\d+ u=(-?\d+) ")


def seconds(stamp: str) -> float:
    h, m, s = stamp.split(":")
    return int(h) * 3600 + int(m) * 60 + float(s)


def rows(arm: str, skip: float):
    lines = open(os.path.join(arm, "logcat.txt"), errors="replace").read().splitlines()
    trace = [(seconds(m.group(1)), m.group(2), int(m.group(3)) / 1000) for m in (TRACE.match(l) for l in lines) if m]
    if not trace:
        return []
    pid = trace[0][1]
    vr = [m for m in (VRAPI.match(l) for l in lines) if m and m.group(2) == pid]
    live = [m for m in vr if int(m.group(3)) > 1]
    if not live:
        return []
    start = seconds(live[0].group(1)) + skip
    out = []
    for m in vr:
        t = seconds(m.group(1))
        if t < start:
            continue
        fps, prd, early, stale = (int(m.group(i)) for i in (3, 4, 5, 6))
        if early or not 31 <= prd <= 33 or fps < 60:
            continue
        us = [u for (ts, _, u) in trace if t - 1.0 < ts <= t]
        if len(us) < 50:
            continue
        out.append((statistics.fmean(us), float(m.group(8)), stale, fps, int(m.group(7))))
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("arms", nargs="+")
    ap.add_argument("--skip", type=float, default=5.0)
    ap.add_argument("--bin", type=float, default=0.5)
    args = ap.parse_args()
    bins: dict[float, list] = defaultdict(list)
    count = 0
    for arm in args.arms:
        if not os.path.exists(os.path.join(arm, "logcat.txt")):
            continue
        for u, app, stale, fps, mhz in rows(arm, args.skip):
            bins[int((u + app) / args.bin) * args.bin].append((stale, fps, u, app))
            count += 1
    print(f"{count} lead-0 seconds from {len(args.arms)} arms; bins of {args.bin} ms of mean start-to-submit + App")
    print("  u+App ms   seconds  stale/frames   stale %   mean u   mean App")
    for key in sorted(bins):
        b = bins[key]
        stale = sum(x[0] for x in b)
        frames = sum(x[1] for x in b)
        print(f"  {key:5.1f}-{key + args.bin:<5.1f} {len(b):6d}  {stale:5d}/{frames:<6d}  {100 * stale / frames:6.2f}   "
              f"{statistics.fmean(x[2] for x in b):6.2f}   {statistics.fmean(x[3] for x in b):6.2f}")
    return 0 if count else 1


if __name__ == "__main__":
    sys.exit(main())
