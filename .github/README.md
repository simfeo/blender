<!--
Keep this document short & concise,
linking to external resources instead of including content in-line.
See 'release/text/readme.html' for the end user read-me.
-->

Blender for Android (fork)
==========================

**A fork of [simfeo/blender](https://github.com/simfeo/blender), which ports Blender to Android
(arm64, Vulkan).** This fork continues that work; it is not affiliated with Blender or with the
port's original authors.

What this fork adds on top of the port:

- **Text input from a hardware keyboard.** A docked keyboard could run shortcuts but could not
  put a single character into a text field: the Android backend never filled the UTF-8 buffer
  that Blender inserts text from. Characters now come from the platform's own keyboard layout,
  and F1–F12, punctuation, the numpad and Home/End/PageUp/PageDown are mapped.
- **`debug.blender.renderpass`**, which forces the Vulkan render-pass fallback on a device that
  supports dynamic rendering. Without it the fallback path — the one every Vulkan 1.1 tablet
  depends on — cannot be exercised on a newer device at all.

**Testing status: one device only.** Everything here has been tested on a **Samsung Galaxy Tab S9+**
(SM-X810 / SM-X816B, Snapdragon 8 Gen 2, Adreno 740, Vulkan 1.3, 12 GB, S Pen, DeX). No other
device has been tried. In particular **Vulkan 1.1 devices, Mali GPUs and low-memory (<6 GiB)
devices are untested** by this fork, even though the code paths for them exist and are forced by
the switch above.

- Building from source: [build_files/android/BUILDING.md](../build_files/android/BUILDING.md)
- What changed here and how to verify it: [FORK_NOTES.md](../FORK_NOTES.md)
- Upstream APKs from the original port: [simfeo/blender Releases](https://github.com/simfeo/blender/releases)

The rest of this file is the port's read-me, and below the horizontal rule is upstream Blender's.

---

**The original port** (unofficial, arm64, Vulkan):

- Website, which APK to pick, device requirements: [blenderandroid.github.io](https://blenderandroid.github.io)
- Ready-made APKs: [Releases](https://github.com/simfeo/blender/releases)
- Building from source: [build_files/android/BUILDING.md](../build_files/android/BUILDING.md)
- Tested by the original author on a Samsung Galaxy Tab S7 FE (Adreno 642L, Vulkan 1.1) and a
  Galaxy S22 Ultra SM-S908B (Xclipse 920, Vulkan 1.3). These results do not transfer to this fork.

> [!IMPORTANT]
> Cloning from this [GitHub mirror](https://github.com/blender/blender) may cause Git LFS errors. To avoid this, use `GIT_LFS_SKIP_SMUDGE=1` when doing your initial clone.  
> See [the documentation](https://developer.blender.org/docs/handbook/contributing/using_git/#github-mirror) for full instructions.

Blender

=======

Blender is the free and open source 3D creation suite.
It supports the entirety of the 3D pipeline—modeling, rigging, animation, simulation, rendering, compositing,
motion tracking and video editing.

![Blender screenshot](https://code.blender.org/wp-content/uploads/2018/12/springrg.jpg "Blender screenshot")

Project Pages
-------------

- [Main Website](http://www.blender.org)
- [Reference Manual](https://docs.blender.org/manual/en/latest/index.html)
- [User Community](https://www.blender.org/community/)

Development
-----------

- [Build Instructions](https://developer.blender.org/docs/handbook/building_blender/)
- [Code Review & Bug Tracker](https://projects.blender.org)
- [Developer Forum](https://devtalk.blender.org)
- [Developer Documentation](https://developer.blender.org/docs/)


License
-------

Blender as a whole is licensed under the GNU General Public License, Version 3.
Individual files may have a different but compatible license.

See [blender.org/about/license](https://www.blender.org/about/license) for details.
