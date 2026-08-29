#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Package the cross-compiled Blender into an installable APK (no gradle).
# Gathers libblender.so + its transitive .so deps, compiles BlenderActivity,
# and assembles a debug-signed APK with the SDK build-tools.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
# shellcheck source=/dev/null
source "$REPO_ROOT/build_files/android/env.sh"

CONFIG="${BLENDER_ANDROID_CONFIG:-full}"
# Canonical path: CMake records a resolved one, and comparing an unresolved
# path against it made drop_stale_cache wipe the build dir on every run.
BUILD_BASE="${BUILD_BASE:-$(cd "$REPO_ROOT/.." && pwd)/blender_build_android}"
: "${LIBDIR:=$REPO_ROOT/lib/android_arm64}"
BUILD="${BUILD:-$BUILD_BASE/build_android_$CONFIG}"
BT="$ANDROID_HOME/build-tools/35.0.1"
ANDROID_JAR="$ANDROID_HOME/platforms/android-35/android.jar"
# Flavours differ only in what gets packaged, so they share a build tree but
# need their own stage and output name.
FLAVOUR="${BLENDER_ANDROID_FLAVOUR:-}"
STAGE="$BUILD_BASE/android_apk_stage_$CONFIG$FLAVOUR"
JNI="$STAGE/lib/arm64-v8a"
OUT="$STAGE/blender-$CONFIG$FLAVOUR.apk"

rm -rf "$STAGE"; mkdir -p "$JNI"

echo "[apk] gathering native libraries"
cp "$BUILD/lib/libblender.so" "$JNI/"
cp "$ANDROID_SYSROOT/usr/lib/aarch64-linux-android/libc++_shared.so" "$JNI/"

