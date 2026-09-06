# Building Blender for Android from scratch

End-to-end guide to reproduce the Android arm64 build. Developed on Linux
(including WSL) and on an Apple-Silicon Mac.
Target: Android 12+ (minSdk 31), built against Android 14 (targetSdk 34).

> The dependencies are not built here. They come from the `lib/android_arm64`
> submodule inside the repo, which is what CMake uses as `LIBDIR`, the same way
> every other platform consumes `lib/<platform>`.
>
> Everything the build produces goes into a sibling of the repo:
> `../blender_build_android/build_android_<cfg>` (Blender),
> `../blender_build_android/build_host_tools_<cfg>` (native codegen tools),
> `../blender_build_android/android_apk_stage_<cfg>` (APK stage), where `<cfg>`
> is `full` or `lite`. The only dependency living there is
> `../blender_build_android/lib/vulkan_headers`, supplied by hand because the
> prebuilt set predates the Vulkan headers GHOST needs.

## Build configurations

Two feature sets, selected with `-DBLENDER_ANDROID_CONFIG=full|lite` (default
`full`):

- **full** - everything: Cycles with Embree and denoising, USD, MaterialX,
  OpenVDB, Alembic, LLVM, ffmpeg, fluid and ocean simulation, motion tracking,
  the exact and manifold boolean solvers, Draco and PDF export.
- **lite** - modelling, sculpting, animation, EEVEE, Workbench, Python and the
  add-ons. Everything in the list above is off.

Note that the heavy features live in `android_features_full.cmake`, so a
feature added there does not reach lite. The two share the runtime payload
(scripts, assets, translations, Python), so lite is lighter in features rather
than dramatically smaller.

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
to the system default, so nothing needs to become the system compiler. It is
passed both to the standalone code generator build and to the `host_tools`
sub-build that runs inside the target build, which has no toolchain file of
its own. Ubuntu 22.04 ships GCC 11, too old:

```bash
sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test
sudo apt install -y gcc-14 g++-14
```

The rest is the same on either host. Versions matter in two places: the SDK
build-tools and platform are named in `apk/package.sh` (35.0.1 and android-35),
and the NDK is named in `env.sh`.

**The NDK must be r30.** The prebuilt libraries are compiled with clang 21 and
reference libc++ internals that r28 does not provide. An older NDK produces an
APK that installs and then dies in `dlopen` on the device.

### Linux

```bash
sudo apt install -y build-essential cmake ninja-build patchelf zip unzip \
                    git-lfs openjdk-17-jdk python3
```

CMake must be 3.26 or newer; Ubuntu 22.04 ships 3.22, so use the Kitware
packages or the upstream tarball there.

The SDK comes from the command line tools. Unpack them anywhere and let
`sdkmanager` fetch the rest:

```bash
export ANDROID_HOME="$HOME/android-sdk"
export JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64
"$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager" \
  "ndk;30.0.14904198-beta1" "platform-tools" \
  "build-tools;35.0.1" "platforms;android-35"
```

`env.sh` looks for the SDK in `$ANDROID_HOME`, falling back to
`$HOME/android-sdk`.

### macOS (Homebrew)

```bash
brew install openjdk cmake ninja pkgconf
brew install --cask android-commandlinetools     # sdkmanager, adb, aapt2…
```

```bash
export JAVA_HOME=/opt/homebrew/opt/openjdk/libexec/openjdk.jdk/Contents/Home
sdkmanager "ndk;30.0.14904198-beta1" "platform-tools" \
           "build-tools;35.0.1" "platforms;android-35"
```

The environment (NDK path, ABI, API levels) lives in
`build_files/android/env.sh` - everything below sources it.

---

## 1. Get the sources (LFS + submodule)

The GitHub repo is a mirror without LFS/submodule content - use
projects.blender.org:

The host prefix to fetch depends on the machine you build on: `lib/linux_x64`
on Linux, `lib/macos_arm64` on an Apple-Silicon Mac. It supplies the libraries
the native code generators link against in step 3.

```bash
git config lfs.url https://projects.blender.org/blender/blender.git/info/lfs
git lfs install --local && git lfs pull

HOST_LIB=lib/linux_x64          # lib/macos_arm64 on a Mac
git -c submodule.$HOST_LIB.update=checkout submodule update --init "$HOST_LIB"
```

`git lfs pull` asks for a username and password for projects.blender.org. The
content is public, so press Enter twice and it proceeds. Unattended runs have no
terminal to prompt at and hang silently instead, with no output and no traffic,
which is indistinguishable from a stalled download. For scripts, hand it empty
credentials up front:

```bash
git config --local credential.https://projects.blender.org.helper \
  '!f() { echo username=; echo password=; }; f'
```

