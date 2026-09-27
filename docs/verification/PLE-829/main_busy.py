#!/usr/bin/env python3
"""PLE-829: what the app's main thread ran just before each key reached it (arm off).

For every `deliverInputEvent` slice on the app's main thread, it takes the main thread's top-level
slices that overlap the 10 ms before it, and sums them by name. That is the work a key queued behind
while it waited for the main looper. Needs the `perfetto` Python package.

  main_busy.py <trace>... [--package fi.madekivi.pleikkari] [--window-ms 10]
"""
import argparse
import collections
import re

from perfetto.trace_processor import TraceProcessor


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("traces", nargs="+")
    ap.add_argument("--package", default="fi.madekivi.pleikkari")
    ap.add_argument("--window-ms", type=float, default=10.0)
    args = ap.parse_args()
    window = int(args.window_ms * 1e6)
    busy = collections.Counter()
    count = collections.Counter()
    keys = 0
    for path in args.traces:
        tp = TraceProcessor(trace=path)
        main_utid = """(select utid from thread join process using(upid)
            where process.name like '{pkg}%' and thread.tid = process.pid)""".format(pkg=args.package)
        keys_rows = list(tp.query("""select s.ts from slice s join thread_track tt on s.track_id = tt.id
            where tt.utid in {main} and s.name like 'deliverInputEvent%'""".format(main=main_utid)))
        keys += len(keys_rows)
        for k in keys_rows:
            rows = tp.query("""select s.name, s.ts, s.dur from slice s join thread_track tt on s.track_id = tt.id
                where tt.utid in {main} and s.depth = 0 and s.ts < {end} and s.ts + s.dur > {start}
                and s.name not like 'deliverInputEvent%'""".format(main=main_utid, start=k.ts - window, end=k.ts))
            for r in rows:
                name = re.sub(r"\d+", "N", r.name)[:70]
                overlap = min(r.ts + r.dur, k.ts) - max(r.ts, k.ts - window)
                busy[name] += overlap
                count[name] += 1
        tp.close()
    print("%d keys on the main thread; top-level main-thread work in the %.0f ms before each:" % (keys, args.window_ms))
    for name, ns in busy.most_common(12):
        print("  %8.2f ms total  %4d slices  %s" % (ns / 1e6, count[name], name))


if __name__ == "__main__":
    main()
