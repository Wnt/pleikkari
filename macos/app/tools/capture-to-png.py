#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
"""Pictures of what `Pleikkari replay --headless --capture DIR` wrote.

usage: capture-to-png.py DIR [--pq] [--out OUTDIR]

Reads DIR/frames.csv and writes one PNG beside each NNNN.raw (or into OUTDIR).

  bgra8     an SDR target: written as it is.
  bgr10a2   ten bits a channel. With --pq the values are taken as an HDR10 picture (PQ,
            BT.2020) and written as the SDR rendition the neural rendering is given of it
            (macos/video/README.md): SDR white at 203 nits, the highlights on a shoulder
            above it, BT.709, 2.2 gamma. Without --pq the code values are written as they
            are, scaled to eight bits.
  rgba16f   extended linear light, 1.0 SDR white: written with 2.2 gamma, clipped at white.

Needs numpy and Pillow.
"""
import csv
import os
import sys

import numpy as np
from PIL import Image

M1, M2 = 2610.0 / 16384.0, 2523.0 / 4096.0 * 128.0
C1, C2, C3 = 3424.0 / 4096.0, 2413.0 / 4096.0 * 32.0, 2392.0 / 4096.0 * 32.0
SDR_WHITE_NITS, KNEE, SHOULDER = 203.0, 0.6, 9.3
BT2020_TO_BT709 = np.array([[1.660491, -0.587641, -0.072850],
                            [-0.124550, 1.132900, -0.008349],
                            [-0.018151, -0.100579, 1.118730]])


def sdr_rendition(pq):
    """PQ code values 0...1 (BT.2020) to the rendition's code values (BT.709, 2.2 gamma)."""
    p = pq ** (1.0 / M2)
    nits = (np.maximum(p - C1, 0.0) / (C2 - C3 * p)) ** (1.0 / M1) * 10000.0
    lin = (nits / SDR_WHITE_NITS) @ BT2020_TO_BT709.T
    t = np.minimum(lin, KNEE) + np.log1p(SHOULDER * np.maximum(lin - KNEE, 0.0)) / SHOULDER
    return np.sign(t) * np.abs(t) ** (1.0 / 2.2)


def load(path, width, height, fmt, pq=False):
    """A captured frame as float R, G, B in 0...1, height x width x 3."""
    if fmt == "bgra8":
        a = np.fromfile(path, dtype=np.uint8).reshape(height, width, 4)
        return a[:, :, [2, 1, 0]].astype(np.float32) / 255.0
    if fmt == "bgr10a2":
        v = np.fromfile(path, dtype="<u4").reshape(height, width)
        rgb = np.stack([(v >> 20) & 1023, (v >> 10) & 1023, v & 1023], axis=2).astype(np.float64) / 1023.0
        return np.clip(sdr_rendition(rgb), 0.0, 1.0).astype(np.float32) if pq else rgb.astype(np.float32)
    if fmt == "rgba16f":
        a = np.fromfile(path, dtype="<f2").reshape(height, width, 4)[:, :, :3].astype(np.float32)
        return np.clip(a, 0.0, 1.0) ** (1.0 / 2.2)
    raise ValueError("unknown pixel format " + fmt)


def frames(directory):
    with open(os.path.join(directory, "frames.csv"), newline="") as f:
        return list(csv.DictReader(f))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) < 1:
        sys.exit(__doc__)
    directory = args[0]
    pq = "--pq" in sys.argv
    out = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else directory
    if "--out" in sys.argv:
        args = [a for a in args if a != out]
        directory = args[0]
    os.makedirs(out, exist_ok=True)
    rows = frames(directory)
    for row in rows:
        rgb = load(os.path.join(directory, row["file"]), int(row["width"]), int(row["height"]), row["format"], pq)
        Image.fromarray((rgb * 255.0 + 0.5).astype(np.uint8)).save(os.path.join(out, row["file"].replace(".raw", ".png")))
    print("%d pictures in %s" % (len(rows), out))


if __name__ == "__main__":
    main()
