#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build OpenImageDenoise as static libraries for Android. The prebuilt deps
# ship it shared, and shared OIDN cannot create a CPU device inside an APK: it
# dlopens its CPU module as libOpenImageDenoise_device_cpu.so.<version>, and
# Android only installs native libraries named exactly lib*.so. Linked
# statically, the CPU device is registered directly and nothing is loaded.
#
# Same source, patches and options as build_environment/cmake/openimagedenoise.cmake
# for Android, plus OIDN_STATIC_LIB. build.py runs this for configurations with
# WITH_OPENIMAGEDENOISE. Finished steps are skipped, so reruns are cheap.
#
# Progress goes to stderr. The install prefix is printed on stdout as a
# KEY=VALUE line for build.py to pick up.

set -euo pipefail

# Keep stdout for the result line only.
exec 3>&1 1>&2

source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

# Pinned to match build_environment/cmake/versions.cmake; bump together.
OIDN_VERSION="2.5.0"
OIDN_MD5="ae984ce4cc4c81ec152ab81b241a8738"
ISPC_VERSION="1.30.0"

DEPS="${BLENDER_ANDROID_OIDN_DEPS:-$(dirname "$ANDROID_REPO_ROOT")/blender_build_android/oidn-deps}"
PATCH_DIR="$ANDROID_REPO_ROOT/build_files/build_environment/patches"
LIBDIR="${LIBDIR:-$ANDROID_REPO_ROOT/lib/android_arm64}"
SRC="$DEPS/oidn-$OIDN_VERSION"
BUILD="$DEPS/build-$OIDN_VERSION"
PREFIX="$DEPS/install-$OIDN_VERSION"
ISPC_DIR="$DEPS/ispc-v$ISPC_VERSION"
ISPC="$ISPC_DIR/bin/ispc"
mkdir -p "$DEPS"

log() { echo "[oidn] $*"; }

if [ -f "$PREFIX/lib/libOpenImageDenoise_core.a" ]; then
  log "OpenImageDenoise $OIDN_VERSION already built"
  echo "BLENDER_ANDROID_OIDN_ROOT=$PREFIX" >&3
  exit 0
fi

# --- ISPC (host compiler for the CPU kernels) --------------------------------
if [ ! -x "$ISPC" ]; then
  log "downloading ISPC $ISPC_VERSION"
  rm -rf "$ISPC_DIR"
  mkdir -p "$ISPC_DIR"
  curl -fL "https://github.com/ispc/ispc/releases/download/v$ISPC_VERSION/ispc-v$ISPC_VERSION-linux.tar.gz" \
    | tar -xz -C "$ISPC_DIR" --strip-components=1
fi
# The help text wraps the OS list, so join it before matching.
if ! "$ISPC" --help | tr '\n' ' ' | grep -q -- '--target-os=[^[]*android'; then
  log "ERROR: $ISPC cannot target Android (no android in --target-os)"
  exit 1
fi

# --- OIDN source --------------------------------------------------------------
TARBALL="$DEPS/oidn-$OIDN_VERSION.src.tar.gz"
if [ ! -f "$TARBALL" ]; then
  log "downloading OpenImageDenoise $OIDN_VERSION source"
  curl -fL -o "$TARBALL.part" \
    "https://github.com/RenderKit/oidn/releases/download/v$OIDN_VERSION/oidn-$OIDN_VERSION.src.tar.gz"
  mv "$TARBALL.part" "$TARBALL"
fi
echo "$OIDN_MD5  $TARBALL" | md5sum -c -

if [ ! -f "$SRC/.patched" ]; then
  rm -rf "$SRC"
  mkdir -p "$SRC"
  tar -xzf "$TARBALL" -C "$SRC" --strip-components=1
  patch --verbose -p 1 -N -d "$SRC" < "$PATCH_DIR/oidn.diff"
  patch -p 1 -d "$SRC" < "$PATCH_DIR/oidn_android_thread_affinity.diff"
  touch "$SRC/.patched"
fi

# --- Build --------------------------------------------------------------------
log "configuring OpenImageDenoise $OIDN_VERSION (static, CPU device)"
cmake -S "$SRC" -B "$BUILD" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_TOOLCHAIN_FILE" \
  -DANDROID_ABI="$ANDROID_ABI" \
  -DANDROID_PLATFORM="android-$ANDROID_API" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DOIDN_STATIC_LIB=ON \
  -DOIDN_DEVICE_CPU=ON \
  -DOIDN_APPS=OFF \
  -DOIDN_FILTER_RTLIGHTMAP=OFF \
  -DTBB_ROOT="$LIBDIR/tbb" \
  -DTBB_DIR="$LIBDIR/tbb/lib/cmake/TBB" \
  -DCMAKE_FIND_ROOT_PATH="$LIBDIR/tbb" \
  -DISPC_EXECUTABLE="$ISPC" \
  -DISPC_TARGET_OS=--target-os=android \
  -DPython_EXECUTABLE="$(command -v python3)"
log "building OpenImageDenoise $OIDN_VERSION"
ninja -C "$BUILD" install

log "installed: $PREFIX"
echo "BLENDER_ANDROID_OIDN_ROOT=$PREFIX" >&3
