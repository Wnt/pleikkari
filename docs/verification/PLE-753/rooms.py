#!/usr/bin/env python3
"""PLE-753: summarise Go cinema arms (a room or the plain screen) from the logcats that
docs/verification/PLE-715/go-pacing.sh streams, one row per arm.

    rooms.py <session-dir>/<arm> ...  [--skip S] [--json]

Per arm, over the app's own VrApi lines after the first S seconds (default 5):
  App   VrApi's eye-buffer GPU time, median ms      TW     its TimeWarp GPU time, median ms
  MHz   the GPU clocks VrApi reported, with counts  Prd    VrApi's prediction, median ms
  Stale frames TimeWarp showed again, total and worst second
  Early frames VrApi counted a refresh early, total  FPS   minimum and median
and from the cinema's "Frame pacing" lines over the same seconds: the pacing mode, frames at
lead 0/1/2+, drains, late (unthrottled) submits, and frame start to submit (mean of the
per-second means, worst max). The arm's room and MSAA come from the cinema's own log lines.
With the debug trace (vr_pacing=trace), frame start to submit is split, p50/p95 in ms: start to
draw() (the loop's input, latch and UI), draw() to its glFlush (GL commands), the glFlush.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import statistics
import sys
from collections import Counter

VRAPI = re.compile(r"^\S+ (\S+)\s+(\d+)\s+\d+ I VrApi\s*: FPS=(\d+),Prd=(\d+)ms,Tear=\d+,Early=(\d+),Stale=(\d+),.*?"
                   r"CPU\d/GPU=(\d+)/(\d+),(\d+)/(\d+)MHz,.*?TW=([\d.]+)ms,App=([\d.]+)ms")
PACING = re.compile(r"^\S+ (\S+)\s+(\d+)\s+\d+ I GoCinema: Frame pacing \(([^)]+)\): (\d+) frames, (\d+) throttled, (\d+) late"
                    r".*?start to submit mean/max ([\d.]+)/([\d.]+) ms.*?leads 0/1/2\+ (\d+)/(\d+)/(\d+), (\d+) drained")
ENTERED = re.compile(r"GoCinema: VrApi cinema entered; requested (\d+) Hz, eye (\d+)x(\d+).*environment MSAA (\d+)x")
ENVIRONMENT = re.compile(r"GoCinema: Environment (\w+): (?:screen|black)")
CLOCKS = re.compile(r"GoCinema: Clock levels CPU (\d+) / GPU (\d+)")
MODE = re.compile(r"GoCinema: Frame pacing: ([a-zA-Z ]+?)(?:,| \(|$)")
# The debug trace (vr-cinema.cpp recordPacing, us): u frame start to submit; since PLE-753 also d
# start to draw(), g draw() to its glFlush, f the glFlush.
TRACE = re.compile(r"^\S+ (\S+)\s+(\d+)\s+\d+ I GoPacing: F \d+ s=-?\d+ z=-?\d+ u=(-?\d+) r=-?\d+ p=-?\d+"
                   r"(?: d=(-?\d+) g=(-?\d+) f=(-?\d+))?")


def seconds(stamp: str) -> float:
    h, m, s = stamp.split(":")
    return int(h) * 3600 + int(m) * 60 + float(s)


def summarise(arm: str, skip: float) -> dict:
    path = os.path.join(arm, "logcat.txt")
    lines = open(path, errors="replace").read().splitlines()
    pid = None
    info: dict = {"arm": os.path.basename(arm.rstrip("/"))}
    for line in lines:
        if " GoCinema: " in line and pid is None:
            pid = line.split()[2]
        m = ENTERED.search(line)
        if m:
            info.update(hz=int(m.group(1)), eye=f"{m.group(2)}x{m.group(3)}", msaa=int(m.group(4)))
        m = ENVIRONMENT.search(line)
        if m:
            info["environment"] = m.group(1)
        m = CLOCKS.search(line)
        if m:
            info["levels"] = f"{m.group(1)}/{m.group(2)}"
        m = MODE.search(line)
        if m:
            info["mode"] = m.group(1).strip()
    vr = [m for m in (VRAPI.match(l) for l in lines) if m and m.group(2) == pid]
    pc = [m for m in (PACING.match(l) for l in lines) if m and m.group(2) == pid]
    # The window: VrApi's first line after the cinema came up (FPS > 1), plus skip seconds.
    start = next((seconds(m.group(1)) for m in vr if int(m.group(3)) > 1), None)
    if start is None:
        info["error"] = "no VrApi lines from the app"
        return info
    vr = [m for m in vr if seconds(m.group(1)) >= start + skip]
    pc = [m for m in pc if seconds(m.group(1)) >= start + skip]
    if not vr:
        info["error"] = "no VrApi lines after the warm-up"
        return info
    stale = [int(m.group(6)) for m in vr]
    tr = [m for m in (TRACE.match(l) for l in lines) if m and m.group(2) == pid and seconds(m.group(1)) >= start + skip]
    if tr:
        def pct(values, q):
            values = sorted(values)
            return round(values[min(len(values) - 1, int(q * len(values)))] / 1000, 2)
        for key, group in (("u", 3), ("d", 4), ("g", 5), ("f", 6)):
            values = [int(m.group(group)) for m in tr if m.group(group) is not None]
            if values:
                info[f"trace_{key}"] = [pct(values, 0.5), pct(values, 0.95)]
    info.update(
        seconds=len(vr),
        app_ms=round(statistics.median(float(m.group(12)) for m in vr), 2),
        tw_ms=round(statistics.median(float(m.group(11)) for m in vr), 2),
        prd_ms=statistics.median(int(m.group(4)) for m in vr),
        prd_range=f"{min(int(m.group(4)) for m in vr)}-{max(int(m.group(4)) for m in vr)}",
        stale=sum(stale), stale_max=max(stale),
        stale_seconds=sum(1 for s in stale if s),
        early=sum(int(m.group(5)) for m in vr),
        fps_min=min(int(m.group(3)) for m in vr), fps_median=statistics.median(int(m.group(3)) for m in vr),
        gpu_mhz=dict(sorted(Counter(int(m.group(10)) for m in vr).items())),
        gpu_levels=dict(sorted(Counter(int(m.group(8)) for m in vr).items())),
    )
    if pc:
        info.update(
            pacing=pc[-1].group(3),
            frames=sum(int(m.group(4)) for m in pc),
            late=sum(int(m.group(6)) for m in pc),
            leads=[sum(int(m.group(i)) for m in pc) for i in (9, 10, 11)],
            drains=sum(int(m.group(12)) for m in pc),
            work_ms=round(statistics.fmean(float(m.group(7)) for m in pc), 2),
            work_max_ms=max(float(m.group(8)) for m in pc),
        )
    return info


def row(i: dict) -> str:
    if "error" in i:
        return f"{i['arm']:<22} {i['error']}"
    mhz = " ".join(f"{k}:{v}" for k, v in i["gpu_mhz"].items())
    leads = "/".join(str(x) for x in i.get("leads", [])) or "-"
    return (f"{i['arm']:<22} {i.get('environment', '?'):<8} {i.get('msaa', '?')}x {i.get('pacing', i.get('mode', '?')):<15} "
            f"{i['seconds']:>3}s App {i['app_ms']:5.2f} TW {i['tw_ms']:4.2f} Prd {i['prd_ms']:>4} ({i['prd_range']}) "
            f"Stale {i['stale']:>4} (max {i['stale_max']}/s, {i['stale_seconds']} s) Early {i['early']:>5} FPS {i['fps_min']}/{i['fps_median']} "
            f"leads {leads} drains {i.get('drains', '-')} late {i.get('late', '-')} work {i.get('work_ms', '-')}/{i.get('work_max_ms', '-')} ms "
            f"GPU {mhz}"
            + ("".join(f" {k[6:]} {v[0]}/{v[1]}" for k, v in i.items() if k.startswith("trace_"))))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("arms", nargs="+")
    ap.add_argument("--skip", type=float, default=5.0)
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()
    out = [summarise(a, args.skip) for a in args.arms if os.path.exists(os.path.join(a, "logcat.txt"))]
    if args.json:
        print(json.dumps(out, indent=1))
    else:
        for i in out:
            print(row(i))
    return 0 if out and all("error" not in i for i in out) else 1


if __name__ == "__main__":
    sys.exit(main())
