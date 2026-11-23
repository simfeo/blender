/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 * Declaration of GHOST_EventButton class.
 */

#pragma once

#include "GHOST_Event.hh"
#include "GHOST_Window.hh"

/**
 * Mouse button event.
 */
class GHOST_EventButton : public GHOST_Event {
 public:
  /**
   * Constructor.
   * \param time: The time this event was generated.
   * \param type: The type of this event.
   * \param window: The window of this event.
   * \param button: The state of the buttons were at the time of the event.
   * \param tablet: The tablet data associated with this event.
   */
  GHOST_EventButton(uint64_t time,
                    GHOST_TEventType type,
                    GHOST_IWindow *window,
                    GHOST_TButton button,
                    const GHOST_TabletData &tablet,
                    bool is_touch = false,
                    bool is_touch_drag = false,
                    int32_t touch_start_x = 0,
                    int32_t touch_start_y = 0)
      : GHOST_Event(time, type, window),
        button_event_data_({button, tablet, is_touch, is_touch_drag, touch_start_x, touch_start_y})
  {
    data_ = &button_event_data_;
  }

 protected:
  /** The button event data. */
  GHOST_TEventButtonData button_event_data_;
};
