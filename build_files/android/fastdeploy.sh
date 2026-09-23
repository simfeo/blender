#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Fast iteration: recompile libblender.so and swap it into the already-packaged
# APK, reusing the runtime payload + all other native libs (no 166 MB re-zip).
# Requires a prior full `build_apk.sh <cfg>` to have populated the stage dir.
#
#   build_files/android/fastdeploy.sh [lite|full]

set -euo pipefail
CONFIG="${1:-full}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
unset ANDROID_HOME ANDROID_NDK_ROOT ANDROID_NDK_HOME
# shellcheck source=/dev/null
source "$SCRIPT_DIR/env.sh"
cd "$REPO_ROOT"

# Canonical path: CMake records a resolved one, and comparing an unresolved
# path against it made drop_stale_cache wipe the build dir on every run.
BUILD_BASE="${BUILD_BASE:-$(cd "$REPO_ROOT/.." && pwd)/blender_build_android}"
FLAVOUR="${BLENDER_ANDROID_FLAVOUR:-}"
BUILD="$BUILD_BASE/build_android_$CONFIG$FLAVOUR"
STAGE="$BUILD_BASE/android_apk_stage_$CONFIG$FLAVOUR"
OUT="$STAGE/blender-$CONFIG$FLAVOUR.apk"
JNI="$STAGE/lib/arm64-v8a"
BT="$ANDROID_HOME/build-tools/35.0.1"
ADB="$ANDROID_HOME/platform-tools/adb"
[ -x "$ADB" ] || ADB="$HOME/Library/Android/sdk/platform-tools/adb"

if [ ! -f "$STAGE/base.apk" ] || [ ! -f "$JNI/libblender.so" ]; then
  echo "No packaged stage found; run build_apk.sh $CONFIG once first." >&2
  exit 1
fi

echo "=== [$CONFIG] compile libblender.so ==="
ninja -C "$BUILD" blender

echo "=== swap libblender.so into APK ==="
cp "$BUILD/lib/libblender.so" "$JNI/libblender.so"
patchelf --set-soname libblender.so "$JNI/libblender.so" 2>/dev/null || true
"$ANDROID_LLVM_BIN/llvm-strip" --strip-unneeded "$JNI/libblender.so" 2>/dev/null || true

echo "=== reassemble + sign (reusing runtime payload in base.apk) ==="
cp "$STAGE/base.apk" "$OUT"
( cd "$STAGE/dex" && zip -q "$OUT" classes.dex )
( cd "$STAGE" && zip -qr "$OUT" lib )
"$BT/zipalign" -f -p 4 "$OUT" "$STAGE/fast-aligned.apk"
mv "$STAGE/fast-aligned.apk" "$OUT"
"$BT/apksigner" sign --ks "$BUILD_BASE/android-debug.keystore" \
  --ks-pass pass:android --key-pass pass:android "$OUT"

if [ -n "${FASTDEPLOY_NO_INSTALL:-}" ]; then
  echo "=== built ($CONFIG), install skipped ==="
  exit 0
fi

echo "=== install + launch ==="
"$ADB" install -r "$OUT"
"$ADB" shell am force-stop org.blender.blender
"$ADB" shell am start -n org.blender.blender/.BlenderActivity
echo "=== deployed ($CONFIG) ==="
