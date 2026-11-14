# Fork notes — Blender for Android

This repository is a fork of [simfeo/blender](https://github.com/simfeo/blender), which ports
Blender 5.3 to Android (arm64, Vulkan, NativeActivity). It keeps that port's history and adds the
changes below.

## Tested hardware

**One device: Samsung Galaxy Tab S9+** (SM-X810 / SM-X816B), Snapdragon 8 Gen 2, Adreno 740,
Vulkan 1.3, 12 GB, S Pen, DeX.

| Path | Status |
| --- | --- |
| Adreno 740, Vulkan 1.3, dynamic rendering | **tested** |
| Vulkan 1.1 / render-pass fallback | **not tested on real 1.1 hardware** (exercised only through the switch below) |
| Mali GPUs | **not tested** |
| Low-memory devices (<6 GiB) | **not tested** |
| Vulkan 1.1 devices with 16 KB page size | **not tested** |

Claims about the other devices in [build_files/android/README.md](build_files/android/README.md)
come from the original port's author and have not been reproduced with the changes here.

## Changes

### 1. Text input from a hardware keyboard

A docked keyboard could drive shortcuts but could not type a single character into a text field.

- `intern/ghost/intern/GHOST_SystemAndroid.cc`
  - `handleKeyEvent` now fills `GHOST_EventKey::utf8_buf`, which is the only thing Blender's text
    fields insert from (`interface_handlers.cc`). The character is resolved by the platform
    (`KeyEvent.getUnicodeChar`) so the device's keyboard layout applies; the NDK has no
    equivalent API and a table in C would hard-code a US layout.
  - `convertAndroidKey` gained F1–F12, punctuation (`[ ] \ ; ' , . / - = ` `), the numpad,
    Home/End/PageUp/PageDown, Insert, PrintScreen and the menu key. Keys not in the table are
    still ignored, so nothing new can fire a shortcut by accident.
  - ASCII is not emitted as text while Ctrl or Alt is held: those are command chords in a text
    field, and emitting the letter as well would type `c` on every copy. Non-ASCII is still
    emitted, because AltGr characters arrive as Ctrl+Alt on several layouts.
- `build_files/android/apk/app/src/main/java/org/blender/blender/BlenderActivity.java`
  - New `unicodeChar(int keyCode, int metaState)` static, called by the native side.

Verification on device: focus any text field (rename an object with F2, the operator search with
F3, a console line), type ASCII and layout-specific characters, then check `Ctrl+Z`/`Ctrl+C`
still undo and copy rather than typing letters.

### 2. `debug.blender.renderpass` — force the Vulkan render-pass fallback

Devices with `VK_KHR_dynamic_rendering` and devices without it run different code through the
render graph, and a single tablet can only exercise one of them. This property makes the other
path reachable:

```bash
adb shell setprop debug.blender.renderpass 1   # emulate a Vulkan 1.1 device (render pass fallback)
adb shell setprop debug.blender.renderpass 0   # force dynamic rendering
adb shell setprop debug.blender.renderpass ""  # follow the driver (default)
```

- `source/blender/gpu/vulkan/vk_backend.cc` — the flag is forced where extensions are detected,
  before anything reads it, so command buffers, pipeline builds, pipeline libraries and local
  read all agree on the fallback. Forcing it later, per command buffer, would leave pipelines
  built for dynamic rendering and used with a classic render pass.

Verification on device:

```bash
adb shell setprop debug.blender.log 1
adb shell setprop debug.blender.renderpass 1
# relaunch, then:
adb logcat -s blender-rpfb   # render pass / framebuffer cache lines appear when the fallback runs
```

The device still enables the Vulkan 1.3 feature; it is simply not used while the switch is on.

### 3. The session survives being backgrounded

Android can reclaim a backgrounded app at any moment: no dialog, no exit handler. On a tablet that
is the normal way a session ended — switch to another app, come back, the work is gone.

- `intern/ghost/intern/GHOST_AndroidMain.cc` — `APP_CMD_PAUSE` writes the auto-save through a new
  `WM_autosave_write_background()` and stops driving Blender; the frame loop blocks on the event
  queue behind a destroyed surface instead of spinning. Rate limited to once per 10 s, because
  switching between two apps fires PAUSE/RESUME repeatedly.
- `APP_CMD_LOW_MEMORY` releases the render-result and compositor GPU caches (the parts that can be
  rebuilt) rather than waiting to be killed.
- `source/blender/windowmanager/{WM_api.hh,intern/wm_files.cc}` — the auto-save entry point.

Recovery works the way Blender's own does: the next start offers to restore the auto-save.

### 4. Long renders keep the screen on and the process in the foreground

A render can outlast the screen timeout, and once the screen goes off Android is free to suspend
or reclaim the process, which killed long renders with nothing in the log.

- `source/blender/render/intern/pipeline.cc` — `RE_RenderFrame` and `RE_RenderAnim` are bracketed
  by an RAII guard that calls `GHOST_android_set_busy()`. An animation render nests (loop plus
  each frame), so the platform counts depth and only crosses the boundary once.
- `intern/ghost/intern/GHOST_SystemAndroid.cc` — the busy counter and the JNI call.
- `BlenderActivity.setBusy()` — sets `FLAG_KEEP_SCREEN_ON` and runs `BlenderRenderService`, a
  foreground service with a notification, for the duration.

The screen flag is the part that matters; the service is what stops the process being reclaimed
when the user walks away. `dataSync` is the closest declared foreground type for a long local job
and needs `FOREGROUND_SERVICE_DATA_SYNC` on Android 14, both declared in the manifest.

### 5. First launch no longer raises the "not responding" dialog

The runtime payload (Python, scripts, datafiles) is over a hundred megabytes to unpack, and it was
unpacked in `onCreate`, on the UI thread — on the one launch every new user sees.

- `BlenderActivity.prepareRuntimeAsync()` unpacks on a background thread and publishes the path of
  a ready marker (`BLENDER_ANDROID_READY_FILE`) before `super.onCreate()` starts native.
- `GHOST_AndroidMain.cc` waits for that marker before it starts Blender. This is not the UI thread,
  so waiting is free; without the variable it starts at once, as before.
- The marker is written even when preparation failed, so a broken extraction becomes Python
  reporting missing files instead of a splash screen that never ends.

## Verification status

None of this has been built into an APK or run on a device yet. What was checked while writing it:

- Every changed C++ translation unit that could be preprocessed outside a full build passes
  `clang -fsyntax-only` against the NDK r30 sysroot for `aarch64-linux-android31`, C++20:
  `GHOST_SystemAndroid.cc`, `GHOST_AndroidMain.cc` and `vk_backend.cc` (the last two with
  `-Wall -Wextra`). `wm_files.cc` and `pipeline.cc` could not be checked that way because they
  include headers generated by the build (RNA prototypes); their changes only call existing
  functions with existing types.
- Both Java sources compile with `javac -source 17` against `platforms/android-35/android.jar`.
- The manifest parses, and the service and its permissions are declared.

## Not done yet

The full gap list and the on-device test plan live in the tracking document outside this
repository (`blender-android-roadmap.md`). The next items, in order:

1. Extension installation: the extension framework spawns `cli/blender_ext.py` through
   `subprocess`, which does not work on Android, so extensions can only be bundled at packaging
   time.
2. SAF (Storage Access Framework) so `MANAGE_EXTERNAL_STORAGE` is not required.
3. Splitting the interface into two regions on one window, which is the only way a single native
   window can show a reference image beside the model.
4. The first-launch screen shows the splash only. A determinate progress indicator needs a launch
   activity in front of `NativeActivity`.

## Build notes for this fork

Unchanged from [build_files/android/BUILDING.md](build_files/android/BUILDING.md), with two
practical notes from building it on macOS:

- The SDK ships NDK r30 as `ndk;30.0.14904198`, while `build_files/android/env.sh` looks for a
  directory named `30.0.14904198-beta1`. Either symlink it or export
  `ANDROID_NDK_VERSION=30.0.14904198` before building.
- `platforms;android-35` may be installed under an unexpected directory name by the Homebrew
  command line tools; `sdkmanager` then reports it as "inconsistent location". Move it to
  `$ANDROID_HOME/platforms/android-35` and `sdkmanager "platforms;android-35"` sees it again.
