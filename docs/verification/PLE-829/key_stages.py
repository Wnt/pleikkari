#!/usr/bin/env python3
"""PLE-829: each key's path through the app, in ns, from a Perfetto trace with the `input` and `view`
atrace categories (s22-probe.sh's trace.cfg).

A key is matched across processes by its input channel sequence number: system_server's
InputDispatcher `sendMessage(inputChannel=<the app's window>, seq=S, type=KEY)` and the app's
`receiveMessage(..., seq=S, type=KEY)`. Then, on the thread that read it:

  dispatcher -> app read   the key waiting for the app's looper (main, or PLE-829's PadInput)
  IME round trip           the app's ViewRootImpl handing the key to the keyboard's process and
                           getting it back (ImeInputStage): the `ClientState` channel's send and
                           receive between the read and the stream's dispatch
  app read -> stream       the read to PLE-746's `PLE746 input dispatchKeyEvent` section, the IME
                           round trip included
  dispatcher -> stream     the whole

  key_stages.py <trace>... [--package fi.madekivi.pleikkari]
"""
import argparse
import math
import re
import statistics

from perfetto.trace_processor import TraceProcessor

CHANNEL_SEQ = re.compile(r"inputChannel=(.*), seq=(0x[0-9a-f]+)")


def pct(values, p):
    ordered = sorted(values)
    return ordered[max(1, math.ceil(len(ordered) * p / 100)) - 1]


def app_slices(tp, package):
    return list(tp.query("""
        select s.ts, s.name, t.name as thread, t.tid, p.pid
        from slice s join thread_track tt on s.track_id = tt.id join thread t using(utid) join process p using(upid)
        where p.name like '{pkg}%' and (s.name like 'receiveMessage(inputChannel=%' or s.name like 'sendMessage(inputChannel=%'
            or s.name = 'PLE746 input dispatchKeyEvent')
        order by s.ts""".format(pkg=package)))


def dispatcher_sends(tp):
    """(channel, seq) -> ts of every key InputDispatcher sent (PLE-829's window's channel is "<id> PadInput")."""
    sends = {}
    for r in tp.query("""
            select s.ts, s.name from slice s join thread_track tt on s.track_id = tt.id join thread t using(utid)
            join process p using(upid)
            where p.name = 'system_server' and s.name like 'sendMessage(inputChannel=%type=KEY)'"""):
        found = CHANNEL_SEQ.search(r.name)
        if found:
            sends[found.groups()] = r.ts
    return sends


def keys(path, package):
    tp = TraceProcessor(trace=path)
    sends = dispatcher_sends(tp)
    slices = app_slices(tp, package)
    out = []
    for i, s in enumerate(slices):
        if not s.name.startswith("receiveMessage(") or "ClientState" in s.name or "type=KEY" not in s.name:
            continue
        found = CHANNEL_SEQ.search(s.name)
        if not found or found.groups() not in sends:
            continue
        sent = sends[found.groups()]
        stream = ime_out = ime_back = None
        for later in slices[i + 1:]:
            if later.tid != s.tid:
                continue
            if later.name == "PLE746 input dispatchKeyEvent":
                stream = later.ts
                break
            if later.name.startswith("sendMessage(inputChannel=ClientState") and ime_out is None:
                ime_out = later.ts
            elif later.name.startswith("receiveMessage(inputChannel=ClientState") and ime_back is None:
                ime_back = later.ts
            elif later.name.startswith("receiveMessage(") and "ClientState" not in later.name:
                break  # the next key arrived first: this one never reached the stream
        if stream is None:
            continue
        out.append({
            "thread": s.thread if s.tid != s.pid else "main",
            "dispatcher_to_read": (s.ts - sent) / 1e6,
            "ime": (ime_back - ime_out) / 1e6 if ime_out and ime_back else None,
            "read_to_stream": (stream - s.ts) / 1e6,
            "dispatcher_to_stream": (stream - sent) / 1e6,
        })
    tp.close()
    return out


def line(label, values):
    if not values:
        return "  %-22s -" % label
    return "  %-22s n %3d  p50 %6.3f  p95 %6.3f  max %6.3f  mean %6.3f ms" % (
        label, len(values), pct(values, 50), pct(values, 95), max(values), statistics.fmean(values))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("traces", nargs="+")
    ap.add_argument("--package", default="fi.madekivi.pleikkari")
    args = ap.parse_args()
    for path in args.traces:
        ks = keys(path, args.package)
        threads = sorted({k["thread"] for k in ks})
        print("%s: %d keys, read on %s" % (path, len(ks), ", ".join(threads) or "-"))
        print(line("dispatcher -> app read", [k["dispatcher_to_read"] for k in ks]))
        ime = [k["ime"] for k in ks if k["ime"] is not None]
        print(line("IME round trip", ime) + ("" if ime else "  (none: the keys never went to the keyboard)"))
        print(line("app read -> stream", [k["read_to_stream"] for k in ks]))
        print(line("dispatcher -> stream", [k["dispatcher_to_stream"] for k in ks]))


if __name__ == "__main__":
    main()
