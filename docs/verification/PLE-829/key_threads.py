#!/usr/bin/env python3
"""PLE-829: which thread of the app handled each key event, from a Perfetto trace.

Counts the app's `deliverInputEvent` slices (ViewRootImpl, atrace `view`) and PLE-746's
`PLE746 input dispatchKeyEvent` sections by thread name. With stream_pad_input_thread on, the
stream's keys are delivered on `PadInput`; with it off, on the main thread (named after the
package). Needs the `perfetto` Python package (the ab harness venv has it).

  key_threads.py <trace> [--package fi.madekivi.pleikkari]
"""
import argparse
import sys

from perfetto.trace_processor import TraceProcessor


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--package", default="fi.madekivi.pleikkari")
    args = ap.parse_args()
    tp = TraceProcessor(trace=args.trace)
    rows = tp.query("""
        select
          case when s.name like 'deliverInputEvent%' then 'deliverInputEvent'
               else s.name end as slice,
          substr(s.name, 1, 60) as example,
          t.name as thread, t.tid = p.pid as main, count(*) as n
        from slice s
        join thread_track tt on s.track_id = tt.id
        join thread t using(utid)
        join process p using(upid)
        where p.name like '{pkg}%'
          and (s.name like 'deliverInputEvent%' or s.name = 'PLE746 input dispatchKeyEvent')
        group by slice, thread, main
        order by slice, n desc
    """.format(pkg=args.package))
    found = False
    for r in rows:
        found = True
        print("%-32s %-18s main=%-5s %4d  e.g. %s" % (r.slice, r.thread, bool(r.main), r.n, r.example))
    if not found:
        print("no deliverInputEvent or PLE746 slices for %s" % args.package)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
