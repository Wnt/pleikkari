#!/usr/bin/env bash
# Build the neural-rendering engine: libplknr.dylib and plk-nr-bench, with MLX's Metal
# library next to them (MLX looks for mlx.metallib beside the binary that contains it).
#
# usage: macos/nr/build.sh [<scratch-dir>]
#   scratch-dir  default: <workspace>/build/native-app/nr, where <workspace> is the
#                directory that holds wt/ (or the repository's parent for a plain checkout)
#
# What others use ends up in <scratch-dir>/dist, whatever SwiftPM calls its own output:
#   libplknr.dylib          the engine; macos/app/build.sh copies it into Pleikkari.app
#   mlx-swift_Cmlx.bundle   MLX's Metal library as the app bundle carries it
#   mlx.metallib            the same file, for a library or tool used from this directory
#   plk-nr-bench            the engine without the app
# env:
#   JOBS    parallel jobs (default 3)
#   NICE    nice level (default 15)
#   CONFIG  debug or release (default release)
#
#
# Needs Xcode 26 or later (Swift 6.2) with its Metal Toolchain component, and network
# access the first time: SwiftPM fetches MLX-DLSS (pinned by revision in Package.swift)
# and mlx-swift. MLX keeps its precompiled Metal kernels in a .metallib that the build
# compiles, so the offline Metal compiler is required here (the video pipeline itself
# compiles its shaders at run time and does not need it). Install it once with
#     xcodebuild -downloadComponent MetalToolchain
# The app does not need this build: without the library it runs as before, with no
# neural rendering.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
SRC=$(cd "$HERE/../.." && pwd -P)
case "$SRC" in
	*/wt/*) WORKSPACE=${SRC%%/wt/*} ;;
	*) WORKSPACE=$(dirname "$SRC") ;;
esac
SCRATCH=${1:-$WORKSPACE/build/native-app/nr}
JOBS=${JOBS:-3}
NICE=${NICE:-15}
CONFIG=${CONFIG:-release}


# The offline Metal compiler: its stub exists either way, only running it tells. Stop here
# with the command that installs it, instead of failing deep inside the MLX build.
PROBE=$(mktemp -d)
trap 'rm -rf "$PROBE"' EXIT
printf 'kernel void k(uint i [[thread_position_in_grid]]) {}\n' > "$PROBE/k.metal"
if ! xcrun -sdk macosx metal -c "$PROBE/k.metal" -o "$PROBE/k.air" > /dev/null 2>&1; then
	echo "Xcode's Metal Toolchain is not installed; MLX's Metal kernels cannot be compiled." >&2
	echo "Install it once:  xcodebuild -downloadComponent MetalToolchain" >&2
	exit 69
fi

swiftpm() { swift build --package-path "$HERE" --scratch-path "$SCRATCH" -c "$CONFIG" "$@"; }
nice -n "$NICE" swift build --package-path "$HERE" --scratch-path "$SCRATCH" -c "$CONFIG" --jobs "$JOBS"
BIN=$(swiftpm --show-bin-path)

# Swift Build compiles MLX's Metal sources into the package's resource bundle.
METALLIB=$(find "$BIN" -path '*mlx-swift_Cmlx.bundle*' -name default.metallib -size +1k 2> /dev/null | head -1)
[ -n "$METALLIB" ] || { echo "the build left no default.metallib in mlx-swift_Cmlx.bundle under $BIN" >&2; exit 70; }
cp "$METALLIB" "$BIN/mlx.metallib"

LIB=$(find "$BIN" -maxdepth 3 -name 'libplknr.dylib' | head -1)
[ -n "$LIB" ] || { echo "libplknr.dylib was not produced in $BIN" >&2; exit 70; }
# MLX looks beside the image that holds it; keep the pair together wherever the dylib is.
[ "$(dirname "$LIB")" = "$BIN" ] || cp "$METALLIB" "$(dirname "$LIB")/mlx.metallib"

BUNDLE=$(find "$BIN" -maxdepth 3 -type d -name 'mlx-swift_Cmlx.bundle' | head -1)
DIST=$SCRATCH/dist
rm -rf "$DIST"
mkdir -p "$DIST"
cp "$LIB" "$BIN/plk-nr-bench" "$DIST/"
cp "$METALLIB" "$DIST/mlx.metallib"
cp -R "$BUNDLE" "$DIST/mlx-swift_Cmlx.bundle"

echo "dist:    $DIST   (macos/app/build.sh puts the engine into the app from here)"
echo "library: $DIST/libplknr.dylib"
echo "bench:   $DIST/plk-nr-bench"
echo "mlx:     $SCRATCH/checkouts/MLX-DLSS   (source of tools/prepare-model.sh and make-synthetic-model.py)"
