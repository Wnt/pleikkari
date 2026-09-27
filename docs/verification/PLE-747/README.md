# PLE-747: the Go input-lag A/B, one command (runner from PLE-815)

`ab-arms.sh` runs PLE-746's `go-probe.sh rounds` over PLE-747's arm matrix, one `--resource go`
lease per batch, and writes `<run-dir>/summary.md`:

    bash docs/verification/PLE-747/ab-arms.sh plan                  # no device: the batch plan
    APK=<gate arm64 debug apk> BACKUP=<backup dir> \
      bash docs/verification/PLE-747/ab-arms.sh run build/ple747/ab-<stamp>
    bash docs/verification/PLE-747/ab-arms.sh summarize build/ple747/ab-<stamp>

* Before each batch, `go.sh ready` and `go-keepawake.sh alert-text` (empty = no dialog) must
  pass. The first failure stops the run, and `finish` and `summarize` still run.
* Arms are set with go-probe.sh's `ARM_PREFS`/`ARM_PROPS` (PLE-815) on top of the snapshot. Every
  switch the matrix touches is in `BASELINE_DROP`, so control is all defaults. The late-start budget
  arms use the debug build's `debug.pleikkari.vr_pacing=late,budget=<us>`.
* `input_thread` (PLE-802) is not in the default `ARMS`: add it once PLE-802 is in the APK.
  `STIMULUS` passes through to go-probe.sh and needs PLE-803's go-probe.sh.
* Cooldowns: `COOLDOWN_S` (300) between batches, and `LONG_COOLDOWN_S` (1200) after every
  `HOT_AFTER_S` (2400) s of streaming.
* The summary: `ab_summary.py` merges each arm's `input_to_photon.py analyze` output. Its host test
  is `python3 docs/verification/PLE-747/test_ab_summary.py` (PLE-746's baseline CSVs).

No device session has run this yet: the one-arm smoke is PLE-747's first step.
