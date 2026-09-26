#!/usr/bin/env bash
# PLE-603: build the VR environment renderer for the host and render every environment
# to PNG through Mesa. Needs libegl1-mesa-dev, libgles-dev, zlib1g-dev, cmake, ninja.
#
#   android/vr-environment/render-host.sh [--check] [--size N] [--env NAME]
#
# Output goes to <worktree>/build/vr-environment/ (gitignored).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
build="$root/build/vr-environment-host"
out="$root/build/vr-environment"
mkdir -p "$build" "$out"
cmake -S "$here/host" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
ninja -C "$build" >/dev/null
# llvmpipe on a headless box; the surfaceless platform needs no display server.
export LIBGL_ALWAYS_SOFTWARE=1
exec "$build/vr-environment-render" --out "$out" "$@"
