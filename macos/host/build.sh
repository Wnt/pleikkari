#!/usr/bin/env bash
# Build the native macOS session host (library, plk-host-probe, plk-host-tests) against
# Homebrew packages and run the unit tests. No Qt, SDL or FFmpeg is configured.
#
# Usage: macos/host/build.sh [<out-dir>]      (default: build-macos-host)
#
# Needs: brew install cmake ninja opus json-c miniupnpc openssl@3
# Environment: JOBS (default 4), NICE (default 15), PYTHON (a python with protobuf for nanopb;
# default: a venv created in <out-dir>/venv).
set -euo pipefail

SRC=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)
mkdir -p "${1:-build-macos-host}"
OUT=$(cd "${1:-build-macos-host}" && pwd -P)
BREW=$(brew --prefix)
OPENSSL=$(brew --prefix openssl@3)
JOBS=${JOBS:-4}
NICE=${NICE:-15}

PYTHON=${PYTHON:-}
if [ -z "$PYTHON" ]; then
	if [ ! -x "$OUT/venv/bin/python" ]; then
		python3 -m venv "$OUT/venv"
		"$OUT/venv/bin/pip" install --quiet protobuf grpcio-tools setuptools
	fi
	PYTHON="$OUT/venv/bin/python"
fi

# $BREW/include/openssl can point at OpenSSL 4; the lib needs the OpenSSL 3 headers that
# match the library it links, ahead of every other -I.
mkdir -p "$OUT/openssl3-shim"
ln -sfn "$OPENSSL/include/openssl" "$OUT/openssl3-shim/openssl"
SHIM="-I$OUT/openssl3-shim"

cmake -S "$SRC" -B "$OUT/build" -G Ninja \
	-DCMAKE_BUILD_TYPE=Release \
	-DCHIAKI_ENABLE_GUI=OFF \
	-DCHIAKI_ENABLE_CLI=OFF \
	-DCHIAKI_ENABLE_TESTS=OFF \
	-DCHIAKI_ENABLE_STEAMDECK_NATIVE=OFF \
	-DCHIAKI_ENABLE_STEAM_SHORTCUT=OFF \
	-DCHIAKI_ENABLE_FFMPEG_DECODER=OFF \
	-DCHIAKI_ENABLE_MACOS_NATIVE=ON \
	-DPYTHON_EXECUTABLE="$PYTHON" \
	-DCMAKE_PREFIX_PATH="$BREW;$OPENSSL" \
	-DOPENSSL_ROOT_DIR="$OPENSSL" \
	-DCMAKE_C_FLAGS="$SHIM" -DCMAKE_CXX_FLAGS="$SHIM" \
	-DCMAKE_OBJC_FLAGS="$SHIM" -DCMAKE_OBJCXX_FLAGS="$SHIM" \
	-DOpus_INCLUDE_DIRS="$(brew --prefix opus)/include"

nice -n "$NICE" cmake --build "$OUT/build" -j "$JOBS" --target plk-host-probe plk-host-tests
(cd "$OUT/build" && ctest --output-on-failure -R plk-host)
echo "built: $OUT/build/macos/host/plk-host-probe"
