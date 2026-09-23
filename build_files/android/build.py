#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build, package and deploy Blender for Android.

Wraps the shell scripts in this directory so the four usable builds are one
command each:

    ./build.py lite                     # lite APK
    ./build.py full --install --run     # full APK onto the connected device
    ./build.py lite --validation        # + Khronos validation layer
    ./build.py --enable-turnip          # run on Mesa Turnip (no rebuild)

Run with --help for the rest.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
BUILD_BASE = REPO_ROOT.parent / "blender_build_android"

PACKAGE = "org.blender.blender"
ACTIVITY = f"{PACKAGE}/.BlenderActivity"

# Pinned so a validation run is reproducible; bump deliberately.
VVL_VERSION = "1.4.357.0"
VVL_URL = (
    "https://github.com/KhronosGroup/Vulkan-ValidationLayers/releases/download/"
    f"vulkan-sdk-{VVL_VERSION}/android-binaries-{VVL_VERSION}.zip"
)
VVL_SO = "libVkLayer_khronos_validation.so"

# Caches that survive reinstall and will happily serve stale shaders.
DEVICE_CACHES = (
    "vk-spirv-cache-vk11",
    "vk-spirv-cache-vk12",
    "vk-pipeline-cache",
)


def run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess:
    print("+ " + " ".join(str(c) for c in cmd), flush=True)
    return subprocess.run(cmd, check=True, **kwargs)


def tool_env() -> dict[str, str]:
    """env.sh settings merged over the caller's environment.

    zipalign and apksigner are wrapper scripts that need JAVA_HOME and the SDK
    on PATH, so they cannot inherit a bare environment.
    """
    merged = dict(os.environ)
    merged.update(env_from_env_sh())
    return merged


def sh(script: str) -> None:
    """Run a snippet through the login shell so env.sh resolves the SDK."""
    run(["bash", "-lc", script])


