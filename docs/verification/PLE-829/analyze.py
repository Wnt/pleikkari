#!/usr/bin/env python3
"""PLE-829: summarise the S22 A/B of stream_pad_input_thread from each session's presses.csv.

Every <session> is a directory s22-probe.sh wrote; its session.txt names the arm. Per arm, over
all its sessions, the stages of PLE-746's probe (CLOCK_MONOTONIC ns; the KeyEvent's time has ms
resolution, so `input -> app` carries up to 1 ms of rounding, the same on both arms):

  input -> app      KeyEvent.getEventTime() to StreamInput's key dispatch (the arm's stage)
  app -> state      StreamInput to the controller state handed to chiaki
  state -> sent     that state to the feedback history packet that carried it
  input -> sent     the whole

  analyze.py <session>... [--out FILE]
"""
import argparse
import csv
import os
import re
import statistics
import sys

STAGES = [
    ("input_event_ns", "app_received_ns", "input -> app"),
    ("app_received_ns", "state_set_ns", "app -> state"),
    ("state_set_ns", "sent_ns", "state -> sent"),
    ("input_event_ns", "sent_ns", "input -> sent"),
]


def arm_of(session):
    text = open(os.path.join(session, "session.txt")).read()
    found = re.search(r"arm (on|off) ", text)
    if not found:
        raise SystemExit("%s: session.txt names no arm" % session)
    return found.group(1)


def read_presses(session):
    path = os.path.join(session, "presses.csv")
    if not os.path.exists(path):
        return []
    with open(path) as f:
        return [{k: int(v) for k, v in row.items()} for row in csv.DictReader(f)]


def percentile(values, p):
    """Nearest rank."""
    ordered = sorted(values)
    rank = max(1, -(-len(ordered) * p // 100))
    return ordered[int(rank) - 1]


def summarise(presses):
    complete = [p for p in presses if p["input_event_ns"] and p["app_received_ns"] and p["state_set_ns"] and p["sent_ns"]]
    lines = ["presses %d, complete %d" % (len(presses), len(complete))]
    for a, b, label in STAGES:
        ms = [(p[b] - p[a]) / 1e6 for p in presses if p[a] and p[b]]
        if not ms:
            lines.append("  %-14s -" % label)
            continue
        lines.append("  %-14s n %3d  p50 %6.2f  p95 %6.2f  max %6.2f  mean %6.2f ms" % (
            label, len(ms), percentile(ms, 50), percentile(ms, 95), max(ms), statistics.fmean(ms)))
    return lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sessions", nargs="+")
    ap.add_argument("--out")
    args = ap.parse_args()
    by_arm = {"off": [], "on": []}
    out = []
    for session in args.sessions:
        arm = arm_of(session)
        presses = read_presses(session)
        by_arm[arm] += presses
        out.append("%s (%s): %s" % (os.path.basename(os.path.normpath(session)), arm, summarise(presses)[0]))
    for arm in ("off", "on"):
        out.append("")
        out.append("arm %s, all sessions:" % arm)
        out += summarise(by_arm[arm])
    text = "\n".join(out) + "\n"
    sys.stdout.write(text)
    if args.out:
        open(args.out, "w").write(text)


if __name__ == "__main__":
    main()
