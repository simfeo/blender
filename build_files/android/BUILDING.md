# Building Blender for Android from scratch

End-to-end guide to reproduce the Android arm64 build on an Apple-Silicon Mac.
Target: Android 12+ (minSdk 31), built against Android 14 (targetSdk 34).

> Everything installs into a sibling of the repo:
> `../blender_build_android/lib/android_arm64` (harvested deps), `../blender_build_android/build_android_<cfg>` (Blender),
> `../blender_build_android/build_host_tools_<cfg>` (native codegen tools), `../blender_build_android/android_apk_stage_<cfg>`
> (APK stage), where `<cfg>` is `full` or `lite`.

## Build configurations

Two feature sets, selected with `-DBLENDER_ANDROID_CONFIG=full|lite` (default
`full`):

- **full** - everything: Cycles (+embree), USD, MaterialX, OpenVDB, Alembic,
  LLVM, ffmpeg video codecs. For modern Qualcomm/Exynos flagships.
- **lite** - those heavy features off; core modelling/sculpt/Python only. For
  weaker devices and a much smaller APK.

The feature toggles live in `build_files/android/android_features_{common,full,
lite}.cmake`. The host codegen tools must be built with the **same** config as
the target (else generated RNA/DNA mismatches), so each config has its own host
tools + build + stage dirs.

### One-shot build

```bash
build_files/android/build.py full         # or: lite
build_files/android/build.py full --install --run
```

`build.py` wraps everything below and additionally handles the validation and
Turnip variants, the on-device debug switches, and cache clearing; run it with
`--help` for the list. The shell script it calls still works on its own:

```bash
build_files/android/build_apk.sh full     # or: lite
```

builds the config-matched host tools, cross-compiles `libblender.so`, and
packages `../blender_build_android/android_apk_stage_<cfg>/blender-<cfg>.apk`. The manual steps below
show what it does under the hood.

---

## 0. Prerequisites

Blender's own requirement applies to the host compiler used for the code
generators in step 3: **GCC 14 or newer, or Clang 17 or newer**. `env.sh` picks
the first of `gcc-15`, `gcc-14`, `clang-18`, `clang-17` it finds and falls back
to the system default, so nothing needs to become the system compiler. Ubuntu
22.04 ships GCC 11, too old:

```bash
sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test
sudo apt install -y gcc-14 g++-14
```

On Linux you also need the usual build tools plus `patchelf`, `meson` and
`git-lfs`, and a host Python 3.13 for the numpy cross-build, built from source
if the distribution has nothing newer than 3.12.

### macOS (Homebrew)

```bash
brew install openjdk cmake ninja meson pkgconf
brew install --cask android-commandlinetools     # sdkmanager, adb, aapt2…
brew install python@3.13                          # drives the numpy cross-build
```

Install the NDK + platform + build-tools:

```bash
export JAVA_HOME=/opt/homebrew/opt/openjdk/libexec/openjdk.jdk/Contents/Home
sdkmanager "ndk;28.2.13676358" "platform-tools" \
           "build-tools;35.0.1" "platforms;android-35"
```

The environment (NDK path, ABI, API levels) lives in
`build_files/android/env.sh` - everything below sources it.

---

## 1. Get the sources (LFS + submodule)

The GitHub repo is a mirror without LFS/submodule content - use
projects.blender.org:

```bash
git config lfs.url https://projects.blender.org/blender/blender.git/info/lfs
git lfs install --local && git lfs pull
git -c submodule.lib/macos_arm64.update=checkout submodule update --init lib/macos_arm64
```

`git lfs pull` asks for a username and password for projects.blender.org. The
content is public, so press Enter twice and it proceeds. Unattended runs have no
terminal to prompt at and hang silently instead, with no output and no traffic,
which is indistinguishable from a stalled download. For scripts, hand it empty
credentials up front:

```bash
git config credential.helper '!f() { echo username=; echo password=; }; f'
```


`lib/macos_arm64` is needed for the **native host tools** build (step 3).

---

## 2. Fetch the precompiled dependencies

```bash
git submodule update --init lib/android_arm64
```

The libraries come from the official Android port's prebuilt set, the same way
every other platform consumes `lib/<platform>`. There is nothing to compile.

They are built for API 29 and this port targets 31, which is the safe
direction. Packages such as the video codecs, sqlite and libffi are not
harvested separately: they are linked into ffmpeg, python and OIIO, matching
what `lib/linux_x64` does.

### Requirements that come with the prebuilt set

**NDK r30 beta1 is required, not merely preferred.** Around thirty of the
archives, including all of shaderc, SPIRV-Tools, LLVM, abseil and draco,
reference `std::__ndk1::__hash_memory`, a libc++ internal that NDK 28 does not
provide. An older NDK compiles everything and then fails at the final link with
undefined symbols. Install it and point the build at it:

