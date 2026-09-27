#!/usr/bin/env bash
# PLE-741: fail unless an APK really is the Oculus Go VR variant. A build that
# silently fell back to the flat app (chiakiGoVr=auto with no SDK) installs and
# runs on the Go, but only as a 2D Oculus TV panel.
#
# usage: check-go-vr-apk.sh <apk>
#   AAPT defaults to $ANDROID_HOME/build-tools/35.0.0/aapt.
set -euo pipefail

apk=${1:?usage: check-go-vr-apk.sh <apk>}
aapt=${AAPT:-${ANDROID_HOME:?set ANDROID_HOME or AAPT}/build-tools/35.0.0/aapt}
fail=0

entries=$(unzip -Z1 "$apk")
for lib in lib/arm64-v8a/libpleikkari-vr.so lib/arm64-v8a/libvrapi.so; do
  if grep -qxF "$lib" <<<"$entries"; then
    echo "ok: $lib"
  else
    echo "::error::$apk has no $lib"
    fail=1
  fi
done

# The VR activity, the Library's VR launch alias and its Oculus category
# (android/app/src/vr/AndroidManifest.xml).
manifest=$("$aapt" dump xmltree "$apk" AndroidManifest.xml)
for name in '[^"]*\.stream\.StreamVrActivity' '[^"]*\.stream\.GoVrLibraryEntry' 'com\.oculus\.intent\.category\.VR'; do
  if found=$(grep -oE "android:name\(0x01010003\)=\"$name\"" <<<"$manifest" | head -1); then
    echo "ok: manifest ${found#*=}"
  else
    echo "::error::$apk manifest declares no android:name matching $name"
    fail=1
  fi
done

[ "$fail" = 0 ] && echo "$apk is the Go VR variant"
exit "$fail"
