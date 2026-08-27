# Blender on Android — cross-compile toolchain

Work-in-progress port. Target device: **Samsung Galaxy Tab S8 FE**
(Exynos 1380, arm64, Vulkan 1.1+, S Pen with side button → right-mouse).

This directory holds the NDK cross-compile plumbing. It does **not** yet build
the full `blender` binary — that needs the third-party dependency stack ported to
Android (the large remaining task; see `../../ANDROID_PORT_SPEC.md` §6).

## Installed toolchain (macOS / Homebrew)

```bash
brew install openjdk                              # JDK (keg-only, no sudo)
brew install --cask android-commandlinetools      # sdkmanager, adb wrappers
sdkmanager "ndk;28.2.13676358" "platform-tools" \
           "build-tools;35.0.1" "platforms;android-35"
```

- SDK root: `/opt/homebrew/share/android-commandlinetools`
- NDK:      `.../ndk/28.2.13676358` (r28c, clang 19)
- The NDK host binaries are `darwin-x86_64`; on Apple Silicon they run under
  Rosetta 2 (`softwareupdate --install-rosetta` if not already present).

## Environment

All knobs live in `env.sh` (ABI, min/target API, paths). Source it:

```bash
source build_files/android/env.sh
```

Defaults: `ABI=arm64-v8a`, `min API=28` (Android 9), `target API=35`.

## Verify the toolchain (milestone 1 — done)

```bash
build_files/android/configure.sh test
```

Builds `build_files/android/toolchain_test/` into `../blender_build_android/build_android_toolchain_test/`:
an arm64 Android executable plus a shared library linked against `liblog` and
`libandroid` — the system libs the GHOST backend needs (`ANativeWindow`, logging).
This proves CMake → NDK → Ninja works before any dependency work.

## Roadmap

1. ✅ NDK toolchain installed + CMake cross-compile verified.
2. ✅ Android surface case in `GHOST_ContextVK` (`VK_KHR_android_surface`,
   `ANativeWindow*`).
3. ✅ Scaffold `GHOST_SystemAndroid` / `GHOST_WindowAndroid` + the `ANDROID`
   branch in `intern/ghost/CMakeLists.txt` (gated by `WITH_GHOST_ANDROID`).
4. ✅ Invert the main loop: `android_main` (NativeActivity glue) runs init once
   then `WM_main_loop_body(C)` per frame. `WM_main` split into
   `WM_main_entry` + `WM_main_loop_body`; creator seam via `GHOST_android_launch`.
5. ✅ Input: touch/stylus/keys → GHOST events. Single pointer → cursor+button;
   two-finger → scroll+magnify trackpad; S Pen pressure/tilt → tablet data,
   side button → right mouse; hardware keys mapped. Soft keyboard is shown/hidden
   at text-edit begin/end via base GHOST `popupOnScreenKeyboard`/`hideOnScreenKeyboard`
   hooks (no-op on desktop). Soft-keyboard text is delivered via a Java
   `BlenderActivity` (NativeActivity subclass) with a focusable proxy view whose
   InputConnection forwards committed text/keys to native over JNI.
5b. ✅ Backend stubs implemented: DPI from `AConfiguration`, modifier/button/cursor
   state from events, headless Vulkan offscreen context, `GHOST_SystemPathsAndroid`,
   clipboard via JNI `ClipboardManager`, surface-resize event on recreate.
   App is landscape-only. No backend stubs remain.
   Note: `BlenderActivity.java` needs a Java build step in packaging (step 7);
   soft-keyboard typing only activates once the app runs as that activity.
6. ✅ Third-party dependencies for `android-arm64`, taken prebuilt from the
   official Android port via the `lib/android_arm64` submodule. The hand built
   deps builder this port used before has been retired.
7. ⬜ APK/AAB packaging (NativeActivity manifest) + `adb install` to the tablet.

## Deploy (once there is an APK)

```bash
adb devices          # tablet in Developer Mode + USB debugging
adb install app.apk
```
