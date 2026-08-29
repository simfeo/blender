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

# The Python interpreter, shipped so sys.executable is real. Blender's extension
# system runs its CLI as a subprocess, so browsing or installing an online
# extension needs an interpreter it can actually execute.
#
# It has to live here rather than in the runtime payload: the payload is
# unpacked into the app's data directory, which is mounted noexec from API 29
# on, while this directory is extracted by the package manager and stays
# executable. The lib*.so name is what makes the installer extract it at all;
# it is an ELF executable, not a library, and nothing dlopens it.
PY_BIN="$LIBDIR/python/bin/python3.13"
if [ -f "$PY_BIN" ]; then
  cp "$PY_BIN" "$JNI/libpython3_13_bin.so"
  chmod 755 "$JNI/libpython3_13_bin.so"
  # A plain exec'd child does not inherit the app's library namespace, so the
  # interpreter has to find libpython3.13.so itself. $ORIGIN is this directory.
  patchelf --set-rpath '$ORIGIN' "$JNI/libpython3_13_bin.so"
  echo "[apk] bundled the Python interpreter for sys.executable"
else
  echo "[apk] WARNING: $PY_BIN missing; online extensions will not work" >&2
fi

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

# The essentials asset library. Since 4.3 a brush is an asset rather than code,
# so without this there is not one brush in sculpt, texture paint, vertex paint,
# weight paint, grease pencil or curves, the asset browser reports no catalogs,
# and the bundled compositor, geometry and shader node groups do not exist.
# Blender looks for it at BLENDER_SYSTEM_DATAFILES/assets.
cp -R "$REPO_ROOT/assets" "$PAYLOAD/datafiles/assets"
echo "[apk] bundled the essentials asset library (brushes, node groups)"

# Interface translations. WITH_INTERNATIONAL is on, so Blender uses these when
# present and otherwise leaves the Language menu empty. Only the compiled
# catalogues ship: the .po sources are 81MB and have no business in an APK.
# msgfmt is Blender's own, already built with the host code generators.
MSGFMT="$BUILD_BASE/build_host_tools_$CONFIG/bin/msgfmt"
if [ -x "$MSGFMT" ] && [ -d "$REPO_ROOT/locale/po" ]; then
  LOCALE_DIR="$PAYLOAD/datafiles/locale"
  mkdir -p "$LOCALE_DIR"
  cp "$REPO_ROOT/locale/languages" "$LOCALE_DIR/"
  locale_count=0
  for po in "$REPO_ROOT"/locale/po/*.po; do
    lang="$(basename "$po" .po)"
    mkdir -p "$LOCALE_DIR/$lang/LC_MESSAGES"
    "$MSGFMT" "$po" "$LOCALE_DIR/$lang/LC_MESSAGES/blender.mo"
    locale_count=$((locale_count + 1))
  done
  echo "[apk] bundled $locale_count interface translations"
else
  echo "[apk] WARNING: no msgfmt or locale/po; the interface will be English only" >&2
fi

# USD finds its file-format plugins through the plugInfo.json files here, so
# without them the importers and exporters are built but never register.
if [ -d "$LIBDIR/usd/plugin/usd" ]; then
  mkdir -p "$PAYLOAD/datafiles/usd"
  cp -R "$LIBDIR/usd/plugin/usd/." "$PAYLOAD/datafiles/usd/"
  echo "[apk] bundled USD plugin resources"
fi

# The glTF add-on dlopens these for compressed meshes, from
# resource_path('SYSTEM_LIBS')/scripts/addons_core/io_scene_gltf2. Plain glTF
# works without them; only Draco and meshopt compression need the bridge.
for bridge in meshopt draco; do
  so="$BUILD/lib/libbf_intern_${bridge}_bridge.so"
  if [ -f "$so" ]; then
    dest="$PAYLOAD/scripts/addons_core/io_scene_gltf2/libbf_intern_${bridge}_bridge.so"
    cp "$so" "$dest"
    # Draco is linked statically and arrives around 25MB, nearly all of it
    # debug information no on-device workflow can use.
    "$ANDROID_LLVM_BIN/llvm-strip" --strip-unneeded "$dest"
    echo "[apk] bundled the glTF $bridge bridge ($(du -h "$dest" | cut -f1))"
  fi
done

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
# Blender only defines PYTHON_SSL_CERT_FILE for a portable install, which this
# build is not, so nothing sets SSL_CERT_FILE and OpenSSL looks in the path it
# was configured with on the build machine. BlenderActivity exports the variable
# instead, but that alone is not enough: measured on device, the bundle is
# reported by ssl.get_default_verify_paths() and the file is present, yet
# set_default_verify_paths() leaves an empty trust store and every request fails
# with "unable to get local issuer certificate". Loading the same file
# explicitly yields the full store, so override the default here. site.py
# imports this at interpreter start, which covers Blender's own interpreter and
# the children the extension system spawns.
SITE="$PAYLOAD/python/lib/python3.13/site-packages"

# pip, which a desktop Blender ships in its Python. Add-ons rely on being able
# to install their own dependencies with it. Pure Python, so the host
# interpreter can put it straight into the target tree; the version is the one
# Blender pins. Cached, because packaging should not need the network twice.
PIP_VERSION="$(sed -nE 's/^set\(PYTHON_PIP_VERSION ([0-9.]+)\).*/\1/p' \
  "$REPO_ROOT/build_files/build_environment/cmake/versions.cmake")"
