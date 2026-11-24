#!/usr/bin/env bash
# Development build of the Qt GUI on macOS against Homebrew packages.
#
# Usage: scripts/build-macos-homebrew.sh [<out-dir>]   (default: build-macos)
#
# Needs: brew install cmake ninja qtbase qtdeclarative qtsvg qtwebengine libplacebo \
#        molten-vk vulkan-loader vulkan-headers sdl2-compat ffmpeg opus json-c \
#        miniupnpc speexdsp hidapi openssl@3
#
# The result is <out-dir>/build/gui/chiaki.app. It is not a relocatable bundle:
# it links the Homebrew libraries where they are.
set -euo pipefail

SRC=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
mkdir -p "${1:-build-macos}"
OUT=$(cd "${1:-build-macos}" && pwd -P)
BREW=$(brew --prefix)
OPENSSL=$(brew --prefix openssl@3)

# nanopb's generator needs protobuf for the python that cmake runs.
if [ ! -x "$OUT/venv/bin/python" ]; then
	python3 -m venv "$OUT/venv"
	"$OUT/venv/bin/pip" install --quiet protobuf grpcio-tools setuptools
fi

# $BREW/include/openssl points at whichever OpenSSL Homebrew linked last, which can be
# OpenSSL 4. The bundled curl and the lib need the OpenSSL 3 headers that match the
# library they link, so put a directory holding only those ahead of every other -I.
mkdir -p "$OUT/openssl3-shim"
ln -sfn "$OPENSSL/include/openssl" "$OUT/openssl3-shim/openssl"
SHIM="-I$OUT/openssl3-shim"

cmake -S "$SRC" -B "$OUT/build" -G Ninja \
	-DCMAKE_BUILD_TYPE=Release \
	-DCHIAKI_ENABLE_CLI=OFF \
	-DCHIAKI_ENABLE_STEAMDECK_NATIVE=OFF \
	-DCHIAKI_ENABLE_STEAM_SHORTCUT=ON \
	-DPYTHON_EXECUTABLE="$OUT/venv/bin/python" \
	-DCMAKE_PREFIX_PATH="$BREW;$OPENSSL" \
	-DOPENSSL_ROOT_DIR="$OPENSSL" \
	-DCMAKE_C_FLAGS="$SHIM" -DCMAKE_CXX_FLAGS="$SHIM" \
	-DCMAKE_OBJC_FLAGS="$SHIM" -DCMAKE_OBJCXX_FLAGS="$SHIM" \
	-DOpus_INCLUDE_DIRS="$(brew --prefix opus)/include" \
	-DSPEEXDSP_INCLUDE_DIR="$(brew --prefix speexdsp)/include"

cmake --build "$OUT/build" --target chiaki chiaki-unit
"$OUT/build/test/chiaki-unit"
echo "built: $OUT/build/gui/chiaki.app"
