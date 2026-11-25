#!/usr/bin/env bash
# Configure and build Pleikkari.app (and the module tests), then run the unit tests.
#
# usage: macos/app/build.sh [<build-dir>] [--no-tests]
#   build-dir  default: <workspace>/build/native-app/app, where <workspace> is the
#              directory that holds wt/ (or the repository's parent for a plain checkout)
# env:
#   JOBS       parallel jobs (default 4)
#   NICE       nice level (default 15)
#   PYTHON     python for the lib's protobuf generator
#              (default <workspace>/build/mac/venv/bin/python)
#   CONFIG     CMake build type (default Release)
#   NR_DIST    the neural rendering engine to put into the app: the dist directory of
#              macos/nr/build.sh (default <workspace>/build/native-app/nr/dist; when it is
#              not there the app is built without the engine and runs as before)
#
# Needs Xcode (the full SDK: the video pipeline uses CAMetalDisplayLink, macOS 14) and
# Homebrew's openssl@3, opus, json-c, miniupnpc, protobuf-compatible python.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
SRC=$(cd "$HERE/../.." && pwd -P)
case "$SRC" in
	*/wt/*) WORKSPACE=${SRC%%/wt/*} ;;
	*) WORKSPACE=$(dirname "$SRC") ;;
esac
BUILD=$WORKSPACE/build/native-app/app
RUN_TESTS=1
for a in "$@"; do
	case "$a" in
		--no-tests) RUN_TESTS=0 ;;
		-*) echo "unknown option $a" >&2; exit 2 ;;
		*) BUILD=$a ;;
	esac
done
JOBS=${JOBS:-4}
NICE=${NICE:-15}
CONFIG=${CONFIG:-Release}
PYTHON=${PYTHON:-$WORKSPACE/build/mac/venv/bin/python}
NR_DIST=${NR_DIST:-$WORKSPACE/build/native-app/nr/dist}
[ -f "$NR_DIST/libplknr.dylib" ] || NR_DIST=
SDK=$(xcrun --sdk macosx --show-sdk-path)
OPENSSL=$(brew --prefix openssl@3 2> /dev/null || echo /opt/homebrew/opt/openssl@3)
OPUS=$(brew --prefix opus 2> /dev/null || echo /opt/homebrew/opt/opus)

# /opt/homebrew/include may hold a newer OpenSSL's headers; the lib links openssl@3, so its
# headers go first on the include path through a one-entry directory, and Opus is taken from
# its own prefix (found through /opt/homebrew/include, that directory would come first).
SHIM=$WORKSPACE/build/native-app/openssl3-shim
mkdir -p "$SHIM"
[ -e "$SHIM/openssl" ] || ln -s "$OPENSSL/include/openssl" "$SHIM/openssl"

if [ ! -f "$BUILD/CMakeCache.txt" ]; then
	nice -n "$NICE" cmake -G Ninja -S "$SRC" -B "$BUILD" \
		-DCMAKE_BUILD_TYPE="$CONFIG" \
		-DCMAKE_OSX_SYSROOT="$SDK" \
		-DCHIAKI_ENABLE_MACOS_NATIVE=ON \
		-DCHIAKI_ENABLE_GUI=OFF \
		-DCHIAKI_ENABLE_CLI=OFF \
		-DCHIAKI_ENABLE_TESTS=OFF \
		-DCHIAKI_ENABLE_FFMPEG_DECODER=OFF \
		-DCHIAKI_ENABLE_STEAMDECK_NATIVE=OFF \
		-DCHIAKI_ENABLE_STEAM_SHORTCUT=OFF \
		-DCMAKE_PREFIX_PATH="/opt/homebrew;$OPENSSL" \
		-DOPENSSL_ROOT_DIR="$OPENSSL" \
		-DOpus_INCLUDE_DIRS="$OPUS/include" \
		-DPYTHON_EXECUTABLE="$PYTHON" \
		-DCMAKE_C_FLAGS="-I$SHIM" \
		-DCMAKE_CXX_FLAGS="-I$SHIM" \
		-DCMAKE_OBJC_FLAGS="-I$SHIM" \
		-DCMAKE_OBJCXX_FLAGS="-I$SHIM"
fi
# A build directory configured before the 1 MB receive buffer became the default on macOS
# still has it off: the cache keeps the old answer.
if ! grep -q '^CHIAKI_LIB_ENABLE_TAKION_SOCKET_TUNING:BOOL=ON' "$BUILD/CMakeCache.txt"; then
	nice -n "$NICE" cmake -S "$SRC" -B "$BUILD" -DCHIAKI_LIB_ENABLE_TAKION_SOCKET_TUNING=ON > /dev/null
fi
# The engine comes and goes with its dist directory, also in a build directory that exists.
if ! grep -q "^PLK_NR_DIST:PATH=$NR_DIST\$" "$BUILD/CMakeCache.txt"; then
	nice -n "$NICE" cmake -S "$SRC" -B "$BUILD" -DPLK_NR_DIST="$NR_DIST" > /dev/null
fi
if [ -n "$NR_DIST" ]; then
	echo "neural rendering engine: $NR_DIST"
else
	echo "neural rendering engine: none (macos/nr/build.sh builds it)"
fi
nice -n "$NICE" cmake --build "$BUILD" -j "$JOBS"

echo "app: $BUILD/macos/app/Pleikkari.app"
if [ "$RUN_TESTS" = 1 ]; then
	# Unit tests only: no console, no window, nothing written to a controller.
	(cd "$BUILD" && nice -n "$NICE" ctest --output-on-failure -j 1)
fi