readelf_needed() {
  "$ANDROID_LLVM_BIN/llvm-readelf" -d "$1" 2>/dev/null |
    sed -nE 's/.*\(NEEDED\).*\[(.*)\]/\1/p'
}
# Android requires unversioned sonames (libX.so, never libX.so.N).
unversion() { echo "${1%%.so*}.so"; }
searchdirs=$(ls -d "$LIBDIR"/*/lib 2>/dev/null)

# Assemble the runtime payload first, so its python .so can seed dependency
# resolution (their NEEDED libs — libffi, ssl, sqlite… — must ship in jniLibs).
echo "[apk] assembling runtime payload (python + scripts + datafiles)"
ASSETS="$STAGE/assets"
PAYLOAD="$STAGE/payload"
mkdir -p "$ASSETS" "$PAYLOAD/python/lib"
cp -R "$REPO_ROOT/release/datafiles" "$PAYLOAD/datafiles"
cp -R "$REPO_ROOT/scripts" "$PAYLOAD/scripts"

# Cycles registers itself from Python, and that half lives outside scripts/ --
# CMake only puts it in place during install, which this packaging path skips.
# Without it the engine is linked in but never appears in the render engine list.
if grep -q "set(WITH_CYCLES ON" "$REPO_ROOT/build_files/android/android_features_$CONFIG.cmake"; then
  cp -R "$REPO_ROOT/intern/cycles/blender/addon" "$PAYLOAD/scripts/addons_core/cycles"
  echo "[apk] bundled the Cycles add-on"
fi
cp -R "$LIBDIR/python/lib/python3.13" "$PAYLOAD/python/lib/python3.13"

# Extensions are not part of the source tree; a desktop install ships the empty
# extensions/system directory and the user fetches the rest from
# extensions.blender.org. Android has no practical way to do that on device, so
# anything dropped in EXT_SRC is baked in instead. Blender's built-in "System"
# repository picks these up with no preference changes.
EXT_SRC="${BLENDER_ANDROID_EXTENSIONS_DIR:-$SCRIPT_DIR/extensions}"
EXT_DST="$PAYLOAD/extensions/system"
mkdir -p "$EXT_DST"
if [ -d "$EXT_SRC" ]; then
  for ext in "$EXT_SRC"/*; do
    [ -e "$ext" ] || continue
    name="$(basename "$ext")"
    case "$ext" in
      *.zip)
        name="${name%.zip}"
        # A published extension zips its manifest at the archive root, so it
        # unpacks straight into the destination directory.
        mkdir -p "$EXT_DST/$name"
        unzip -qo "$ext" -d "$EXT_DST/$name"
        ;;
      *)
        [ -d "$ext" ] || continue
        cp -R "$ext" "$EXT_DST/$name"
        ;;
    esac
    if [ ! -f "$EXT_DST/$name/blender_manifest.toml" ]; then
      echo "[apk] WARNING: $name has no blender_manifest.toml, Blender will ignore it"
    fi
    # Everything bundled ships inside a GPL binary, so a non-GPL-compatible
    # extension has to be caught here rather than at release time.
    lic="$(sed -nE 's/^license *= *\["([^"]+)".*/\1/p' "$EXT_DST/$name/blender_manifest.toml" 2>/dev/null)"
    echo "[apk] bundled extension: $name (${lic:-license unknown})"
  done
fi
find "$PAYLOAD" -name '__pycache__' -type d -prune -exec rm -rf {} + 2>/dev/null || true

echo "[apk] gathering native libraries (unversioned)"
# Seed queue: libblender + c++_shared + every python extension module.
for so in $(find "$PAYLOAD/python" -name '*.so'); do
  b="$(unversion "$(basename "$so")")"
  [ -f "$JNI/$b" ] || cp "$so" "$JNI/$b"
done
changed=1
while [ "$changed" = 1 ]; do
  changed=0
  for cur in "$JNI"/*.so; do
    for need in $(readelf_needed "$cur"); do
      case "$need" in lib*.so|lib*.so.*) ;; *) continue;; esac
      base="$(unversion "$need")"
      [ -f "$JNI/$base" ] && continue
      case "$base" in
        libc.so|libm.so|libdl.so|liblog.so|libandroid.so|libGLESv1_CM.so|\
        libGLESv2.so|libGLESv3.so|libEGL.so|libvulkan.so|libOpenSLES.so|\
        libjnigraphics.so|libz.so) continue;;
      esac
      for d in $searchdirs; do
        if [ -f "$d/$base" ]; then cp "$d/$base" "$JNI/$base"; changed=1; break; fi
        cand=$(ls "$d/$base".* 2>/dev/null | head -1 || true)
        if [ -n "$cand" ]; then cp "$cand" "$JNI/$base"; changed=1; break; fi
      done
    done
  done
done

echo "[apk] rewriting sonames + NEEDED to unversioned"
patch_unversion() {
  patchelf --set-soname "$(basename "$1")" "$1" 2>/dev/null || true
  for need in $(readelf_needed "$1"); do
    case "$need" in
      *.so.*) patchelf --replace-needed "$need" "$(unversion "$need")" "$1" 2>/dev/null || true;;
    esac
  done
}
for so in "$JNI"/*.so; do patch_unversion "$so"; done
# Python extension modules load from filesDir but resolve NEEDED via jniLibs.
for so in $(find "$PAYLOAD/python" -name '*.so'); do
  patch_unversion "$so"
done

echo "[apk] stripping native libraries"
for so in "$JNI"/*.so; do
  "$ANDROID_LLVM_BIN/llvm-strip" --strip-unneeded "$so" 2>/dev/null || true
done
echo "[apk] bundled $(ls "$JNI" | wc -l | tr -d ' ') native libraries ($(du -sh "$JNI" | cut -f1))"

( cd "$PAYLOAD" && zip -qr -X "$ASSETS/blender_runtime.zip" . )
echo "[apk] runtime payload: $(du -sh "$ASSETS/blender_runtime.zip" | cut -f1)"

echo "[apk] compiling BlenderActivity"
mkdir -p "$STAGE/javac" "$STAGE/dex"
"$JAVA_HOME/bin/javac" -classpath "$ANDROID_JAR" -source 17 -target 17 \
  -d "$STAGE/javac" \
  "$SCRIPT_DIR/app/src/main/java/org/blender/blender/BlenderActivity.java"
"$BT/d8" --min-api "$ANDROID_API" --output "$STAGE/dex" \
  $(find "$STAGE/javac" -name '*.class')

echo "[apk] compiling resources (launcher icon)"
RES_SRC="$SCRIPT_DIR/app/src/main/res"
mkdir -p "$STAGE/rescompiled"
"$BT/aapt2" compile --dir "$RES_SRC" -o "$STAGE/rescompiled/res.zip"

echo "[apk] linking resources"
# The manifest is not debuggable, so a distributed APK never is by accident.
# Debug builds ask for it here; needed to attach validation layers or a debugger.
AAPT_DEBUG=""
if [ -n "${BLENDER_ANDROID_DEBUGGABLE:-}" ]; then
  AAPT_DEBUG="--debug-mode"
  echo "[apk] debuggable build"
fi
"$BT/aapt2" link -o "$STAGE/base.apk" -I "$ANDROID_JAR" $AAPT_DEBUG \
  --manifest "$SCRIPT_DIR/app/src/main/AndroidManifest.xml" \
  -R "$STAGE/rescompiled/res.zip" \
  -A "$ASSETS" -0 zip \
  --min-sdk-version "$ANDROID_API" --target-sdk-version "$ANDROID_TARGET_API"

echo "[apk] assembling"
cp "$STAGE/base.apk" "$OUT"
( cd "$STAGE/dex" && zip -q "$OUT" classes.dex )
( cd "$STAGE" && zip -qr "$OUT" lib )

echo "[apk] signing"
KS="$BUILD_BASE/android-debug.keystore"  # persistent: stable signature across runs
if [ ! -f "$KS" ]; then
  "$JAVA_HOME/bin/keytool" -genkeypair -keystore "$KS" -storepass android \
    -keypass android -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 \
    -dname "CN=Android Debug,O=Android,C=US" >/dev/null 2>&1
fi
"$BT/zipalign" -f -p 4 "$OUT" "$STAGE/blender-aligned.apk"
mv "$STAGE/blender-aligned.apk" "$OUT"
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android "$OUT"

echo "[apk] done -> $OUT"
ls -lh "$OUT"
