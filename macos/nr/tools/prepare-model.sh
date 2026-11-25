#!/usr/bin/env bash
# Turn an NVIDIA nvngx_dlssnr.dll into the .dlssmodel packages the engine loads.
#
# usage: macos/nr/tools/prepare-model.sh <nvngx_dlssnr.dll> [<out-dir>]
#   out-dir  default: ~/Library/Application Support/Pleikkari/models, where the app looks
#            (elsewhere: start the app with --nr-models <out-dir>)
# env:
#   SCRATCH  the scratch directory macos/nr/build.sh used (default <workspace>/build/native-app/nr);
#            MLX-DLSS's weight tool is taken from its checkout there
#   PYTHON   python 3.10 or later that creates the tool's virtual environment (default python3)
#   VENV     that environment (default $SCRATCH/weights-venv)
#
# Supported builds are the ones MLX-DLSS supports: FileVersion 310.9.16.0 gives both
# networks, NeuralRendering-Small.dlssmodel (31 blocks, about twice as fast, NVIDIA's
# default) and NeuralRendering.dlssmodel (HT, 71 blocks); 310.8.0.0 gives HT only.
#
# The DLL and everything made from it are NVIDIA's: they stay in the output directory on
# this machine and never go into the repository or an app bundle. The conversion's
# intermediate files (.safetensors) are left in <scratch>/weights-work, not beside the models.
set -euo pipefail
[ $# -ge 1 ] || { sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 64; }
DLL=$(cd "$(dirname "$1")" && pwd -P)/$(basename "$1")
[ -f "$DLL" ] || { echo "no such file: $1" >&2; exit 66; }

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
SRC=$(cd "$HERE/../../.." && pwd -P)
case "$SRC" in
	*/wt/*) WORKSPACE=${SRC%%/wt/*} ;;
	*) WORKSPACE=$(dirname "$SRC") ;;
esac
SCRATCH=${SCRATCH:-$WORKSPACE/build/native-app/nr}
OUT=${2:-$HOME/Library/Application Support/Pleikkari/models}
PYTHON=${PYTHON:-python3}
VENV=${VENV:-$SCRATCH/weights-venv}
MLXDLSS=$SCRATCH/checkouts/MLX-DLSS
[ -f "$MLXDLSS/python/pyproject.toml" ] || {
	echo "no MLX-DLSS checkout at $MLXDLSS: run macos/nr/build.sh first (or set SCRATCH)" >&2
	exit 69
}

# MLX-DLSS's documented way: pip install ./python, then mlxdlss-weights. The package is
# installed from a copy so that pip's in-tree build leaves SwiftPM's checkout untouched.
if ! "$VENV/bin/python" -I -c 'import mlxdlss.tools.cli' > /dev/null 2>&1; then
	[ -x "$VENV/bin/python" ] || "$PYTHON" -m venv "$VENV"
	COPY=$(mktemp -d)
	trap 'rm -rf "$COPY"' EXIT
	cp -R "$MLXDLSS/python" "$COPY/mlxdlss-python"
	"$VENV/bin/python" -m pip install --disable-pip-version-check -q "$COPY/mlxdlss-python"
fi

WORK=$SCRATCH/weights-work
rm -rf "$WORK"
mkdir -p "$WORK" "$OUT"
cd "$WORK"
# -I: the DLL is data from elsewhere; nothing next to it or in the environment is imported.
"$VENV/bin/python" -I -m mlxdlss.tools.cli sha256 "$DLL"
"$VENV/bin/python" -I -m mlxdlss.tools.cli all "$DLL" "$WORK"
# Only the packages the engine loads go to the output directory.
FOUND=0
for MODEL in "$WORK"/*.dlssmodel; do
	[ -d "$MODEL" ] || continue
	rm -rf "$OUT/$(basename "$MODEL")"
	cp -R "$MODEL" "$OUT/"
	FOUND=1
done
[ "$FOUND" = 1 ] || { echo "the conversion produced no .dlssmodel package in $WORK" >&2; exit 70; }
echo
echo "models:"
find "$OUT" -maxdepth 1 -name '*.dlssmodel' -print | sort | sed 's/^/  /'
