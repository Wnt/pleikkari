#!/usr/bin/env python3
"""PLE-801: one line per go-pacing.sh arm, for the latch on the frame signal.

    latch_sum.py <out>/<arm>/logcat.txt ...

Per arm:
* over every `Cinema latency:` window after the first 3 (the stream's start), the mean of the
  windows' p50 and p95 of decoded->latched (PLE-698; the ticket's release -> latch) and of the
  total, arrival -> predicted photon (the ticket's frame complete -> photon);
* VrApi's own per-second line from the cinema's process, skipping the first 2: Prd median, and
  the Early, Stale and FPS sums (the head loop must not get worse);
* the `Frame pacing latch on signal:` counters, summed (only with the setting on and the stats log).

The cinema's pid comes from its GoCinema lines, so the arms need no per-frame `trace`.
"""

from __future__ import annotations

import re
import statistics
import sys

FIGURE = r" {} ([\d.]+)/([\d.]+)/([\d.]+)/([\d.]+)"
LATCH = re.compile(r"Frame pacing latch on signal: (\d+) of (\d+) frames waited, (\d+) woken by a frame, (\d+) by the deadline"
                   r" \| wait mean/max ([\d.]+)/([\d.]+) ms \| (\d+) waited and came late")


def pid_of(lines: list[str]) -> str | None:
    for line in lines:
        if re.search(r" GoCinema *: ", line):
            return line.split()[2]
    return None


def figure(line: str, name: str) -> tuple[float, ...] | None:
    m = re.search(FIGURE.format(name), line)
    return tuple(float(x) for x in m.groups()) if m else None


def summarise(path: str) -> str:
    lines = open(path, errors="replace").read().splitlines()
    pid = pid_of(lines)
    ours = [l for l in lines if pid and l.split()[2:3] == [pid]]
    latency = [l for l in ours if "Cinema latency:" in l][3:]
    parts = [f"windows {len(latency)}"]

    def mean_of(name: str, index: int) -> str:
        values = [f[index] for f in (figure(l, name) for l in latency) if f]
        return f"{statistics.fmean(values):.1f}" if values else "-"

    parts.append(f"decoded->latched p50/p95 {mean_of('decoded_latched', 0)}/{mean_of('decoded_latched', 1)}")
    parts.append(f"total p50/p95/mean {mean_of('total', 0)}/{mean_of('total', 1)}/{mean_of('total', 3)}")
    parts.append(f"submitted->photon p50 {mean_of('submitted_photon', 0)}")
    vrapi = [l for l in ours if re.search(r" VrApi *: FPS=", l)][2:]
    if vrapi:
        get = lambda key, l: int(re.search(key + r"=(\d+)", l).group(1))
        parts.append(f"VrApi {len(vrapi)} s: Prd median {statistics.median(get('Prd', l) for l in vrapi):g} ms,"
                     f" Early {sum(get('Early', l) for l in vrapi)}, Stale {sum(get('Stale', l) for l in vrapi)},"
                     f" frames {sum(get('FPS', l) for l in vrapi)}")
    else:
        parts.append("VrApi: no line from the cinema's process")
    latch = [m for m in (LATCH.search(l) for l in ours) if m]
    if latch:
        total = [sum(int(m.group(i)) for m in latch) for i in (1, 2, 3, 4, 7)]
        wait_max = max(float(m.group(6)) for m in latch)
        parts.append(f"latch on signal: {total[0]} of {total[1]} frames waited, {total[2]} woken by a frame,"
                     f" {total[3]} by the deadline, wait max {wait_max:.2f} ms, {total[4]} waited and came late")
    label = path.rstrip("/").split("/")[-2] if "/" in path else path
    return f"{label:18s} " + " | ".join(parts)


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    for path in sys.argv[1:]:
        print(summarise(path))
    return 0


if __name__ == "__main__":
    sys.exit(main())
