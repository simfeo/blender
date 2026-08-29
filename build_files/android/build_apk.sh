#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build a full Android APK for a given config, end to end:
#   build_files/android/build_apk.sh [lite|full]
#
# Steps: host codegen tools (config-matched) -> cross-compile libblender.so ->
# package APK. Requires lib/android_arm64 (git submodule update --init).

set -euo pipefail
CONFIG="${1:-full}"
case "$CONFIG" in lite|full) ;; *) echo "usage: $0 [lite|full]" >&2; exit 1;; esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
# Drop any leaked Android Studio SDK/NDK paths so env.sh picks the Homebrew ones
# (Studio's SDK lacks our build-tools/NDK version).
unset ANDROID_HOME ANDROID_NDK_ROOT ANDROID_NDK_HOME
# shellcheck source=/dev/null
source "$SCRIPT_DIR/env.sh"
cd "$REPO_ROOT"

# Canonical path: CMake records a resolved one, and comparing an unresolved
# path against it made drop_stale_cache wipe the build dir on every run.
BUILD_BASE="${BUILD_BASE:-$(cd "$REPO_ROOT/.." && pwd)/blender_build_android}"
HOST="$BUILD_BASE/build_host_tools_$CONFIG"
BUILD="$BUILD_BASE/build_android_$CONFIG"
FEATURES="build_files/android/android_features_$CONFIG.cmake"

# A CMake build dir records its own absolute path; if it was moved, cmake refuses
# to reconfigure. Wipe a relocated cache so the configure below regenerates it.
drop_stale_cache() {
  local d="$1" cache="$1/CMakeCache.txt"
  [ -f "$cache" ] || return 0
  if ! grep -q "CMAKE_CACHEFILE_DIR:INTERNAL=$d\$" "$cache" 2>/dev/null; then
    echo "[build_apk] relocated cache in $d — wiping for a clean reconfigure"
    rm -rf "$d"
  fi
}
drop_stale_cache "$HOST"
drop_stale_cache "$BUILD"

echo "=== [$CONFIG] host codegen tools ==="
if [ ! -x "$HOST/bin/makesdna" ]; then
  # Only the code generators are wanted here, so keep the GUI out: on Linux a
  # full configure demands X11 development packages that nothing here uses.
  # The feature set must otherwise match the target, or makesrna emits a
  # different set of files than the target build expects. So TBB stays on
  # and gets an rpath: Linux does not bake the library path into the
  # executable the way macOS does, and the generator would not start.
  cmake -S . -B "$HOST" -G Ninja -C "$FEATURES" -DWITH_CROSSCOMPILED_TOOLS=OFF \
    -DCMAKE_C_COMPILER="$ANDROID_HOST_CC" -DCMAKE_CXX_COMPILER="$ANDROID_HOST_CXX" \
    -DWITH_HEADLESS=ON -DWITH_X11_XINPUT=OFF -DWITH_AUDASPACE=OFF \
    -DCMAKE_BUILD_RPATH="$REPO_ROOT/lib/linux_x64/tbb/lib"
  ninja -C "$HOST" makesdna makesrna datatoc msgfmt shader_tool
fi

echo "=== [$CONFIG] configure + build libblender.so ==="
# The nested host_tools sub-build gets no toolchain file and would otherwise
# fall back to the system default cc, which is below the minimum Blender takes.
cmake -S . -B "$BUILD" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_TOOLCHAIN_FILE" \
  -DANDROID_ABI="$ANDROID_ABI" -DANDROID_PLATFORM="android-$ANDROID_API" \
  -DHOST_C_COMPILER="$ANDROID_HOST_CC" -DHOST_CXX_COMPILER="$ANDROID_HOST_CXX" \
  -DBLENDER_ANDROID_CONFIG="$CONFIG"
ninja -C "$BUILD" blender
# Built separately: nothing links them, the glTF add-on dlopens them. Only the
# configs that enable the codecs define these targets, so ask ninja first.
for _bridge in bf_intern_meshopt_bridge bf_intern_draco_bridge; do
  if ninja -C "$BUILD" -n "$_bridge" >/dev/null 2>&1; then
    ninja -C "$BUILD" "$_bridge"
  fi
done

echo "=== [$CONFIG] package APK ==="
BLENDER_ANDROID_CONFIG="$CONFIG" BUILD="$BUILD" bash "$SCRIPT_DIR/apk/package.sh"
