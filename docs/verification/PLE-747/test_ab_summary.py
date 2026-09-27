#!/usr/bin/env python3
"""PLE-815: ab_summary.py on PLE-746's baseline CSVs (python3 test_ab_summary.py)."""
import os
import shutil
import tempfile
import unittest

import ab_summary

BASELINE = "/home/wnt/gta6/docs/latency/go-input-to-photon/baseline-2026-09-27"
LOGCAT = """\
09-27 13:01:00.000  4100  4120 I GoCinema: Cinema video: 70 decoder frames latched
09-27 13:01:01.000  4100  4130 I VrApi   : FPS=72,Prd=45ms,Tear=0,Early=72,Stale=2,VSnc=1,Lat=0,Fov=0,CPU2/GPU=1/1,1056/214MHz
09-27 13:01:02.000  4100  4130 I VrApi   : FPS=72,Prd=47ms,Tear=0,Early=0,Stale=0,VSnc=1,Lat=0,Fov=0,CPU2/GPU=1/1,1056/214MHz
09-27 13:01:02.000   900   930 I VrApi   : FPS=60,Prd=57ms,Tear=0,Early=60,Stale=9,VSnc=1,Lat=0,Fov=0,CPU2/GPU=1/1,1056/214MHz
"""


class AbSummaryTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        for arm in ("control", "late72"):
            os.makedirs(os.path.join(self.dir, arm))
            shutil.copy(os.path.join(BASELINE, "presses.csv"), os.path.join(self.dir, arm))
        with open(os.path.join(self.dir, "late72", "logcat.txt"), "w") as f:
            f.write(LOGCAT)

    def tearDown(self):
        shutil.rmtree(self.dir)

    def test_baseline_presses(self):
        p = ab_summary.press_stats(os.path.join(BASELINE, "presses.csv"))
        self.assertGreater(p["ok"], 0)
        self.assertLessEqual(p["ok"], p["presses"])
        self.assertLessEqual(p["p50"], p["p95"])
        self.assertTrue(all(v is not None for v in p["stages"].values()))

    def test_table(self):
        text = ab_summary.summary(self.dir)
        rows = [l for l in text.splitlines() if l.startswith("| control") or l.startswith("| late72")]
        self.assertEqual(len(rows), 2)
        self.assertIn("| 0 | n/a | n/a | n/a |", rows[0])
        # Only the cinema's pid: vrshell's 60 Hz line is not counted.
        self.assertTrue(rows[1].endswith("| 2 | 45 | 1.00 | 0.50 |"), rows[1])


if __name__ == "__main__":
    unittest.main()
