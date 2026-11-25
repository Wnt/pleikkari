#!/usr/bin/env python3
"""Random weights with the real network's tensor names and shapes, as a .dlssmodel package.

The package holds no NVIDIA data. The picture it produces is noise; what it is good for is
everything else: the GPU work per frame is the real network's (the kernels and their sizes
depend on the tensor shapes, not on the values), so the engine, the pipeline and the latency
of a stream can be built and measured on a machine that has no extracted model.

    make-synthetic-model.py --mlxdlss <MLX-DLSS checkout> --network small out/Synthetic-Small.dlssmodel

Needs numpy and safetensors (no torch). <MLX-DLSS checkout> is the SwiftPM checkout that
macos/nr/build.sh leaves in <scratch>/checkouts/MLX-DLSS: this script reads its
python/mlxdlss/weight_spec.json (the tensors of the HT network) and topology.py (the block
order of both networks), and writes the package the way its packaging tool does.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import pathlib
import re
import sys

import numpy
from safetensors.numpy import save_file

ARCHITECTURE = "mlxdlss.neural-rendering-transformer.v1"
IDENTIFIER = "fi.madekivi.pleikkari.synthetic.neural-rendering-transformer"
SCALE_LIKE = ("attn_scale", "ffn_cos_skip", "attn_cos_skip", "inp_merge_sin", "inp_merge_cos",
              "inp_upsample_sin", "sin", "blend_scale")
BLOCK = re.compile(r"^block(\d+)\.(.*)$")


def load_topology(checkout: pathlib.Path):
    path = checkout / "python" / "mlxdlss" / "topology.py"
    spec = importlib.util.spec_from_file_location("plk_mlxdlss_topology", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module  # dataclasses looks the module up by name
    spec.loader.exec_module(module)
    return module


def tensors_for(network: str, checkout: pathlib.Path) -> dict[str, dict]:
    topology = load_topology(checkout)
    spec = json.loads((checkout / "python" / "mlxdlss" / "weight_spec.json").read_text())["tensors"]
    by_block: dict[int, dict[str, dict]] = {}
    for name, entry in spec.items():
        match = BLOCK.match(name)
        if not match:
            raise SystemExit(f"unexpected tensor outside a block: {name}")
        by_block.setdefault(int(match.group(1)), {})[match.group(2)] = entry
    source = topology.HT
    if len(by_block) != source.block_count:
        raise SystemExit(f"weight_spec.json has {len(by_block)} blocks, the HT topology {source.block_count}")
    target = topology.KNOWN[network]
    # A block's tensors depend on its family and width only, so every block of the target
    # network takes the shapes of the first HT block of the same kind.
    first: dict[tuple[str, int], int] = {}
    for block in source.blocks:
        first.setdefault((block.family, block.channels), block.index)
    out: dict[str, dict] = {}
    for block in target.blocks:
        donor = first[(block.family, block.channels)]
        for suffix, entry in by_block[donor].items():
            out[f"block{block.index}.{suffix}"] = entry
    recovered = topology.infer_logical({name: entry["shape"] for name, entry in out.items()})
    if recovered.depths != target.depths:
        raise SystemExit(f"built {recovered.spec}, wanted {target.spec}")
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("destination", type=pathlib.Path, help="the .dlssmodel directory to create")
    parser.add_argument("--mlxdlss", type=pathlib.Path, required=True, help="MLX-DLSS source checkout")
    parser.add_argument("--network", choices=("small", "ht"), default="small")
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--scale", type=float, default=0.05, help="standard deviation of the ordinary weights")
    args = parser.parse_args()

    destination: pathlib.Path = args.destination
    if destination.exists():
        raise SystemExit(f"{destination} exists")
    generator = numpy.random.default_rng(args.seed)
    arrays: dict[str, numpy.ndarray] = {}
    specs = []
    for name, entry in sorted(tensors_for(args.network, args.mlxdlss).items()):
        shape = entry["shape"]
        if name.endswith(SCALE_LIKE):
            values = generator.uniform(0.5, 1.0, size=shape)
        elif name.endswith("attn_bias"):
            values = generator.normal(0, 0.1, size=shape)
        else:
            values = generator.normal(0, args.scale, size=shape)
        half = entry["dtype"] == "F16"
        arrays[name] = values.astype(numpy.float16 if half else numpy.float32)
        specs.append({"name": name, "dataType": "float16" if half else "float32", "shape": list(shape)})

    destination.mkdir(parents=True)
    weights = destination / "weights.safetensors"
    save_file(arrays, str(weights), metadata={"format": "dlssnr-logical-v18", "fully_logical": "true",
                                               "synthetic": f"seed {args.seed}, no vendor data"})
    digest = hashlib.sha256(weights.read_bytes()).hexdigest()
    manifest = {
        "schemaVersion": 1,
        "identifier": IDENTIFIER,
        "architecture": ARCHITECTURE,
        "inputs": [{"name": "color", "dataType": "float32", "layout": "nhwc", "shape": [1, "height", "width", 16]}],
        "outputs": [{"name": "color", "dataType": "float32", "layout": "nhwc", "shape": [1, "height", "width", 4]}],
        "state": {"kind": "stateless"},
        "weights": {"file": "weights.safetensors", "sha256": digest, "tensors": specs},
    }
    (destination / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    megabytes = weights.stat().st_size / 1e6
    print(f"{destination}: {args.network}, {len(specs)} tensors, {megabytes:.0f} MB, synthetic (no vendor data)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
