#!/usr/bin/env python3
"""PLE-199: per-thread CPU (utime+stime delta) from <session>_threads.txt files.

    aggregate.py <captures dir> r1_on r2_off ...

Threads are grouped by comm (two "Chiaki Takion" threads sum into one row). The CLK_TCK
is read from <session>_proc_cpu.txt. Prints a markdown table of cores (CPU-s / wall-s).
"""
import re
import sys
from collections import defaultdict
from pathlib import Path


def snapshots(path):
    snaps, cur = {}, None
    for line in path.read_text().splitlines():
        m = re.match(r'^(start|end) (\d+) pid=(\d+)$', line)
        if m:
            cur = snaps[m.group(1)] = {'t': int(m.group(2)), 'pid': m.group(3), 'tid': {}}
            continue
        tid, comm, ut, st = line.split('\t')
        comm = re.sub(r'^(Hw)?[Bb]inder:.*', 'binder (all)', comm)
        cur['tid'][tid] = (comm, int(ut) + int(st))
    return snaps


def session(cap, name):
    s = snapshots(cap / f'{name}_threads.txt')
    clk = int(re.search(r'clk_tck=(\d+)', (cap / f'{name}_proc_cpu.txt').read_text()).group(1))
    a, b = s['start'], s['end']
    assert a['pid'] == b['pid'], name
    wall = b['t'] - a['t']
    per = defaultdict(float)
    for tid, (comm, ticks) in b['tid'].items():
        per[comm] += (ticks - a['tid'].get(tid, (comm, 0))[1]) / clk
    return wall, per


def main():
    cap = Path(sys.argv[1])
    names = sys.argv[2:]
    data = {n: session(cap, n) for n in names}
    total = {n: sum(p.values()) for n, (_, p) in data.items()}
    comms = sorted({c for _, p in data.values() for c in p},
                   key=lambda c: -sum(data[n][1].get(c, 0) for n in names))
    print('| thread | ' + ' | '.join(names) + ' | off mean | on mean | on-off % |')
    print('|---|' + '---|' * (len(names) + 3))
    rows = [('all threads', {n: total[n] for n in names})]
    rows += [(c, {n: data[n][1].get(c, 0.0) for n in names}) for c in comms]
    for label, vals in rows:
        cores = {n: vals[n] / data[n][0] for n in names}
        off = [cores[n] for n in names if n.endswith('_off')]
        on = [cores[n] for n in names if n.endswith('_on')]
        mo, mn = sum(off) / len(off), sum(on) / len(on)
        if max(mo, mn) < 0.002:
            continue
        pct = f'{(mn - mo) / mo * 100:+.1f} %' if mo else 'n/a'
        print(f'| {label} | ' + ' | '.join(f'{cores[n]:.3f}' for n in names)
              + f' | {mo:.3f} | {mn:.3f} | {pct} |')
    print()
    print('wall s: ' + ', '.join(f'{n}={data[n][0]}' for n in names))


if __name__ == '__main__':
    main()
