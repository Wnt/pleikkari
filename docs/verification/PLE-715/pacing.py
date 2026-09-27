#!/usr/bin/env python3
"""PLE-715: summarise a Go cinema pacing trace (debug.pleikkari.vr_pacing=trace).

Reads the logcat that docs/verification/PLE-715/go-pacing.sh streams for one arm and prints, per
second of the cinema: frames, how many submits VrApi throttled (held until its release) or let
through at once (late), the loop's own sleep, frame start to submit, the wait inside
vrapi_SubmitFrame2, predicted display minus submit (VrApi's lead) and minus frame start (the video
latch to predicted photon), each frame's lead in refreshes, and the app's own VrApi line of that
second (FPS, Prd, Early, Stale).

    pacing.py <arm>/logcat.txt [--frames A:B] [--transitions]

Trace line fields (vr-cinema.cpp recordPacing, microseconds): s frame start (CLOCK_MONOTONIC),
z the loop's sleep before it, u start to the submit call, r the wait inside the submit, p the
predicted display time minus the submit's return.
"""

from __future__ import annotations

import argparse
import re
import statistics
import sys
from dataclasses import dataclass

TRACE = re.compile(r"^(\S+ \S+)\s+(\d+)\s+(\d+) I GoPacing: F (\d+) s=(-?\d+) z=(-?\d+) u=(-?\d+) r=(-?\d+) p=(-?\d+)")
VRAPI = re.compile(r"^(\S+ \S+)\s+(\d+)\s+\d+ I VrApi\s*: FPS=(\d+),Prd=(\d+)ms,Tear=\d+,Early=(\d+),Stale=(\d+)")
REFRESH = re.compile(r"GoCinema: Frame pacing: .*period ([\d.]+) ms")


@dataclass
class Frame:
    index: int
    start: int  # us
    slept: int
    work: int
    wait: int
    lead: int  # predicted - return

    @property
    def submit(self) -> int:
        return self.start + self.work

    @property
    def returned(self) -> int:
        return self.submit + self.wait

    @property
    def predicted(self) -> int:
        return self.returned + self.lead


def parse(path: str):
    frames: list[Frame] = []
    vrapi: list[tuple[str, int, int, int, int]] = []
    pid = None
    period_ms = None
    lines = open(path, errors="replace").read().splitlines()
    for line in lines:
        m = TRACE.match(line)
        if m:
            pid = pid or m.group(2)
            frames.append(Frame(int(m.group(4)), *(int(m.group(i)) for i in range(5, 10))))
            continue
        m = REFRESH.search(line)
        if m and period_ms is None:
            period_ms = float(m.group(1))
    for line in lines:
        m = VRAPI.match(line)
        if m and m.group(2) == pid:
            vrapi.append((m.group(1), int(m.group(3)), int(m.group(4)), int(m.group(5)), int(m.group(6))))
    return frames, vrapi, period_ms or 1000 / 72


def ms(us: float) -> str:
    return f"{us / 1000:6.2f}"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("logcat")
    ap.add_argument("--frames", help="print these frames one per line, A:B")
    ap.add_argument("--transitions", action="store_true", help="print the frames around each change of lead")
    args = ap.parse_args()
    frames, vrapi, period_ms = parse(args.logcat)
    if not frames:
        print("no GoPacing trace lines", file=sys.stderr)
        return 1
    period = period_ms * 1000
    # A frame's lead: predicted display minus the submit's return, in refreshes, less the 1.38 a
    # throttled submit shows at lead 0 on the Go (vr-frame-pacing.h). Only throttled submits
    # (3 ms or more inside the call) return at VrApi's release; the rest read -1.
    def lead_of(f: Frame) -> int:
        if f.wait < 3000:
            return -1
        x = f.lead / period - 1.38
        return 0 if x < 0.5 else 1 if x < 1.5 else 2

    if args.frames:
        a, b = (int(x) for x in args.frames.split(":"))
        base = frames[0].start
        print(" frame   start ms  slept  work   wait  P-sub  P-start  lead   ret-phase")
        grid0 = next((f.returned for f in frames if f.wait >= 3000), frames[0].returned)
        for f in frames:
            if a <= f.index <= b:
                phase = ((f.returned - grid0) % period) / period
                print(f"{f.index:6d} {ms(f.start - base):>9} {ms(f.slept)} {ms(f.work)} {ms(f.wait)} {ms(f.predicted - f.submit)} "
                      f"{ms(f.predicted - f.start)}  {lead_of(f):3d}   {phase:5.2f}")
        return 0

    if args.transitions:
        prev = None
        for i, f in enumerate(frames):
            lead = lead_of(f)
            if prev is not None and lead != prev and i > 5:
                print(f"-- frame {f.index}: lead {prev} -> {lead}")
                for g in frames[max(0, i - 4): i + 3]:
                    print(f"   {g.index:6d} slept {ms(g.slept)} work {ms(g.work)} wait {ms(g.wait)} P-ret {ms(g.lead)} lead {lead_of(g)}")
            prev = lead
        return 0

    # Per second of cinema time.
    t0 = frames[0].start
    buckets: dict[int, list[Frame]] = {}
    for f in frames:
        buckets.setdefault(int((f.start - t0) // 1_000_000), []).append(f)
    print(f"period {period_ms:.3f} ms; {len(frames)} frames; VrApi lines {len(vrapi)}")
    print("  s frames thr late  slept  work   wait   P-sub  P-start  leads(0/1/2)      VrApi")
    vi = 0
    for second in sorted(buckets):
        fs = buckets[second]
        thr = sum(1 for f in fs if f.wait >= 3000)
        leads = [lead_of(f) for f in fs]
        mean = lambda xs: statistics.fmean(xs) if xs else 0.0
        v = vrapi[second] if second < len(vrapi) else None
        vs = f"FPS={v[1]} Prd={v[2]} Early={v[3]} Stale={v[4]}" if v else ""
        print(f"{second:3d} {len(fs):5d} {thr:4d} {len(fs) - thr:4d} {ms(mean([f.slept for f in fs]))} {ms(mean([f.work for f in fs]))} "
              f"{ms(mean([f.wait for f in fs]))} {ms(mean([f.predicted - f.submit for f in fs]))} {ms(mean([f.predicted - f.start for f in fs]))}"
              f"  {leads.count(0):3d}/{leads.count(1):3d}/{leads.count(2):3d}   {vs}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
