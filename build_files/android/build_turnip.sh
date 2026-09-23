#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Download and build the two Turnip dependencies that are not in the repo:
# libadrenotools and Mesa's Turnip Vulkan driver. build.py runs this for a
# --turnip build when BLENDER_ANDROID_ADRENOTOOLS or BLENDER_ANDROID_TURNIP_DRIVER
# is unset. Finished steps are skipped, so reruns are cheap.
#
# Progress goes to stderr. The two resulting paths are printed on stdout as
# KEY=VALUE lines for build.py to pick up.

set -euo pipefail

# Keep stdout for the result lines only.
exec 3>&1 1>&2

source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

# Pinned so a Turnip APK is reproducible; bump deliberately.
MESA_TAG="${BLENDER_ANDROID_MESA_TAG:-mesa-26.2.3}"
ADRENOTOOLS_REV="${BLENDER_ANDROID_ADRENOTOOLS_REV:-8fae8ce254dfc1344527e05301e43f37dea2df80}"
MESON_VERSION="1.9.1"
GLSLANG_VERSION="16.6.0"

DEPS="${BLENDER_ANDROID_TURNIP_DEPS:-$(dirname "$ANDROID_REPO_ROOT")/blender_build_android/turnip-deps}"
ADRENOTOOLS_DIR="$DEPS/libadrenotools-${ADRENOTOOLS_REV:0:12}"
MESA_DIR="$DEPS/$MESA_TAG"
DRIVER="$MESA_DIR/out/libvulkan_freedreno.so"
mkdir -p "$DEPS"

log() { echo "[turnip] $*"; }

# --- libadrenotools -----------------------------------------------------------
if [ ! -f "$ADRENOTOOLS_DIR/build/libadrenotools.a" ]; then
  if [ ! -d "$ADRENOTOOLS_DIR/.git" ]; then
    log "cloning libadrenotools ${ADRENOTOOLS_REV:0:12}"
    git clone https://github.com/bylaws/libadrenotools "$ADRENOTOOLS_DIR"
  fi
  git -C "$ADRENOTOOLS_DIR" checkout --quiet "$ADRENOTOOLS_REV"
  git -C "$ADRENOTOOLS_DIR" submodule update --init --recursive
  log "building libadrenotools"
  cmake -S "$ADRENOTOOLS_DIR" -B "$ADRENOTOOLS_DIR/build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_TOOLCHAIN_FILE" \
    -DANDROID_ABI="$ANDROID_ABI" \
    -DANDROID_PLATFORM="android-$ANDROID_API" \
    -DCMAKE_BUILD_TYPE=Release
  ninja -C "$ADRENOTOOLS_DIR/build"
else
  log "libadrenotools ${ADRENOTOOLS_REV:0:12} already built"
fi

# --- Mesa Turnip --------------------------------------------------------------
if [ ! -f "$DRIVER" ]; then
  # Mesa needs a newer meson than LTS distributions ship, plus mako. Install
  # both into a private directory: a venv would need python3-venv, which
  # Debian and Ubuntu leave out by default.
  PYTOOLS="$DEPS/pytools-meson-$MESON_VERSION"
  if [ ! -f "$PYTOOLS/bin/meson" ]; then
    log "installing meson $MESON_VERSION and mako into $PYTOOLS"
    python3 -m pip install --quiet --target "$PYTOOLS" \
      "meson==$MESON_VERSION" mako pyyaml packaging
  fi
  # Mesa's code generators run under the same python3 and import mako too.
  export PYTHONPATH="$PYTOOLS${PYTHONPATH:+:$PYTHONPATH}"
  MESON=(python3 "$PYTOOLS/bin/meson")

  # Turnip compiles its ray-tracing BVH shaders with a host glslangValidator,
  # which distributions do not install by default.
  if ! command -v glslangValidator >/dev/null 2>&1; then
    GLSLANG="$DEPS/glslang-$GLSLANG_VERSION"
    if [ ! -x "$GLSLANG/bin/glslangValidator" ]; then
      log "downloading glslang $GLSLANG_VERSION"
      mkdir -p "$GLSLANG"
      curl -fsSL "https://github.com/KhronosGroup/glslang/releases/download/$GLSLANG_VERSION/glslang-$GLSLANG_VERSION-linux-x86_64-release.tar.gz" \
        | tar -xz -C "$GLSLANG"
    fi
    export PATH="$GLSLANG/bin:$PATH"
  fi

  if [ ! -d "$MESA_DIR/src/.git" ]; then
    log "cloning $MESA_TAG"
    git clone --depth 1 --branch "$MESA_TAG" \
      https://gitlab.freedesktop.org/mesa/mesa.git "$MESA_DIR/src"
  fi

  # pkg-config is disabled so meson cannot pick up host libraries for the
  # Android target; everything the driver needs comes from the NDK sysroot.
  CROSS="$MESA_DIR/android-cross.ini"
  cat > "$CROSS" <<EOF
[binaries]
c = ['$ANDROID_LLVM_BIN/clang', '--target=aarch64-linux-android$ANDROID_API']
cpp = ['$ANDROID_LLVM_BIN/clang++', '--target=aarch64-linux-android$ANDROID_API']
ar = '$ANDROID_LLVM_BIN/llvm-ar'
strip = '$ANDROID_LLVM_BIN/llvm-strip'
pkg-config = '/bin/false'

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8'
endian = 'little'
EOF

  if [ ! -f "$MESA_DIR/build/build.ninja" ]; then
    # Left behind by a failed configure; meson refuses to reuse it.
    rm -rf "$MESA_DIR/build"
    log "configuring $MESA_TAG (Turnip on KGSL only)"
    "${MESON[@]}" setup "$MESA_DIR/build" "$MESA_DIR/src" \
      --cross-file "$CROSS" \
      -Dbuildtype=release -Db_ndebug=true -Dstrip=true \
      -Dplatforms=android -Dplatform-sdk-version="$ANDROID_API" \
      -Dandroid-stub=true -Dandroid-libbacktrace=disabled \
      -Dvulkan-drivers=freedreno -Dfreedreno-kmds=kgsl -Dgallium-drivers= \
      -Dopengl=false -Degl=disabled -Dgles1=disabled -Dgles2=disabled \
      -Dglx=disabled -Dgbm=disabled -Dllvm=disabled \
      -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
      -Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled
  fi
  log "building $MESA_TAG"
  ninja -C "$MESA_DIR/build" src/freedreno/vulkan/libvulkan_freedreno.so

  mkdir -p "$(dirname "$DRIVER")"
  cp "$MESA_DIR/build/src/freedreno/vulkan/libvulkan_freedreno.so" "$DRIVER"
else
  log "$MESA_TAG Turnip already built"
fi

log "libadrenotools: $ADRENOTOOLS_DIR"
log "driver:         $DRIVER"
echo "BLENDER_ANDROID_ADRENOTOOLS=$ADRENOTOOLS_DIR" >&3
echo "BLENDER_ANDROID_TURNIP_DRIVER=$DRIVER" >&3
