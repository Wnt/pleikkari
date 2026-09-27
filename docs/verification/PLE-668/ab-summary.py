#!/usr/bin/env python3
"""PLE-668: one row per go-ab.sh arm: VrApi Stale/s, App=, GPU clock and load, battery current, temperatures.

Usage: ab-summary.py <out dir> [skip-seconds=10]
Reads vrapi-fps-<arm>.txt, gocinema-<arm>.txt and samples-<arm>.txt. The first `skip` VrApi
lines (one per second) are dropped as start-up. current_now is signed as the kernel reports it
(negative = the battery is being charged, the Go was on AC), so compare arms, not absolutes.
"""
import glob, os, re, statistics as st, sys

out = sys.argv[1]
skip = int(sys.argv[2]) if len(sys.argv) > 2 else 10
print("| arm | clock line | s | Stale/s mean | Stale total | App= ms | GPU MHz | GPU% | VrApi Temp start→end | uA mean | batt °C start→end | pm °C start→end |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|")
for path in sorted(glob.glob(os.path.join(out, "vrapi-fps-*.txt"))):
    arm = path.rsplit("vrapi-fps-", 1)[1][:-4]
    rows = [l for l in open(path) if "FPS=" in l][skip:]
    def f(key, cast=float):
        return [cast(m.group(1)) for m in (re.search(key, l) for l in rows) if m]
    stale = f(r"Stale=(\d+)", int)
    app = f(r"App=([\d.]+)ms")
    mhz = f(r"CPU2/GPU=\d+/\d+,\d+/(\d+)MHz", int)
    gpu = f(r"GPU%=([\d.]+)")
    temp = f(r"Temp=([\d.]+)C")
    clock = ""
    cine = os.path.join(out, f"gocinema-{arm}.txt")
    if os.path.exists(cine):
        m = re.findall(r"Clock levels (.*)", open(cine).read())
        clock = m[-1] if m else ""
    samples = [dict(kv.split("=") for kv in l.split()[2:]) for l in open(os.path.join(out, f"samples-{arm}.txt"))]
    ua = st.mean(int(s["uA"]) for s in samples)
    print(f"| {arm} | {clock} | {len(rows)} | {st.mean(stale):.2f} | {sum(stale)} | {st.mean(app):.2f} | "
          f"{st.mode(mhz)} | {st.mean(gpu):.2f} | {temp[0]:.1f}→{temp[-1]:.1f} | {ua:.0f} | "
          f"{int(samples[0]['batt'])/1000:.1f}→{int(samples[-1]['batt'])/1000:.1f} | "
          f"{int(samples[0]['pm'])/1000:.1f}→{int(samples[-1]['pm'])/1000:.1f} |")
