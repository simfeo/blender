/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup android
 *
 * Stand-ins for the two private platform libraries a Mesa Turnip HAL build
 * links against. Neither is reachable from an app: the dynamic linker only
 * exposes a fixed whitelist of system libraries to an app namespace, and
 * `libcutils` and `libhardware` are not on it, so loading the driver from the
 * APK fails at dlopen before any Vulkan call happens.
 *
 * Turnip needs six symbols between them, all peripheral to rendering: ATrace
 * instrumentation and one property read. Supplying them here lets the driver
 * load out of the APK's own library directory, which a non-debuggable build
 * can do.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/system_properties.h>

/* -------------------------------------------------------------------- */
/** \name libcutils
 * \{ */

#ifdef SHIM_CUTILS

/* Tracing is reported permanently off, so the driver skips emitting events and
 * the begin/end bodies below are never reached in practice. */
uint64_t atrace_get_enabled_tags(void)
{
  return 0;
}

void atrace_init(void) {}

void atrace_begin_body(const char *name)
{
  (void)name;
}

void atrace_end_body(void) {}

/**
 * The libcutils spelling of a system property read, on top of the libc one
 * that is public. Returns the length written, zero when unset.
 */
int property_get(const char *key, char *value, const char *default_value)
{
  const int len = __system_property_get(key, value);
  if (len > 0) {
    return len;
  }
  if (default_value == NULL) {
    value[0] = '\0';
    return 0;
  }
  strncpy(value, default_value, PROP_VALUE_MAX - 1);
  value[PROP_VALUE_MAX - 1] = '\0';
  return (int)strlen(value);
}

#endif /* SHIM_CUTILS */

/** \} */

/* -------------------------------------------------------------------- */
/** \name libhardware
 * \{ */

#ifdef SHIM_HARDWARE

/**
 * Reports every HAL module as absent.
 *
 * Turnip asks for the gralloc module to import buffers allocated elsewhere.
 * Blender only ever presents through its own swapchain, so that path is not
 * taken, and failing the lookup is what the driver already expects on a
 * platform without the module.
 */
int hw_get_module(const char *id, const void **module)
{
  (void)id;
  if (module != NULL) {
    *module = NULL;
  }
  return -ENOENT;
}

#endif /* SHIM_HARDWARE */

/** \} */
