/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 */

#pragma once

#include <cstdint>

#include "GHOST_Types.hh"

namespace blender {
struct bContext;
struct wmWindow;
struct wmWindowManager;
struct wmOperatorType;

void wm_modifier_wheel_init(wmWindow *win);
void wm_modifier_wheel_draw(wmWindow *win);
void wm_modifier_wheel_release(wmWindow *win, uint64_t time);
/** Consume wheel input before updating the editor cursor or emulating mouse buttons. */
bool wm_modifier_wheel_event(
    wmWindowManager *wm, wmWindow *win, GHOST_TEventType type, const void *data, uint64_t time);
void wm_modifier_wheel_popup(bContext *C, wmWindow *win, const int xy[2]);
void wm_event_modifier_wheel_set(wmWindow *win, uint8_t modifiers, uint64_t time);
void WM_OT_modifier_wheel_toggle(wmOperatorType *ot);
}  // namespace blender
