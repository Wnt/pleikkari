#!/usr/bin/env python3
"""PLE-715: the sleep sweep of a go-pacing.sh arm (debug.pleikkari.vr_pacing=trace,sweep=...), one row per sleep step."""
import os, sys, statistics
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pacing import parse
frames, vrapi, period_ms = parse(sys.argv[1])
steps = {}
for f in frames:
    steps.setdefault(round(f.slept / 500) * 0.5, []).append(f)
m = lambda xs: statistics.fmean(xs) / 1000
t0 = frames[0].start
print(" sleep  n   work   wait  P-ret  P-start  cyc   t(s)  lead-changes")
prev = None
for s in sorted(steps):
    fs = steps[s]
    if len(fs) < 20: continue
    cyc = statistics.fmean(b.start - a.start for a, b in zip(fs, fs[1:]) if b.index == a.index + 1) / 1000
    pst = [f.predicted - f.start for f in fs]
    print(f"{s:5.1f} {len(fs):4d} {m([f.work for f in fs]):6.2f} {m([f.wait for f in fs]):6.2f} {m([f.lead for f in fs]):6.2f} {m(pst):7.2f} {cyc:6.2f} {(fs[0].start - t0)/1e6:5.1f}  P-start min/max {min(pst)/1000:.1f}/{max(pst)/1000:.1f}")
