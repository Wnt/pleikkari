#!/usr/bin/env python3
"""PLE-823: summarise the S22 sessions' temporary `PLE823 state` log lines.

usage: analyze.py <label>=<logcat.txt> ...

Each state packet the sender sent logs one line: its sequence number, the left stick it
carried, and, when a stick change was pending, how long after the last and the first
unsent change it went out and how long after the previous state packet the last change came.
The stimulus (stim.sh) sweeps the left stick 0 -> 4915 (15 %) -> 0, 40 times; only lines
between its "stim start" and "stim done" marks count.
"""
import re
import sys

LINE = re.compile(r"PLE823 state seq=(\d+) left_x=(-?\d+) (?:(keepalive)|last_change_to_sent_us=(\d+) "
                  r"first_change_to_sent_us=(\d+) last_change_after_state_us=(\d+))")
MIN_US = 8000
END = 4915  # 0.15 * 32767: the sweep's far end, the UP event of the outward swipe
CENTRE = 0


def pct(values, p):
    values = sorted(values)
    if not values:
        return float("nan")
    return values[min(len(values) - 1, int(round(p / 100 * (len(values) - 1))))]


def fmt(values):
    if not values:
        return "n=0"
    ms = lambda us: f"{us / 1000:.1f}"
    return f"n={len(values)} p50 {ms(pct(values, 50))} / p95 {ms(pct(values, 95))} / max {ms(max(values))} ms"


def summarise(label, path):
    packets = []
    inside = False
    with open(path, errors="replace") as fh:
        for line in fh:
            if "stim start" in line:
                inside = True
                continue
            if "stim done" in line:
                inside = False
                continue
            m = LINE.search(line)
            if not m or not inside:
                continue
            seq, left_x = int(m.group(1)), int(m.group(2))
            if m.group(3):
                packets.append(dict(seq=seq, left_x=left_x, keepalive=True))
            else:
                packets.append(dict(seq=seq, left_x=left_x, keepalive=False, last=int(m.group(4)),
                                    first=int(m.group(5)), after_state=int(m.group(6))))
    changes = [p for p in packets if not p["keepalive"]]
    seqs = [p["seq"] for p in packets]
    gaps = sum(1 for a, b in zip(seqs, seqs[1:]) if b != a + 1)
    centre = [p for p in changes if p["left_x"] == CENTRE]
    end = [p for p in changes if p["left_x"] == END]
    inside_min = [p for p in changes if p["after_state"] < MIN_US]
    out = [
        f"== {label} ({path})",
        f"state packets during the sweeps: {len(packets)} (seq {seqs[0]}-{seqs[-1]}, {gaps} gaps), "
        f"{len(changes)} carrying a stick change, {len(packets) - len(changes)} keepalives",
        f"stick back at centre (left_x=0), change -> sent: {fmt([p['last'] for p in centre])}",
        f"far end of the sweep (left_x={END}) sent: {len(end)} of 40 sweeps; change -> sent: {fmt([p['last'] for p in end])}",
        f"last change queued inside the 8 ms minimum: {len(inside_min)} packets; last change -> sent: "
        f"{fmt([p['last'] for p in inside_min])}",
        f"oldest pending change -> sent, every change packet: {fmt([p['first'] for p in changes])}",
        f"  of which over 20 ms: {sum(1 for p in changes if p['first'] > 20000)}",
    ]
    return "\n".join(out)


def main(argv):
    for arg in argv:
        label, path = arg.split("=", 1)
        print(summarise(label, path))
        print()


if __name__ == "__main__":
    main(sys.argv[1:])
