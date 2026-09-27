"""PLE-800: summarise the temporary 'PLE800 history' logcat lines of one or more S22 sessions."""
import re
import sys

LINE = re.compile(r'PLE800 history seq=(\d+) queued_to_sent_us=(\d+) queued_after_state_us=(\d+) state_sent_with_it=(\d)')
STATE_MIN_US = 8000


def load(path):
    rows = []
    for line in open(path, errors='replace'):
        m = LINE.search(line)
        if m:
            rows.append(tuple(int(x) for x in m.groups()))
    return rows


def pct(values, p):
    values = sorted(values)
    return values[min(len(values) - 1, int(round(p / 100 * (len(values) - 1))))]


for path in sys.argv[1:]:
    rows = load(path)
    lat = [r[1] for r in rows]
    print(f'== {path}: {len(rows)} history packets')
    print(f'  queued->sent us: p50 {pct(lat, 50)} p95 {pct(lat, 95)} max {max(lat)};'
          f' >=1000 us: {sum(1 for x in lat if x >= 1000)}; >=5000 us: {sum(1 for x in lat if x >= 5000)}')
    gaps = [r[2] for r in rows]
    print(f'  queued after the last state packet us: p50 {pct(gaps, 50)} p95 {pct(gaps, 95)} max {max(gaps)}')
    inside = [r for r in rows if r[2] < STATE_MIN_US]
    if inside:
        il = [r[1] for r in inside]
        print(f'  queued inside the 8 ms state minimum: {len(inside)}; their queued->sent us:'
              f' p50 {pct(il, 50)} max {max(il)}; >=1000 us: {sum(1 for x in il if x >= 1000)}')
    else:
        print('  queued inside the 8 ms state minimum: 0')
    print(f'  sent in the same pass as a state packet: {sum(r[3] for r in rows)}')
    seqs = [r[0] for r in rows]
    contiguous = all(b == (a + 1) % 0x10000 for a, b in zip(seqs, seqs[1:]))
    print(f'  history seq contiguous: {contiguous} ({seqs[0]}..{seqs[-1]})')
    slow = [r for r in rows if r[1] >= 1000]
    for r in slow[:12]:
        print(f'    slow: seq {r[0]} queued->sent {r[1]} us, queued {r[2]} us after a state packet, state in same pass {r[3]}')