Scope it to that host, as above. Setting it for the whole repository makes git
answer every server with empty credentials, including when pushing to your own
remote, where the failure reads as "No anonymous write access" and no password
prompt ever appears.


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

**NDK r30 beta1 is required, not merely preferred.** The archives are built
with clang 21 and reference `std::__ndk1::__hash_memory`, a libc++ internal
that NDK 28 does not provide. Whether that surfaces as a link error or as a
`dlopen` failure on the device depends on which library pulls it in, so an
older NDK can produce an APK that installs and then dies at startup.

`env.sh` defaults to it. Override only to test another:

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
The version to match is `VULKAN_VERSION` in
`build_files/build_environment/cmake/versions.cmake`:

```bash
VER=1.4.341
curl -fL -o /tmp/vh.tar.gz \
  https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v$VER.tar.gz
tar -xzf /tmp/vh.tar.gz -C /tmp
mkdir -p ../blender_build_android/lib/vulkan_headers
cp -R /tmp/Vulkan-Headers-$VER/include \
  ../blender_build_android/lib/vulkan_headers/include
```

**Symbol hiding is what makes the static codecs link.** The prefix is static,
so `platform_android.cmake` names every codec ffmpeg was built against and
applies `source/creator/symbols_unix.map` as a version script, the same as
every other Unix. Without it the link fails on symbols such as `aom_init`.
Anything the platform resolves by name has to stay listed there:
`ANativeActivity_onCreate` is looked up with `dlsym` and the app cannot start
without it.

**CMake 3.26 or newer is required.** MaterialX refuses to configure with
anything older, and 3.x and 4.x disagree about whether `Python3_LIBRARY` reaches
the link line, which surfaces as undefined CPython symbols when USD links. The
recipes name that library explicitly, so either version now works. Ubuntu 22.04
ships 3.22, too old: use the Kitware packages or the upstream binary tarball.

There is nothing to build here. The hand-rolled dependency builder this port
used previously has been retired in favour of the prebuilt set.

---

## 3. Build the native host codegen tools

Blender generates source at build time (makesdna, makesrna, shader_tool,
datatoc, msgfmt). These must run on the host, so build them natively with the
**same feature flags** as the target (else generated RNA/DNA mismatch):

The directory names matter: `package.sh` looks for msgfmt in
`build_host_tools_<cfg>` and for the binary in `build_android_<cfg>`. Use other
names and packaging silently ships no translations, or fails outright.

```bash
source build_files/android/env.sh

cmake -S . -B ../blender_build_android/build_host_tools_full -G Ninja \
  -C build_files/android/android_features_full.cmake \
  -DWITH_CROSSCOMPILED_TOOLS=OFF \
  -DCMAKE_C_COMPILER="$ANDROID_HOST_CC" -DCMAKE_CXX_COMPILER="$ANDROID_HOST_CXX" \
  -DWITH_HEADLESS=ON -DWITH_X11_XINPUT=OFF -DWITH_AUDASPACE=OFF \
  -DCMAKE_BUILD_RPATH="$PWD/lib/linux_x64/tbb/lib"
ninja -C ../blender_build_android/build_host_tools_full \
  makesdna makesrna datatoc msgfmt shader_tool
```

The compiler has to be named explicitly: this build gets no toolchain file and
would otherwise take the system default. `WITH_HEADLESS` keeps the GUI out, so
a Linux host does not need X11 development packages for tools that never open a
window. The rpath is for TBB, which the generators link against and would fail
to start without.

---

## 4. Configure + build Blender (arm64)

```bash
source build_files/android/env.sh
cmake -S . -B ../blender_build_android/build_android_full -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_TOOLCHAIN_FILE" \
  -DANDROID_ABI="$ANDROID_ABI" -DANDROID_PLATFORM="android-$ANDROID_API" \
  -DHOST_C_COMPILER="$ANDROID_HOST_CC" -DHOST_CXX_COMPILER="$ANDROID_HOST_CXX" \
  -DBLENDER_ANDROID_CONFIG=full
ninja -C ../blender_build_android/build_android_full blender
ninja -C ../blender_build_android/build_android_full \
  bf_intern_meshopt_bridge bf_intern_draco_bridge
```

`HOST_C_COMPILER` is separate from step 3: the target build spawns its own
`host_tools` sub-build, which also has no toolchain file. If it was configured
once with the wrong compiler, delete
`../blender_build_android/build_android_full/host_tools` before retrying, since
CMake will not change a compiler in an existing cache.

The two bridge targets are built by name because nothing links them; the glTF
add-on dlopens them for compressed meshes.

Produces `../blender_build_android/build_android_full/lib/libblender.so`, an
arm64 shared library: `platform_android.cmake` selects the shared library and
NativeActivity path.

---

