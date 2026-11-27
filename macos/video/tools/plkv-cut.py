#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
"""Cut a PLKVDMP1 recording into short clips that replay by themselves.

usage: plkv-cut.py IN.plkv OUTDIR [--seconds 5] [--fps 60] [--prefix NAME] [--list]

A clip can only start where the decoder can: at a keyframe (an HEVC IRAP picture or an
H.264 IDR). A console sends one at the start of a stream and after that only when it is
asked to, so a recording has few of them and the clips start where they are, not at even
steps. Each clip gets the recording's config sample first, then the frames from its
keyframe on, untouched: nothing is decoded or encoded again. --list only prints the
keyframes.

The format is in macos/video/tools/replay/Dump.h.
"""
import argparse
import os
import struct
import sys

MAGIC = b"PLKVDMP1"
HEADER = struct.Struct("<IHHQi")  # payload_size, frame_index, flags, frame_ready_us, frames_lost


def records(data):
    at = len(MAGIC)
    while at + HEADER.size <= len(data):
        size, index, flags, ready, lost = HEADER.unpack_from(data, at)
        end = at + HEADER.size + size
        if end > len(data):
            break  # a recording that was cut off in the middle of a frame
        yield at, end, flags, ready
        at = end


def nal_types(payload, limit=16):
    """NAL unit header bytes of the first few NAL units of an Annex-B payload."""
    out = []
    at = 0
    while len(out) < limit:
        at = payload.find(b"\x00\x00\x01", at)
        if at < 0 or at + 3 >= len(payload):
            break
        out.append(payload[at + 3])
        at += 3
    return out


def is_keyframe(payload, hevc):
    for b in nal_types(payload):
        if hevc:
            if 16 <= ((b >> 1) & 0x3F) <= 21:  # BLA, IDR, CRA
                return True
        elif (b & 0x1F) == 5:  # IDR slice
            return True
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("input")
    ap.add_argument("outdir", nargs="?")
    ap.add_argument("--seconds", type=float, default=5.0)
    ap.add_argument("--fps", type=float, default=60.0)
    ap.add_argument("--prefix")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()

    with open(args.input, "rb") as f:
        data = f.read()
    if data[: len(MAGIC)] != MAGIC:
        sys.exit(f"{args.input}: not a PLKVDMP1 recording")
    recs = list(records(data))
    config = next((r for r in recs if r[2] & 1), None)
    if not config:
        sys.exit(f"{args.input}: no config sample")
    # HEVC parameter sets are NAL types 32..34; H.264's SPS is type 7.
    first = nal_types(data[config[0] + HEADER.size : config[1]])
    hevc = any(32 <= ((b >> 1) & 0x3F) <= 34 for b in first)
    frames = [r for r in recs if not r[2] & 1]
    if not frames:
        sys.exit(f"{args.input}: no frames")
    t0 = frames[0][3]
    keys = [i for i, r in enumerate(frames) if is_keyframe(data[r[0] + HEADER.size : r[1]], hevc)]
    want = max(1, round(args.seconds * args.fps))
    print(f"{args.input}: {'HEVC' if hevc else 'H.264'}, {len(frames)} frames, {(frames[-1][3] - t0) / 1e6:.1f} s, {len(keys)} keyframes")
    if args.list or not args.outdir:
        for i in keys:
            print(f"  keyframe at frame {i}, {(frames[i][3] - t0) / 1e6:7.1f} s")
        return
    os.makedirs(args.outdir, exist_ok=True)
    prefix = args.prefix or os.path.splitext(os.path.basename(args.input))[0]
    last_end = -1
    n = 0
    for i in keys:
        if i < last_end:  # inside the clip before it
            continue
        end = min(len(frames), i + want)
        if end - i < want // 2:  # too little left of the recording
            continue
        n += 1
        path = os.path.join(args.outdir, f"{prefix}-{n:02d}.plkv")
        with open(path, "wb") as out:
            out.write(MAGIC)
            out.write(data[config[0] : config[1]])
            out.write(data[frames[i][0] : frames[end - 1][1]])
        print(f"  {path}: from {(frames[i][3] - t0) / 1e6:.1f} s, {end - i} frames, {os.path.getsize(path) / 1e6:.1f} MB")
        last_end = end
    if not n:
        sys.exit("no clip written")


if __name__ == "__main__":
    main()
