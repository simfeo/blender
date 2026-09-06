# Blender on Android

Cross-compiled arm64 port. Builds a complete `libblender.so`, packages it as an
installable APK, and runs on device.

Target: Android 12 or newer (minSdk 31), built against Android 14
(targetSdk 34), `arm64-v8a` only. Rendering is Vulkan; devices reporting only Vulkan 1.1 are
supported through a render pass fallback.

Tested on a Galaxy Tab S7 FE (SM-T733, Adreno 642L) and a Galaxy S24 Ultra
(Adreno 750).

To build one, read `BUILDING.md`. This file describes what is here and what
works.

## Layout

| Path | Contents |
| --- | --- |
| `env.sh` | SDK and NDK locations, ABI, API levels, host compiler choice |
| `build_apk.sh` | Host tools, cross-compile, package, end to end |
| `build.py` | Wrapper adding install, run, variants and cache clearing |
| `android_features_{common,full,lite}.cmake` | Feature sets |
| `apk/` | Manifest, `BlenderActivity`, `package.sh`, bundled extensions |
| `compat/` | `std::atomic_ref` polyfill the NDK libc++ lacks |
| `toolchain_test/` | Minimal project to prove the toolchain works |

The GHOST backend lives in `intern/ghost/intern/GHOST_*Android.*`, and the CMake
platform glue in `build_files/cmake/platform/platform_android.cmake`.

## What works

- Cycles with Embree, USD, MaterialX, OpenVDB, Alembic, LLVM, ffmpeg video
- OpenImageDenoise, fluid and ocean simulation, both boolean solvers, motion
  tracking, glTF with Draco, PDF export, path guiding
- Python, the bundled add-ons, and extensions baked into the APK
- Touch: one finger drags panels and pans a 2D region, three fingers orbit the
  viewport, two fingers scroll and zoom
- S Pen pressure and tilt, side button as right mouse
- On-screen keyboard, summoned automatically at text fields or from the status
  bar button

## Device compatibility

Two feature sets exist because mobile hardware varies enormously. `full`
targets flagship devices. `lite` keeps modelling, sculpting, animation, EEVEE
and Workbench, Python and the add-ons, and drops Cycles and its denoising,
video, USD, Alembic, OpenVDB, MaterialX, LLVM, fluid and ocean simulation,
motion tracking, the exact and manifold boolean solvers, Draco glTF and PDF
export.

The two share a runtime payload, so lite is lighter in features rather than
dramatically smaller on disk.

Beyond that, several limits are applied on Android specifically:

- Texture size is capped at 1024 unless the preference says otherwise. Without
  it a scene carrying a dozen 4K maps is killed by the system with nothing in
  the log, because GPU allocations do not appear in the process RSS.
- The shadow pool is capped on devices with less than 6GB of RAM.
- The reflection probe workgroup is reduced from 32 to 16, which some Adreno
  parts require even though they report the larger size as supported.
- GPU subdivision is probed at startup and falls back to the CPU if refused.
- Compute pipeline and pipeline library failures on Adreno are worked around
  rather than fatal.

## Interface defaults

A tablet is held closer than a monitor and has less room, so a fresh
preferences file starts at a 1.1 interface scale with wider editor borders, and
opens renders, the file browser and the preferences as maximized areas rather
than new windows, which a single native window cannot provide.

Stylus pressure starts at a 0.5 maximum threshold. Android normalises pressure
against the range the digitiser declares rather than the range a hand reaches,
and a stylus pressed hard on glass tops out near 0.77, so the desktop default of
1.0 could never be reached.

All of these are ordinary preferences; only the starting point differs.

## Status

Working: toolchain, GHOST backend, inverted main loop, input, prebuilt
dependency set, APK packaging, install and run on device.

Known gaps:

- The runtime payload is extracted on first launch, which takes long enough to
  draw a system "not responding" dialog. There is no progress screen yet.
- Extensions cannot be installed on device; they have to be bundled at
  packaging time. See `BUILDING.md`.
- Turnip is experimental. The vendor driver is the default.
