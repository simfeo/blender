# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Give Python's ``webbrowser`` a browser on Android.

Every link in Blender goes through ``wm.url_open`` and ``webbrowser.open()``, which looks
for ``xdg-open`` and similar programs, finds none on Android and returns False silently.
Registering with ``webbrowser`` rather than changing ``wm.url_open`` also fixes add-ons
and the pre-filled bug report, which call ``webbrowser`` directly.
"""

import sys

__all__ = (
    "register",
    "unregister",
)

_NAME = "blender-android"


class _AndroidBrowser:
    """A ``webbrowser`` controller that hands the URL to ``wm.platform_url_open``."""

    name = _NAME
    basename = _NAME

    def open(self, url, new=0, autoraise=True):
        import bpy
        # The operator reports its own error, so False here is not silent.
        return bpy.ops.wm.platform_url_open(url=url) == {'FINISHED'}

    def open_new(self, url):
        return self.open(url)

    def open_new_tab(self, url):
        return self.open(url)


def register():
    # CPython's marker for an Android build; webbrowser already works elsewhere.
    if not hasattr(sys, "getandroidapilevel"):
        return

    import webbrowser

    # Reloading scripts runs this again, and webbrowser.register() does not deduplicate.
    if _NAME in getattr(webbrowser, "_browsers", {}):
        return

    webbrowser.register(_NAME, None, _AndroidBrowser(), preferred=True)


def unregister():
    # webbrowser has no way to unregister a controller.
    pass
