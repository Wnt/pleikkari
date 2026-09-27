# PLE-199: per-thread CPU for the `stream_thread_priority_boost` A/B

**Short answer: the Takion threads do not save 26-37 % of CPU.** Together, the two
`Chiaki Takion` threads use about 1.5 % of one core with the boost on or off: 0.015 cores
OFF and 0.014 cores ON, a −3.5 % change. PLE-59's figure does not replicate on the current
build.

The app does get cheaper, by 8.8 % (0.623 → 0.569 cores), which matches PLE-198's −9.9 %.
Almost all of that saving comes from two audio-side threads that **the code does not
intend to pin**. They inherit the CPU 4-7 affinity from their creating thread (PLE-198
§3):

- `AAudio_1`: 0.051 → 0.026 cores (−50 %)
- `Thread-8`: 0.059 → 0.034 cores (−42 %)

Together they account for 0.050 of the 0.054-core drop. The threads the code names
change by only a few percent: Takion −3.5 %, `ChiakiVideoOut` −3.6 %, and `NDK
MediaCodec_` −2 %. The UI, `RenderThread` and binder threads are unchanged.

## Method

- Phone: SM-S908B, serial `192.168.1.105:5555` (Wi-Fi ADB), **on battery** (74 %, not AC).
- Console: PS5-466, where `ps5-discover.py` reported `ready`.
- APK: `assembleDebug` of this branch's base `7c8df0c4` (gate PASS). It was installed
  with `install -r` after `app-state.sh backup`, and the backup was restored afterwards
  (`restore verified`).
- Scripts: `setup.sh` and `session.sh` are PLE-198's, with the same pinned shipped-default
  prefs; the arms differ only in the boost. They drive `scripts/dev/ab/soak.sh`, frozen
  into the captures dir. `session.sh` adds two things:
  - a per-thread read of `/proc/<pid>/task/*/stat` utime+stime through `run-as`, taken at
    drive start and drive end (`<s>_threads.txt`);
  - a wake plus swipe-unlock, because the phone on battery sits in AOD.
- Sessions ran in the order **ON, OFF, OFF, ON** (`r1`..`r4`) on 2026-09-27 between
  16:26 and 16:55 UTC. Each was one Samsung lease with 5 × 50 s of synthetic stick input,
  giving a measured window of about 266 s.
- No thermal cooldown between sessions: this round measures CPU-seconds, not heat.
- `aggregate.py` sums threads by name, folds all binder threads into one row, and divides
  by wall time to give cores.
- Captures (gitignored): `/home/wnt/gta6/build/dispatch/ple-199/captures/`.
- Affinity is confirmed in `<s>_affinity.txt`: 8 threads are on CPUs 4-7 in both ON
  sessions and none in either OFF session. Every session streamed at a clean 60 fps with
  0 lost.

## Results (cores = CPU-s / wall-s)

| thread | r1_on | r2_off | r3_off | r4_on | off mean | on mean | on-off % |
|---|---|---|---|---|---|---|---|
| all threads | 0.568 | 0.631 | 0.616 | 0.569 | 0.623 | 0.569 | -8.8 % |
| ekivi.pleikkari | 0.171 | 0.175 | 0.170 | 0.174 | 0.173 | 0.173 | -0.0 % |
| NDK MediaCodec_ | 0.114 | 0.117 | 0.113 | 0.112 | 0.115 | 0.113 | -2.0 % |
| RenderThread | 0.091 | 0.093 | 0.092 | 0.094 | 0.093 | 0.093 | -0.1 % |
| binder (all) | 0.062 | 0.063 | 0.061 | 0.062 | 0.062 | 0.062 | -0.1 % |
| Thread-8 | 0.035 | 0.059 | 0.059 | 0.034 | 0.059 | 0.034 | -42.0 % |
| AAudio_1 | 0.026 | 0.051 | 0.050 | 0.025 | 0.051 | 0.026 | -49.5 % |
| Chiaki Session | 0.023 | 0.024 | 0.023 | 0.022 | 0.024 | 0.023 | -4.0 % |
| Chiaki Takion | 0.014 | 0.015 | 0.014 | 0.014 | 0.015 | 0.014 | -3.5 % |
| ChiakiVideoIn | 0.010 | 0.011 | 0.010 | 0.010 | 0.010 | 0.010 | -6.7 % |
| CodecLooper | 0.008 | 0.008 | 0.008 | 0.008 | 0.008 | 0.008 | -1.7 % |
| ChiakiVideoOut | 0.007 | 0.008 | 0.008 | 0.007 | 0.008 | 0.007 | -3.6 % |
| Chiaki GKCrypt | 0.004 | 0.004 | 0.004 | 0.004 | 0.004 | 0.004 | +10.0 % |

wall s: r1_on=267, r2_off=267, r3_off=265, r4_on=267

The rows are stable within each arm: the two sessions of the same arm agree to ±0.002
cores on every thread that matters.

## Reading it

- PLE-59's "26-37 % Takion CPU" cannot be a thread-CPU saving of that size. The Takion
  threads use about 1.5 % of one core, and the boost moves that by −3.5 %.
- The real saving is audio-side. `AAudio_1` is the AAudio data callback thread, and
  `Thread-8` is most likely the audio decode/output thread. Its creator was not traced.
  Both land on the big cores only through affinity inheritance. On a big core they need
  about half the CPU-seconds.
- So the flag's benefit comes from a side effect that nobody designed. If the pinning is
  "fixed" so that only the named threads are pinned (PLE-198's follow-up), most of the
  CPU saving will disappear.
- Not measured: latency, frame pacing and heat.
