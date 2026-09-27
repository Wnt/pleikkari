#!/usr/bin/env python3
"""PLE-722: VrApi's head-loop figures per window of a go-ui.sh session.

go-ui.sh marks each window in logcat (`log -t PLE722 "<label> <window> start|end"`). This reads the
app's own VrApi stats lines (its pid; every VR process prints them) inside each window and prints the
medians of FPS, Prd, Stale, TW, App and LCnt, per GPU clock, because the runtime moves between
clocks by itself (PLE-666).

usage: windows.py <logcat-threadtime.txt> <pid> [--json]
"""
import argparse
import json
import os
import re
import statistics
import sys

sys.path.insert(0, "/home/wnt/gta6/scripts/dev/go-latency")
import go_stats  # noqa: E402

MARK_RE = re.compile(r"^(?P<label>\S+) (?P<window>\S+) (?P<edge>start|end)$")
GPU_RE = re.compile(r"CPU\d/GPU=[^,]*,\d+/(\d+)MHz")
FIELDS = ("FPS", "Prd", "Stale", "TW", "App", "LCnt")


def windows(text, pid):
    marks = {}
    rows = []
    for line in go_stats.parse_logcat(text):
        if line["tag"] == "PLE722":
            m = MARK_RE.match(line["msg"].strip())
            if m:
                marks.setdefault(m.group("window"), {})[m.group("edge")] = line["t"]
        elif line["tag"] == "VrApi" and line["pid"] == pid:
            stats = go_stats.parse_vrapi_stats(line["msg"])
            if stats:
                gpu = GPU_RE.search(line["msg"])
                stats["gpu"] = int(gpu.group(1)) if gpu else None
                stats["t"] = line["t"]
                rows.append(stats)
    out = {}
    for window, edges in marks.items():
        if "start" not in edges or "end" not in edges:
            continue
        inside = [r for r in rows if edges["start"] + 1.0 <= r["t"] <= edges["end"]]
        groups = {}
        for r in inside:
            groups.setdefault(r["gpu"], []).append(r)
        out[window] = {
            "lines": len(inside),
            "by_gpu": {
                str(gpu): {
                    "lines": len(rs),
                    **{f: statistics.median(r[f] for r in rs if isinstance(r.get(f), float)) for f in FIELDS
                       if any(isinstance(r.get(f), float) for r in rs)},
                    "stale_mean": statistics.mean(r["Stale"] for r in rs if isinstance(r.get("Stale"), float)),
                } for gpu, rs in sorted(groups.items(), key=lambda kv: kv[0] or 0)
            },
        }
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("logcat")
    parser.add_argument("pid", type=int)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    with open(args.logcat, errors="replace") as f:
        result = windows(f.read(), args.pid)
    if args.json:
        print(json.dumps(result, indent=2))
        return
    name = os.path.basename(args.logcat)
    for window, w in result.items():
        print(f"{name} {window}: {w['lines']} VrApi lines")
        for gpu, g in w["by_gpu"].items():
            figures = " ".join(f"{k}={g[k]:g}" for k in FIELDS if k in g)
            print(f"  GPU {gpu} MHz x{g['lines']}: {figures} stale_mean={g['stale_mean']:.2f}")


if __name__ == "__main__":
    main()