```bash
export ANDROID_NDK_VERSION=30.0.14904198-beta1
```

The revision number matches the build id recorded in the libraries themselves,
visible with `llvm-readelf --notes` on any of their shared objects.

**Vulkan headers are not part of the set.** Every other platform gets them from
`lib/<platform>/vulkan`, and the NDK sysroot ships header version 275, which
predates `VK_KHR_dynamic_rendering_local_read` that GHOST uses unconditionally.
Provide a newer Vulkan-Headers checkout; `platform_android.cmake` looks for it
as `<LIBDIR>/vulkan/include` and then in `<BUILD_BASE>/lib/vulkan_headers`.

**ffmpeg is currently disabled.** The prebuilt `libavutil.a` and `libx265.a`
are not built with `-fPIC`, so they cannot be linked into `libblender.so`,
which an APK needs. Every other archive in the set links fine. Re-enable
`WITH_CODEC_FFMPEG` in `android_features_full.cmake` once they are rebuilt.

**CMake 3.26 or newer is required.** MaterialX refuses to configure with
anything older, and 3.x and 4.x disagree about whether `Python3_LIBRARY` reaches
the link line, which surfaces as undefined CPython symbols when USD links. The
recipes name that library explicitly, so either version now works. Ubuntu 22.04
ships 3.22, too old: use the Kitware packages or the upstream binary tarball.

Order matters (leaf → up). A full run, roughly:

```
zlib zstd deflate imath fmt tbb openexr png pugixml jpeg brotli freetype
harfbuzz webp tiff openjpeg expat yamlcpp blosc pystring minizipng opencolorio
opensubdiv robinmap openimageio embree alembic materialx potrace sqlite
libffi openssl lzma bzip2 python openvdb ogg vorbis theora opus lame aom
x265 vpx x264 ffmpeg xml2 eigen sse2neon fribidi abseil vulkan_headers
meshoptimizer shaderc numpy usd llvm rubberband
```

Each installs to `../blender_build_android/lib/android_arm64/<name>`; verify with:

```bash
$ANDROID_LLVM_BIN/llvm-objdump -f ../blender_build_android/lib/android_arm64/<name>/lib/lib*.so | grep aarch64
```

Notes / gotchas (all handled by build.sh):
- **Python** is a two-stage build (native host 3.13, then NDK cross). lzma and
  bzip2 must exist first or the interpreter ships without `_lzma` and `_bz2`,
  and nothing says so until Blender runs on the device. They are listed ahead
  of it above; the earlier order relied on rebuilding Python afterwards.
- **numpy** is cross-built with a meson cross-file + `_PYTHON_SYSCONFIGDATA_NAME`
  pointing at the target sysconfig. It needs a host python 3.13 matching the
  target version: Homebrew provides one on macOS, and on a distribution that
  ships something older it has to be built (see Prerequisites).
- **LLVM** builds host tablegen first, then cross.
- **USD** is built twice: once to bootstrap, then with Python support for the
  Blender `usd_hook`.

---

## 3. Build the native host codegen tools

Blender generates source at build time (makesdna, makesrna, shader_tool,
datatoc, msgfmt). These must run on the host, so build them natively with the
**same feature flags** as the target (else generated RNA/DNA mismatch):

```bash
cmake -S . -B ../blender_build_android/build_host_tools -G Ninja \
  -C build_files/android/android_features_full.cmake -DWITH_CROSSCOMPILED_TOOLS=OFF
ninja -C ../blender_build_android/build_host_tools makesdna makesrna datatoc msgfmt shader_tool
```

---

## 4. Configure + build Blender (arm64)

```bash
source build_files/android/env.sh
cmake -S . -B ../blender_build_android/build_android_blender -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_TOOLCHAIN_FILE" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-31
ninja -C ../blender_build_android/build_android_blender blender
```

Produces `../blender_build_android/build_android_blender/lib/libblender.so` (an arm64 shared library;
`platform_android.cmake` selects the shared-lib + NativeActivity path).

---

## 5. Package the APK

```bash
build_files/android/apk/package.sh
```

This gathers `libblender.so` + all transitive `.so` deps (stripped), bundles the
runtime payload (Python stdlib + numpy, `scripts/`, `datafiles/`) as
`blender_runtime.zip`, compiles `BlenderActivity`, and assembles a debug-signed
APK at `../blender_build_android/android_apk_stage_<cfg>/blender-<cfg>.apk`, sideloadable. Sizes:
**lite ≈ 115 MB**, **full ≈ 166 MB** (129 native libs vs 105).

On first launch `BlenderActivity` extracts the runtime to
`<filesDir>/blender/5.3/`, which is what `GHOST_SystemPathsAndroid` reports.