## 5. Package the APK

```bash
build_files/android/apk/package.sh
```

This gathers `libblender.so` + all transitive `.so` deps (stripped), bundles the
runtime payload (Python stdlib + numpy, `scripts/`, `datafiles/`) as
`blender_runtime.zip`, compiles `BlenderActivity`, and assembles a debug-signed
APK at `../blender_build_android/android_apk_stage_<cfg>/blender-<cfg>.apk`,
sideloadable.
A `full` APK is around 272 MB with 147 native libraries; `lite` is far smaller.

On first launch `BlenderActivity` extracts the runtime to
`<filesDir>/blender/5.3/`, which is what `GHOST_SystemPathsAndroid` reports.

### What the payload carries

Beyond `scripts/` and `datafiles/`, `package.sh` adds several things that the
binary needs but the source tree does not place for it:

- **The essentials asset library** (`assets/`). Since 4.3 a brush is an asset,
  so without it sculpt, texture paint, vertex paint, weight paint, grease
  pencil and curves have no brushes at all, and the bundled node groups are
  missing.
- **Interface translations**, compiled from `locale/po` with Blender's own
  msgfmt. Skipped with a warning if the host tools were not built.
- **USD plugin resources**, without which the importers register nothing.
- **The glTF Draco and meshopt bridges**, built as explicit targets because
  nothing links them; the add-on dlopens them.
- **The Cycles add-on**, which lives outside `scripts/` and is normally placed
  by CMake's install step.
- **pip**, fetched by the host interpreter at packaging time and cached in
  `../blender_build_android/pip-<version>`. This is the only step that needs
  network access; without it the build still succeeds and prints a warning.

### Python on the device

Three pieces make the extension system work, and all three are easy to break:

- The **interpreter** ships as `libpython3_13_bin.so` in the native library
  directory. It cannot live in the payload: that lands in the app's data
  directory, which is mounted noexec from API 29 on. The `lib*.so` name is what
  makes the package manager extract it at all.
- `BlenderActivity` links it to `<python>/bin/`, sets `PYTHONHOME`, and writes
  a **`pyvenv.cfg`**. The last one is not optional: the extension system passes
  `-I`, which implies `-E` and ignores `PYTHONHOME`, and the interpreter then
  resolves the symlink back to a directory with no standard library and dies
  with "Failed to import encodings module".
- **Certificates.** Blender only defines `PYTHON_SSL_CERT_FILE` for a portable
  install, so nothing sets `SSL_CERT_FILE` here and OpenSSL looks in a path
  from the build machine. The activity exports it, and a generated
  `sitecustomize.py` loads the bundle explicitly, because on this platform
  setting the variable alone leaves an empty trust store.

Also note the runtime is re-extracted whenever the app's install time changes,
so reinstalling an APK during development picks up new scripts. It is keyed on
`PackageInfo.lastUpdateTime` rather than the version.

### Bundled extensions

Extensions are not part of the Blender source tree, and there is no practical
way to fetch them on the device yet. Anything placed in
`build_files/android/apk/extensions`, as a published `.zip` or an unpacked
directory, is copied into the payload and picked up by Blender's built-in
System repository with no preference changes. Override the source directory
with `BLENDER_ANDROID_EXTENSIONS_DIR`.

Everything bundled ships inside a GPL binary, so `package.sh` prints each
extension's declared license as it packs it.

---

## 6. Install / run

**Sideload:** copy `blender.apk` to the device, allow "unknown sources", tap it.

**adb:**
```bash
adb install -r ../blender_build_android/android_apk_stage_full/blender-full.apk
adb shell am start -n org.blender.blender/.BlenderActivity
adb logcat --pid=$(adb shell pidof org.blender.blender)
```

**Android Studio (Run/Debug):** run `package.sh` once (to stage libs+assets),
then open `build_files/android/apk` as a project. The gradle app module consumes
`../blender_build_android/android_apk_stage_<cfg>/{lib,assets}` and can Run/Debug on a device or AVD.

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
- **The manifest is not debuggable.** A distributed APK is therefore never
  debuggable by accident. Pass `--debuggable` to `build.py`, or set
  `BLENDER_ANDROID_DEBUGGABLE`, to attach a debugger or validation layers.

---

## Architecture recap

- GHOST backend: `intern/ghost/intern/GHOST_{System,Window}Android.*`,
  `GHOST_AndroidMain.cc` (NativeActivity `android_main` + inverted loop),
  Vulkan surface in `GHOST_ContextVK`.
- Platform glue: `build_files/cmake/platform/platform_android.cmake`,
  `build_files/android/android_features_full.cmake`.
- Dependencies: `lib/android_arm64` submodule (prebuilt, not built here).
- APK: `build_files/android/apk/`.
