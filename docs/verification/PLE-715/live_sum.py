#!/usr/bin/env python3
"""PLE-715: totals of live go-pacing.sh arms: Cinema latency (skipping 3 windows), VrApi Early/Stale/Prd of the app pid, drains, latched fps."""
import os, re, sys, statistics
for path in sys.argv[1:]:
    txt = open(path, errors='replace').read().splitlines()
    pid = next((l.split()[2] for l in txt if ' GoPacing: ' in l), None)
    lat = [l for l in txt if 'Cinema latency:' in l]
    def fig(l, name):
        m = re.search(name + r' ([\d.]+)/([\d.]+)/([\d.]+)/([\d.]+)', l); return tuple(float(x) for x in m.groups()) if m else None
    tot = [fig(l, 'total') for l in lat[3:]]
    sp = [fig(l, 'submitted_photon') for l in lat[3:]]
    ls = [fig(l, 'latched_submitted') for l in lat[3:]]
    dl = [fig(l, 'decoded_latched') for l in lat[3:]]
    rep = sum(int(re.search(r'replaced (\d+)', l).group(1)) for l in lat[3:])
    dec = sum(int(re.search(r'decoded (\d+)', l).group(1)) for l in lat[3:])
    vr = [l for l in txt if l.split()[2:3] == [pid] and 'FPS=' in l][2:]
    st = sum(int(re.search(r'Stale=(\d+)', l).group(1)) for l in vr)
    ea = sum(int(re.search(r'Early=(\d+)', l).group(1)) for l in vr)
    fr = sum(int(re.search(r'FPS=(\d+)', l).group(1)) for l in vr)
    prd = statistics.median(int(re.search(r'Prd=(\d+)', l).group(1)) for l in vr)
    dr = sum(int(re.search(r'(\d+) drained', l).group(1)) for l in txt if 'drained' in l)
    vid = [l for l in txt if 'Cinema video:' in l][1:]
    fps = [float(re.search(r'\(([\d.]+) fps\)', l).group(1)) for l in vid]
    m = lambda xs, i: statistics.fmean(x[i] for x in xs)
    print(f"{path.split('/')[-2]:14s} windows {len(tot)}: total p50 {m(tot,0):.1f} p95 {m(tot,1):.1f} mean {m(tot,3):.1f} | submitted->photon p50 {m(sp,0):.1f} | latched->submitted p50 {m(ls,0):.1f} | decoded->latched p50 {m(dl,0):.1f} | replaced {rep}/{dec} decoded | VrApi {len(vr)} s: Prd median {prd}, Early {ea}/{fr} frames, Stale {st} | drains {dr} | latched fps {', '.join(f'{x:.1f}' for x in fps)}")
