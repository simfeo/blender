/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 *
 * Device memory tier used to scale back GPU memory use on constrained Android devices.
 */

#pragma once

#ifdef __ANDROID__

#  include <cstdlib>
#  include <sys/system_properties.h>
#  include <unistd.h>

/**
 * True when the device has too little RAM to afford full-resolution render targets and a
 * third swapchain image. Roomier devices keep the default (better latency and quality).
 *
 * Override with `setprop debug.blender.lowmem 1` (or `0`) for testing.
 */
inline bool GHOST_android_is_low_memory_device()
{
  static const bool is_low_memory = []() {
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.blender.lowmem", value) > 0 && value[0] != '\0') {
      return atoi(value) != 0;
    }
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long page_size = sysconf(_SC_PAGE_SIZE);
    if (pages <= 0 || page_size <= 0) {
      return false;
    }
    const uint64_t total_bytes = uint64_t(pages) * uint64_t(page_size);
    /* 4GB devices report ~3.4GB usable and cannot hold full-resolution EEVEE targets
     * alongside the rest of the system; 8GB+ devices are comfortable. */
    return total_bytes < (6ull * 1024ull * 1024ull * 1024ull);
  }();
  return is_low_memory;
}

/** Divisor applied to the native window size. 1 = native, 2 = half resolution. */
inline uint32_t GHOST_android_render_scale_divisor()
{
  static const uint32_t divisor = []() -> uint32_t {
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.blender.renderdiv", value) > 0 && value[0] != '\0') {
      const int v = atoi(value);
      if (v >= 1 && v <= 4) {
        return uint32_t(v);
      }
    }
    return GHOST_android_is_low_memory_device() ? 2 : 1;
  }();
  return divisor;
}

/**
 * UI density used by Blender's desktop-oriented layout.
 *
 * Android's density is designed to turn device-independent mobile widgets into large physical
 * touch targets. Passing the S24 Ultra's 450 DPI straight through makes Blender's already dense
 * desktop interface about 4.7 times larger than its baseline and causes whole editors and menus
 * to be clipped. Use a compact but still readable default and keep a system-property override for
 * device testing: `setprop debug.blender.uidpi 160`.
 */
inline uint32_t GHOST_android_ui_dpi()
{
  static const uint32_t dpi = []() -> uint32_t {
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.blender.uidpi", value) > 0 && value[0] != '\0') {
      const int v = atoi(value);
      if (v >= 96 && v <= 320) {
        return uint32_t(v);
      }
    }
    return 160;
  }();
  return dpi;
}

/**
 * Map a display-space input coordinate into the (possibly downscaled) window buffer space
 * that Blender renders and hit-tests in.
 */
inline float ghost_android_scale_input(float value)
{
  const uint32_t divisor = GHOST_android_render_scale_divisor();
  return divisor > 1 ? value / float(divisor) : value;
}

#endif /* __ANDROID__ */