def env_from_env_sh() -> dict[str, str]:
    """env.sh owns the SDK/NDK locations; read them rather than guessing."""
    out = subprocess.run(
        ["bash", "-c", f"unset ANDROID_HOME; source '{SCRIPT_DIR / 'env.sh'}' >/dev/null && env"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    return dict(
        line.split("=", 1) for line in out.splitlines() if "=" in line
    )


def adb(args: list[str], serial: str | None, **kwargs) -> subprocess.CompletedProcess:
    cmd = ["adb"] + (["-s", serial] if serial else []) + args
    return run(cmd, **kwargs)


def flavour(turnip: bool) -> str:
    return "-turnip" if turnip else ""


def build_dir(config: str, turnip: bool = False) -> Path:
    return BUILD_BASE / f"build_android_{config}{flavour(turnip)}"


def stage_dir(config: str, turnip: bool = False) -> Path:
    return BUILD_BASE / f"android_apk_stage_{config}{flavour(turnip)}"


def apk_path(config: str, turnip: bool = False) -> Path:
    return stage_dir(config, turnip) / f"blender-{config}{flavour(turnip)}.apk"


def clean(config: str, turnip: bool = False) -> None:
    for path in (
        build_dir(config, turnip),
        BUILD_BASE / f"build_host_tools_{config}",
        stage_dir(config, turnip),
    ):
        if path.exists():
            print(f"removing {path}")
            shutil.rmtree(path)


def pending_edges(config: str, turnip: bool = False) -> int:
    """How much ninja thinks is stale, so a surprise full rebuild is visible."""
    tree = build_dir(config, turnip)
    if not (tree / "build.ninja").exists():
        return -1
    try:
        out = subprocess.run(["bash", "-lc", f"ninja -C '{tree}' -n blender"],
                             check=True, capture_output=True, text=True).stdout
    except subprocess.CalledProcessError:
        return -1
    return sum(1 for line in out.splitlines() if "Building" in line or "Linking" in line)


def build(config: str, repackage: bool = False, debuggable: bool = False,
          turnip: bool = False) -> None:
    """Recompile, and repackage only as much as the change requires.

    Three paths, cheapest first. Only libblender.so usually changes, so reusing
    the staged runtime payload saves rebuilding a 61 MB asset zip and
    re-gathering every native library. Debuggability and the bundled layer live
    in the APK, not in the compiled code, so they need repackaging but never a
    reconfigure -- forcing one used to turn a flag change into a full rebuild.
    """
    stage = stage_dir(config, turnip)
    tree = build_dir(config, turnip)
    configured = (tree / "build.ninja").exists()
    staged = (stage / "base.apk").exists() and (stage / "lib/arm64-v8a/libblender.so").exists()
    env = "BLENDER_ANDROID_DEBUGGABLE=1 " if debuggable else ""
    if turnip:
        # Always repackage: fastdeploy swaps only libblender.so, not the driver.
        env += f"BLENDER_ANDROID_TURNIP=1 BLENDER_ANDROID_FLAVOUR={flavour(True)} "
        repackage = True

    stale = pending_edges(config, turnip)
    if stale >= 0:
        print(f"[build] {stale} ninja edge(s) stale")
        if stale > 500:
            print(f"[build] WARNING: that is a large rebuild for an incremental change. "
                  f"A corrupt {tree}/.ninja_deps forces this; delete it to reset.")

    if not configured:
        print("[build] path: full (configure + compile + package)")
        sh(f"{env}'{SCRIPT_DIR / 'build_apk.sh'}' {config}")
    elif repackage or not staged:
        print("[build] path: compile + repackage (no reconfigure)")
        # env.sh, not just ninja: the host code generators run during this build
        # and need it to find their libraries.
        sh(f"source '{SCRIPT_DIR / 'env.sh'}' >/dev/null && ninja -C '{tree}' blender")
        sh(f"{env}BLENDER_ANDROID_CONFIG={config} BUILD='{tree}' "
           f"bash '{SCRIPT_DIR / 'apk' / 'package.sh'}'")
    else:
        print("[build] path: fast (compile + swap libblender.so)")
        # fastdeploy installs and launches at the end; build.py owns that.
        sh(f"{env}FASTDEPLOY_NO_INSTALL=1 '{SCRIPT_DIR / 'fastdeploy.sh'}' {config}")


def build_turnip_deps() -> dict[str, str]:
    """Download and build libadrenotools and Turnip; return the paths to use."""
    out = subprocess.run(["bash", str(SCRIPT_DIR / "build_turnip.sh")],
                         check=True, stdout=subprocess.PIPE, text=True).stdout
    return dict(line.split("=", 1) for line in out.splitlines()
                if line.startswith("BLENDER_ANDROID_"))


def uses_oidn(config: str) -> bool:
    features = SCRIPT_DIR / f"android_features_{config}.cmake"
    return "set(WITH_OPENIMAGEDENOISE ON" in features.read_text()


def build_oidn() -> str:
    """Build static OpenImageDenoise; return its install prefix."""
    out = subprocess.run(["bash", str(SCRIPT_DIR / "build_oidn.sh")],
                         check=True, stdout=subprocess.PIPE, text=True).stdout
    return dict(line.split("=", 1) for line in out.splitlines()
                if line.startswith("BLENDER_ANDROID_"))["BLENDER_ANDROID_OIDN_ROOT"]


def point_tree_at_oidn(config: str, turnip: bool, root: str) -> None:
    """An already configured tree only reruns ninja, so hand it the root here."""
    cache = build_dir(config, turnip) / "CMakeCache.txt"
    if not cache.exists() or f"ANDROID_OIDN_ROOT:UNINITIALIZED={root}\n" in cache.read_text():
        return
    sh(f"source '{SCRIPT_DIR / 'env.sh'}' >/dev/null && "
       f"cmake -B '{build_dir(config, turnip)}' -DANDROID_OIDN_ROOT='{root}'")


def fetch_validation_layer() -> Path:
    """Return a local copy of the arm64 validation layer, downloading once."""
    cache = BUILD_BASE / "validation-layers" / VVL_VERSION
    layer = cache / VVL_SO
    if layer.exists():
        return layer

    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / "android-binaries.zip"
    print(f"downloading {VVL_URL}")
    urllib.request.urlretrieve(VVL_URL, archive)
    with zipfile.ZipFile(archive) as zf:
        member = next(
            name for name in zf.namelist() if name.endswith(f"arm64-v8a/{VVL_SO}")
        )
        with zf.open(member) as src, open(layer, "wb") as dst:
            shutil.copyfileobj(src, dst)
    archive.unlink()
    print(f"validation layer -> {layer}")
    return layer


def inject_validation_layer(config: str, turnip: bool = False) -> None:
    """Add the layer to an already-built APK and re-sign it.

    package.sh wipes its staging directory on entry, so the layer cannot be
    staged beforehand: it has to go in afterwards.
    """
    env = tool_env()
    build_tools = Path(env["ANDROID_HOME"]) / "build-tools" / "35.0.1"
    keystore = BUILD_BASE / "android-debug.keystore"
    stage = stage_dir(config, turnip)
    apk = apk_path(config, turnip)

    shutil.copy2(fetch_validation_layer(), stage / "lib" / "arm64-v8a" / VVL_SO)
    run(["zip", "-q", str(apk), f"lib/arm64-v8a/{VVL_SO}"], cwd=stage)

    aligned = stage / "aligned.apk"
    run([str(build_tools / "zipalign"), "-f", "-p", "4", str(apk), str(aligned)], env=env)
    aligned.replace(apk)
    run([
        str(build_tools / "apksigner"), "sign",
        "--ks", str(keystore),
        "--ks-pass", "pass:android",
        "--key-pass", "pass:android",
        str(apk),
    ], env=env)
    print("validation layer bundled; enable it with --enable-validation-layers")


def set_validation_layers(enabled: bool, serial: str | None) -> None:
    settings = {
        "enable_gpu_debug_layers": "1" if enabled else "0",
        "gpu_debug_app": PACKAGE if enabled else "null",
        "gpu_debug_layers": "VK_LAYER_KHRONOS_validation" if enabled else "null",
    }
    for key, value in settings.items():
        adb(["shell", "settings", "put", "global", key, value], serial)
    print(f"validation layers {'enabled' if enabled else 'disabled'}")


def set_property(name: str, value: str, serial: str | None) -> None:
    adb(["shell", "setprop", name, value], serial)
    print(f"{name} = {value}")


def clear_caches(serial: str | None) -> None:
    base = f"/sdcard/Android/data/{PACKAGE}/files/blender"
    adb(["shell", "rm", "-rf"] + [f"{base}/{name}" for name in DEVICE_CACHES], serial)
    print("shader and pipeline caches cleared")


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("config", nargs="?", choices=("lite", "full"),
                        help="feature set to build; omit for device-only actions")
    parser.add_argument("--clean", action="store_true",
                        help="wipe this config's build trees first")
    parser.add_argument("--repackage", action="store_true",
                        help="rebuild the runtime payload too; needed when scripts, "
                             "datafiles or the dependency set change")
    parser.add_argument("--validation", action="store_true",
                        help="bundle the Khronos validation layer into the APK; implies "
                             "--debuggable, which the layer loader requires")
    parser.add_argument("--debuggable", action="store_true",
                        help="mark the APK debuggable; never do this for a release")
    parser.add_argument("--install", action="store_true", help="adb install the APK")
    parser.add_argument("--run", action="store_true", help="launch after installing")
    parser.add_argument("--clear-caches", action="store_true",
                        help="drop the on-device shader and pipeline caches")
    parser.add_argument("--enable-validation-layers", action="store_true")
    parser.add_argument("--disable-validation-layers", action="store_true")
    parser.add_argument("--turnip", action="store_true",
                        help="bundle Mesa Turnip in the APK; its presence is what "
                             "selects the driver at runtime. Downloads and builds "
                             "Turnip and libadrenotools unless the environment "
                             "names them")
    parser.add_argument("--enable-turnip", action="store_true",
                        help="run on Mesa Turnip instead of the vendor driver")
    parser.add_argument("--disable-turnip", action="store_true")
    parser.add_argument("--verbose-log", action="store_true",
                        help="per-frame Vulkan tracing (debug.blender.log)")
    parser.add_argument("-s", "--serial", help="adb device serial")
    args = parser.parse_args()

    if args.config:
        # Resolved before compiling so a failed dependency build costs nothing:
        # packaging runs after a compile that can take an hour.
        if args.turnip:
            missing = [name for name in ("BLENDER_ANDROID_ADRENOTOOLS",
                                         "BLENDER_ANDROID_TURNIP_DRIVER")
                       if not os.environ.get(name)]
            if missing:
                print("[turnip] " + " and ".join(missing) + " not set; "
                      "downloading and building them", flush=True)
                built = build_turnip_deps()
                for name in missing:
                    os.environ[name] = built[name]
        if args.clean:
            clean(args.config, args.turnip)
        if uses_oidn(args.config) and not os.environ.get("BLENDER_ANDROID_OIDN_ROOT"):
            os.environ["BLENDER_ANDROID_OIDN_ROOT"] = build_oidn()
        if os.environ.get("BLENDER_ANDROID_OIDN_ROOT"):
            point_tree_at_oidn(args.config, args.turnip, os.environ["BLENDER_ANDROID_OIDN_ROOT"])
        # A validation build has to be repackaged: base.apk carries the manifest.
        debuggable = args.validation or args.debuggable
        build(args.config,
              repackage=args.repackage or args.clean or debuggable,
              debuggable=debuggable,
              turnip=args.turnip)
        if args.validation:
            inject_validation_layer(args.config, args.turnip)
        print(f"APK: {apk_path(args.config, args.turnip)}")

    if args.install:
        if not args.config:
            parser.error("--install needs a config")
        adb(["install", "-r", str(apk_path(args.config, args.turnip))], args.serial)

    if args.clear_caches:
        clear_caches(args.serial)
    if args.enable_validation_layers:
        set_validation_layers(True, args.serial)
    if args.disable_validation_layers:
        set_validation_layers(False, args.serial)
    if args.enable_turnip:
        set_property("debug.blender.turnip", "1", args.serial)
    if args.disable_turnip:
        set_property("debug.blender.turnip", "0", args.serial)
    if args.verbose_log:
        set_property("debug.blender.log", "1", args.serial)

    if args.run:
        adb(["shell", "am", "force-stop", PACKAGE], args.serial)
        adb(["shell", "am", "start", "-n", ACTIVITY], args.serial)

    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as ex:
        print(f"\nfailed: {' '.join(str(c) for c in ex.cmd)}", file=sys.stderr)
        sys.exit(ex.returncode)
