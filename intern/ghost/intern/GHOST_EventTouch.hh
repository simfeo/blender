/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 */

#pragma once

#include "GHOST_Event.hh"

class GHOST_EventTouch : public GHOST_Event {
 public:
  GHOST_EventTouch(uint64_t msec, GHOST_IWindow *window, const GHOST_TEventTouchData &data)
      : GHOST_Event(msec, GHOST_kEventTouch, window), touch_data_(data)
  {
    data_ = &touch_data_;
  }

 private:
  GHOST_TEventTouchData touch_data_;
};