---

## 6. Install / run

**Sideload:** copy `blender.apk` to the device, allow "unknown sources", tap it.

**adb:**
```bash
adb install -r ../blender_build_android/android_apk_stage/blender.apk
adb shell am start -n org.blender.blender/.BlenderActivity
adb logcat --pid=$(adb shell pidof org.blender.blender)
```

**Android Studio (Run/Debug):** run `package.sh` once (to stage libs+assets),
then open `build_files/android/apk` as a project. The gradle app module consumes
`../blender_build_android/android_apk_stage/{lib,assets}` and can Run/Debug on a device or AVD.

---

## Variants

Variants are switches on top of a config, not extra configs.

### Validation layers

Bundles the Khronos validation layer into the APK. Blender must also be started
with `--debug-gpu` so it installs a debug messenger; messages then arrive in
logcat under the `gpu.vulkan` category rather than a `VALIDATION` tag.

```bash
build_files/android/build.py lite --validation --install
build_files/android/build.py --enable-validation-layers
adb logcat -s blender | grep -E "ERROR|WARNING"
build_files/android/build.py --disable-validation-layers   # when finished
```

Adds ~27 MB to the APK and slows the app noticeably, so it is a debugging build
only. The layer is downloaded once and cached in
`../blender_build_android/validation-layers/`.

Two messages are expected on a Vulkan 1.1 Adreno device and are not bugs:

- `WARNING-Swapchain-PreTransform` - the swap-chain deliberately requests an
  IDENTITY `preTransform` so the compositor performs the rotation; Blender does
  not pre-rotate its rendering.
- `Undefined-Value-ShaderOutputNotConsumed` - a depth-only pass whose fragment
  shader still declares a colour output. The write is discarded.

### Turnip (Mesa) driver

Loads Mesa's Turnip in place of the vendor driver through `libadrenotools`,
which provides `VK_KHR_dynamic_rendering` on devices whose vendor driver is
Vulkan 1.1 only, bypassing the render-pass fallback. Selected at runtime, so no
separate build is needed - but the Turnip driver must be present on the device.

```bash
build_files/android/build.py --enable-turnip
build_files/android/build.py --disable-turnip
```

The driver itself is not shipped: place a Turnip build at
`/data/data/org.blender.blender/files/turnip/vulkan.ad07xx.so`. The
`libadrenotools` hooks are bundled by `package.sh` automatically.

Status on Adreno 642L (as of Aug 2026): loads and renders correctly, and passes
the colour picker stress test, but crashes inside the driver
(`vulkan.ad07xx.so`, null dereference reached from command recording) while
cycling viewport shading modes. Treat it as experimental; the vendor driver is
the default and passes both suites.

---

## On-device debug switches

System properties, read once at startup.

| Property | Effect |
| --- | --- |
| `debug.blender.log` | Per-frame Vulkan submission and draw-lock tracing. Off by default: it costs several logcat lines per frame. |
| `debug.blender.turnip` | Load Mesa Turnip instead of the vendor driver. |
| `debug.blender.lowmem` | Force the low-memory device tier. |
| `debug.blender.renderdiv` | Render-scale divisor; `2` renders at half resolution. |

```bash
adb shell setprop debug.blender.log 1
```

### Shader caches

SPIR-V and pipeline caches live in the app's external files directory and
survive reinstalling, so they will mask shader changes. Clear them whenever
shaders or Vulkan code change:

```bash
build_files/android/build.py --clear-caches
```

---

## Gotchas

- **Host tools must match the target feature set.** Generated RNA/DNA encodes
  the enabled features, so each config keeps its own host-tools tree. Pointing
  one config at another's tools fails deep into the build with missing getters.
- **`apk/package.sh` deletes its staging directory on entry.** Anything copied
  in beforehand is discarded; that is why `--validation` injects the layer after
  packaging and re-signs, rather than staging it first.
- **Android requires unversioned sonames.** `package.sh` runs `patchelf` over
  the gathered libraries; a dependency arriving as `libfoo.so.1` will not load.
- **`android:debuggable` is `true`** in the manifest. Needed to attach
  validation layers, but wrong for a build handed to other people.

---

## Architecture recap

- GHOST backend: `intern/ghost/intern/GHOST_{System,Window}Android.*`,
  `GHOST_AndroidMain.cc` (NativeActivity `android_main` + inverted loop),
  Vulkan surface in `GHOST_ContextVK`.
- Platform glue: `build_files/cmake/platform/platform_android.cmake`,
  `build_files/android/android_features_full.cmake`.
- Dependencies: `lib/android_arm64` submodule (prebuilt, not built here).
- APK: `build_files/android/apk/`.
