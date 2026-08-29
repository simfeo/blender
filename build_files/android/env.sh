# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Android cross-compile environment for Blender.
# Source this file:  `source build_files/android/env.sh`
#
# Overridable by exporting before sourcing. Defaults match the toolchain
# installed via Homebrew `android-commandlinetools` + `sdkmanager`.

# Where the checkout is, resolved from this file rather than the caller's
# directory, since env.sh is sourced from several places.
ANDROID_REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export ANDROID_REPO_ROOT

# --- SDK / NDK locations -----------------------------------------------------
# Host differences: the NDK ships per-host prebuilt binaries, and the SDK lands
# wherever each platform's package manager puts it.
case "$(uname -s)" in
  Darwin) ANDROID_HOST_TAG="darwin-x86_64"; ANDROID_HOME_DEFAULT="/opt/homebrew/share/android-commandlinetools" ;;
  Linux)  ANDROID_HOST_TAG="linux-x86_64";  ANDROID_HOME_DEFAULT="$HOME/android-sdk" ;;
  *)      echo "[blender-android] WARNING: unsupported host $(uname -s)" ;;
esac
export ANDROID_HOST_TAG
export ANDROID_HOME="${ANDROID_HOME:-$ANDROID_HOME_DEFAULT}"
# Matched to whatever built lib/android_arm64: those libraries are clang 21,
# and libc++ symbols they reference (__hash_memory) do not exist in the r28
# runtime, so the app dies in dlopen before reaching any Blender code.
export ANDROID_NDK_VERSION="${ANDROID_NDK_VERSION:-30.0.14904198-beta1}"
export ANDROID_NDK_ROOT="${ANDROID_NDK_ROOT:-$ANDROID_HOME/ndk/$ANDROID_NDK_VERSION}"
export ANDROID_NDK_HOME="$ANDROID_NDK_ROOT"

# JDK (keg-only Homebrew openjdk) — needed for sdkmanager/adb wrappers.
case "$(uname -s)" in
  Darwin) JAVA_HOME_DEFAULT="/opt/homebrew/opt/openjdk/libexec/openjdk.jdk/Contents/Home" ;;
  Linux)  JAVA_HOME_DEFAULT="/usr/lib/jvm/java-17-openjdk-amd64" ;;
esac
export JAVA_HOME="${JAVA_HOME:-$JAVA_HOME_DEFAULT}"

# --- Target: Samsung Galaxy Tab S8 FE ---------------------------------------
# SoC: Exynos 1380 (arm64). Vulkan 1.1+. Ships Android 12, updatable to 14/15.
export ANDROID_ABI="${ANDROID_ABI:-arm64-v8a}"
# Minimum API we build against. 31 (Android 12) is both target devices' launch
# API and provides bionic funcs (timespec_get@29 etc.) Blender core expects.
export ANDROID_API="${ANDROID_API:-31}"
# API we compile/target the app against (manifest targetSdkVersion). Android 14.
export ANDROID_TARGET_API="${ANDROID_TARGET_API:-34}"

# --- Derived ----------------------------------------------------------------
export ANDROID_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake"
# Host prebuilt dir. The NDK ships x86_64 host binaries; on Apple Silicon they
# run under Rosetta.
export ANDROID_LLVM_BIN="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/$ANDROID_HOST_TAG/bin"
export ANDROID_SYSROOT="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/$ANDROID_HOST_TAG/sysroot"

# cmake, ninja and meson come from Homebrew on macOS, which is absent from the
# PATH of a non-interactive shell: ssh, cron and CI all start without it, and
# the build then dies on "cmake: command not found" far from the real cause.
case "$(uname -s)" in
  Darwin) PATH="/opt/homebrew/bin:/usr/local/bin:$PATH" ;;
  # MaterialX and USD need CMake 3.26+, newer than Ubuntu 22.04 ships. A local
  # copy under $HOME wins over the system one when present.
  Linux)  [ -x "$HOME/cmake/bin/cmake" ] && PATH="$HOME/cmake/bin:$PATH" ;;
esac
export PATH="$JAVA_HOME/bin:$ANDROID_HOME/platform-tools:$ANDROID_LLVM_BIN:$PATH"

# Blender wants GCC 14+ or Clang 17+. Distributions often default to something
# older, and the failure only appears once the host tools configure, so pick a
# suitable compiler here rather than changing the system default.
if [ -z "${ANDROID_HOST_CC:-}" ]; then
  for _cc in gcc-15 gcc-14 clang-18 clang-17; do
    if command -v "$_cc" >/dev/null 2>&1; then
      ANDROID_HOST_CC="$_cc"
      case "$_cc" in
        gcc-*)   ANDROID_HOST_CXX="g++-${_cc#gcc-}" ;;
        clang-*) ANDROID_HOST_CXX="clang++-${_cc#clang-}" ;;
      esac
      break
    fi
  done
  unset _cc
fi
export ANDROID_HOST_CC="${ANDROID_HOST_CC:-cc}"
export ANDROID_HOST_CXX="${ANDROID_HOST_CXX:-c++}"

# The host code generators link against the precompiled libraries in
# lib/linux_x64 and are then run during the target build. The rpath recorded in
# them is not honoured here, so name the directories for the loader instead;
# otherwise makesrna dies partway through with a missing libtbb.
if [ "$(uname -s)" = Linux ] && [ -d "$ANDROID_REPO_ROOT/lib/linux_x64" ]; then
  for _d in "$ANDROID_REPO_ROOT"/lib/linux_x64/*/lib; do
    [ -d "$_d" ] && LD_LIBRARY_PATH="$_d${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  done
  export LD_LIBRARY_PATH
  unset _d
fi

for _tool in cmake ninja; do
  command -v "$_tool" >/dev/null 2>&1 || \
    echo "[blender-android] WARNING: $_tool not found in PATH"
done
unset _tool

echo "[blender-android] NDK   : $ANDROID_NDK_ROOT"
echo "[blender-android] ABI   : $ANDROID_ABI   API(min): $ANDROID_API   target: $ANDROID_TARGET_API"
echo "[blender-android] toolch: $ANDROID_TOOLCHAIN_FILE"
[ -f "$ANDROID_TOOLCHAIN_FILE" ] || echo "[blender-android] WARNING: toolchain file not found — check ANDROID_NDK_ROOT"