PIP_CACHE="$BUILD_BASE/pip-$PIP_VERSION"
if [ -n "$PIP_VERSION" ]; then
  if [ ! -d "$PIP_CACHE/pip" ]; then
    mkdir -p "$PIP_CACHE"
    python3 -m pip install -q --no-deps --no-compile --upgrade \
      --target "$PIP_CACHE" "pip==$PIP_VERSION" || true
  fi
  if [ -f "$PIP_CACHE/pip/__main__.py" ]; then
    cp -R "$PIP_CACHE/pip" "$SITE/pip"
    cp -R "$PIP_CACHE"/pip-*.dist-info "$SITE/" 2>/dev/null || true
    echo "[apk] bundled pip $PIP_VERSION"
  else
    echo "[apk] WARNING: pip $PIP_VERSION unavailable; add-ons cannot install dependencies" >&2
  fi
fi
if [ -d "$SITE/certifi" ]; then
  cat > "$SITE/sitecustomize.py" <<'SITECUSTOMIZE'
# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""Make the bundled CA bundle the default TLS trust store."""

import os

_cert = os.environ.get("SSL_CERT_FILE")
if _cert and os.path.isfile(_cert):
    try:
        import ssl as _ssl

        _load_default_certs_orig = _ssl.SSLContext.load_default_certs

        def _load_default_certs(self, purpose=_ssl.Purpose.SERVER_AUTH):
            try:
                self.load_verify_locations(cafile=_cert)
            except Exception:
                # Keep the stock behaviour rather than leaving an empty store.
                _load_default_certs_orig(self, purpose)

        _ssl.SSLContext.load_default_certs = _load_default_certs
    except Exception:
        # A Python without _ssl still has to start; TLS simply stays unavailable.
        pass
SITECUSTOMIZE
  echo "[apk] wrote sitecustomize.py (default TLS trust store)"
else
  echo "[apk] WARNING: no certifi in the payload; HTTPS will not verify" >&2
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
# Passed positionally rather than with -R: that flag means overlay, and an
# overlay may only replace resources that already exist, so anything new (the
# splash theme) is rejected.
"$BT/aapt2" link -o "$STAGE/base.apk" -I "$ANDROID_JAR" $AAPT_DEBUG \
  --manifest "$SCRIPT_DIR/app/src/main/AndroidManifest.xml" \
  "$STAGE/rescompiled/res.zip" \
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
