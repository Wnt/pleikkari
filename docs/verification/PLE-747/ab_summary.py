#!/usr/bin/env python3
"""PLE-815: merge PLE-747's per-arm input_to_photon.py outputs into one summary table.

usage: ab_summary.py <arms-dir>
  <arms-dir>/<arm>/presses.csv   input_to_photon.py analyze's presses.csv (status "ok" rows count)
  <arm>/logcat.txt               optional: the arm's go-probe logcat (threadtime); VrApi's per-second
                                 line from the cinema's own process gives Prd, Stale and Early share.
Prints markdown: per arm the per-press KeyEvent -> photon p50/p95, the Go-side stages' p50, and VrApi.
"""
import csv
import os
import re
import sys

sys.path.insert(0, "/home/wnt/gta6/scripts/dev/go-latency")
from input_to_photon import PRESS_STAGES, pct  # noqa: E402
from go_stats import parse_logcat, parse_vrapi_stats  # noqa: E402

# The Go-side stages: from the app receiving the press to its photon, minus the network + PS5 leg.
GO_STAGES = [(a, b) for a, b, _ in PRESS_STAGES if (a, b) != ("sent", "ready")]
CINEMA_TAGS = ("GoCinema", "GoVrEntry")


def floats(rows, column):
    out = []
    for row in rows:
        try:
            out.append(float(row[column]))
        except (KeyError, TypeError, ValueError):
            pass
    return out


def press_stats(path):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    ok = [r for r in rows if r.get("status") == "ok"]
    e2p = floats(ok, "event_to_photon_ms")
    stats = {"presses": len(rows), "ok": len(ok),
             "p50": pct(e2p, 50) if e2p else None, "p95": pct(e2p, 95) if e2p else None, "stages": {}}
    for a, b in GO_STAGES:
        values = floats(ok, "%s_to_%s_ms" % (a, b))
        stats["stages"][(a, b)] = pct(values, 50) if values else None
    return stats


def vrapi_stats(text):
    records = list(parse_logcat(text))
    pids = {r["pid"] for r in records if r["tag"] in CINEMA_TAGS}
    lines = [s for s in (parse_vrapi_stats(r["msg"]) for r in records if r["tag"] == "VrApi" and r["pid"] in pids) if s]
    if not lines:
        return None
    prd = [s["Prd"] for s in lines if isinstance(s.get("Prd"), float)]
    fps = sum(s["FPS"] for s in lines if isinstance(s.get("FPS"), float))
    early = sum(s["Early"] for s in lines if isinstance(s.get("Early"), float))
    stale = sum(s["Stale"] for s in lines if isinstance(s.get("Stale"), float))
    return {"seconds": len(lines), "prd_p50": pct(prd, 50) if prd else None,
            "stale_per_s": stale / len(lines), "early_share": early / fps if fps else None}


def fmt(value, spec="%.1f"):
    return "n/a" if value is None else spec % value


def summary(arms_dir):
    head = (["arm", "ok/presses", "event->photon p50", "p95"] + ["%s->%s" % s for s in GO_STAGES]
            + ["VrApi s", "Prd p50", "Stale/s", "Early share"])
    lines = ["PLE-747 arms, ms (Go-side stages are per-press p50):", "",
             "| " + " | ".join(head) + " |", "|" + "---|" * len(head)]
    for arm in sorted(os.listdir(arms_dir)):
        presses = os.path.join(arms_dir, arm, "presses.csv")
        if not os.path.isfile(presses):
            continue
        p = press_stats(presses)
        row = [arm, "%d/%d" % (p["ok"], p["presses"]), fmt(p["p50"]), fmt(p["p95"])]
        row += [fmt(p["stages"][s]) for s in GO_STAGES]
        log = os.path.join(arms_dir, arm, "logcat.txt")
        v = vrapi_stats(open(log, errors="replace").read()) if os.path.isfile(log) else None
        if v:
            row += [str(v["seconds"]), fmt(v["prd_p50"], "%.0f"), fmt(v["stale_per_s"], "%.2f"),
                    fmt(v["early_share"], "%.2f")]
        else:
            row += ["0", "n/a", "n/a", "n/a"]
        lines.append("| " + " | ".join(row) + " |")
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    sys.stdout.write(summary(sys.argv[1]))
