#!/usr/bin/env python3
"""PLE-654: per-arm summary of a live Go VR cinema capture (go-live.sh output).

Usage: summarize.py <out dir>/<label> ...   (reads gocinema-, logcat-, session-<label> from go-live.sh)
Cinema video: first window skipped (connect). Feedback stats: first 5 windows skipped.
VrApi: only lines from the cinema's pid, first 5 s after "cinema entered" skipped.
"""
import os
import re
import statistics
import sys

LATCH = re.compile(r"Cinema video: (\d+) decoder frames latched in ([\d.]+) s \(([\d.]+) fps\), (\d+) of (\d+) submitted")
ENTER = re.compile(r"^(\S+ \S+)\s+(\d+)\s+\d+ I GoCinema: VrApi cinema entered; requested (\d+) Hz")
VRAPI = re.compile(r"^\S+ \S+\s+(\d+)\s+\d+ I VrApi\s*: FPS=(\d+),.*?Early=(\d+),Stale=(\d+),.*?App=([\d.]+)ms")
STATS = re.compile(r"Feedback stats: window (\d+) ms video received (\d+) decoded (\d+) dropped_input (\d+) dropped_presenter (\d+)")
ENVGPU = re.compile(r"Environment frame: gpu ([\d.]+) ms")


def arm(prefix):
    d, label = os.path.split(prefix)
    read = lambda name: open(os.path.join(d, f"{name}-{label}.{'log' if name == 'session' else 'txt'}"), errors="replace").read().splitlines()
    latch = [LATCH.search(l) for l in read("gocinema")]
    latch = [m for m in latch if m][1:]
    fps = [float(m.group(3)) for m in latch]
    lat = sum(int(m.group(1)) for m in latch)
    secs = sum(float(m.group(2)) for m in latch)
    shown = sum(int(m.group(4)) for m in latch)
    sub = sum(int(m.group(5)) for m in latch)
    env = [float(m.group(1)) for m in map(ENVGPU.search, read("gocinema")) if m]
    pid, hz, vr = None, None, []
    for line in read("logcat"):
        m = ENTER.search(line)
        if m:
            pid, hz, skip = m.group(2), m.group(3), 5
            continue
        m = VRAPI.search(line)
        if m and pid and m.group(1) == pid:
            if skip > 0:
                skip -= 1
                continue
            vr.append(m.groups()[1:])
    stats = [m.groups() for m in map(STATS.search, read("session")) if m][5:]
    win = sum(int(s[0]) for s in stats) / 1000.0
    dec = sum(int(s[2]) for s in stats)
    rec = sum(int(s[1]) for s in stats)
    med = lambda xs: statistics.median(xs) if xs else float("nan")
    print(f"{label}: requested {hz} Hz"
          f" | cinema latched {lat} in {secs:.0f} s = {lat / secs:.1f} fps (5 s windows {min(fps):.1f}-{max(fps):.1f}), video shown on {shown}/{sub} submitted"
          f" | decoder {dec} decoded / {rec} received in {win:.0f} s = {dec / win:.1f} fps, dropped_input {stats[-1][3]} dropped_presenter {stats[-1][4]}"
          f" | VrApi {len(vr)} s: FPS median {med([int(v[0]) for v in vr])}, Early median {med([int(v[1]) for v in vr])},"
          f" Stale total {sum(int(v[2]) for v in vr)} (max {max(int(v[2]) for v in vr)}/s), App median {med([float(v[3]) for v in vr]):.2f} ms"
          + (f" | Environment frame gpu median {med(env):.2f} ms" if env else ""))
    print(f"    latch deficit vs decoder: {dec / win - lat / secs:.1f} fps ({100 * (1 - (lat / secs) / (dec / win)):.1f}% of decoded frames never latched)")


for p in sys.argv[1:]:
    arm(p)
