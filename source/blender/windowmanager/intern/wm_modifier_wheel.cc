/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 */

#include <algorithm>

#include "BLI_listbase.hh"
#include "BLI_math_color_c.hh"
#include "BLI_math_vector_c.hh"
#include "BLI_rect.hh"

#include "DNA_screen_types.h"
#include "DNA_userdef_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_context.hh"
#include "BKE_screen.hh"
#include "BKE_wm_runtime.hh"

#include "GHOST_ISystem.hh"
#include "GHOST_IWindow.hh"
#include "GPU_state.hh"

#include "UI_interface.hh"
#include "UI_interface_icons.hh"
#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_types.hh"
#include "wm_event_system.hh"
#include "wm_event_types.hh"
#include "wm_modifier_wheel.hh"
#include "wm_window.hh"

namespace blender {

using Wheel = bke::WindowRuntime::ModifierWheel;
using Button = Wheel::Button;

static constexpr float wheel_scale = 0.9f;

static void wheel_redraw(wmWindow *win)
{
  if (bScreen *screen = WM_window_get_active_screen(win)) {
    screen->do_draw = true;
  }
}

static float wheel_radius(const Wheel &wheel)
{
  return (wheel.collapsed ? 1.25f : 3.15f) * UI_UNIT_X * wheel_scale;
}

static void wheel_clamp(wmWindow *win)
{
  Wheel &wheel = win->runtime->modifier_wheel;
  const int2 size = WM_window_native_pixel_size(win);
  const float radius = wheel_radius(wheel);
  if (wheel.center[0] < 0.0f) {
    wheel.center[0] = wheel.center[1] = radius + UI_UNIT_X;
  }
  for (int i = 0; i < 2; i++) {
    const float margin = std::min(radius + 0.25f * UI_UNIT_X, size[i] * 0.5f);
    wheel.center[i] = std::clamp(wheel.center[i], margin, size[i] - margin);
  }
}

static rctf wheel_button_rect(const Wheel &wheel, const Button button)
{
  float x = wheel.center[0], y = wheel.center[1];
  const float offset = 2.15f * UI_UNIT_X * wheel_scale;
  switch (button) {
    case Button::Shift:
      x -= offset;
      break;
    case Button::Ctrl:
      y += offset;
      break;
    case Button::Alt:
      y -= offset;
      break;
    case Button::Favorites:
      x += offset;
      break;
    default:
      break;
  }
  const float radius = (button == Button::Handle && wheel.collapsed ? 1.25f : 1.0f) * UI_UNIT_X *
                       wheel_scale;
  return {x - radius, x + radius, y - radius, y + radius};
}

static Button wheel_hit_test(const Wheel &wheel, const int xy[2])
{
  if (!wheel.enabled) {
    return Button::None;
  }
  for (const Button button :
       {Button::Handle, Button::Ctrl, Button::Shift, Button::Alt, Button::Favorites})
  {
    const rctf rect = wheel_button_rect(wheel, button);
    const float dx = xy[0] - BLI_rctf_cent_x(&rect);
    const float dy = xy[1] - BLI_rctf_cent_y(&rect);
    const float radius = BLI_rctf_size_x(&rect) * 0.5f;
    if (dx * dx + dy * dy <= radius * radius) {
      return button;
    }
    if (wheel.collapsed) {
      break;
    }
  }
  return Button::None;
}

static bool wheel_capture_touch(void *user_data, int32_t x, int32_t y)
{
  wmWindow *win = static_cast<wmWindow *>(user_data);
  if (!win || !win->runtime->modifier_wheel.enabled) {
    return false;
  }
  wm_cursor_position_from_ghost_client_coords(win, &x, &y);
  wheel_clamp(win);
  const int xy[2] = {x, y};
  return wheel_hit_test(win->runtime->modifier_wheel, xy) != Button::None;
}

void wm_modifier_wheel_init(wmWindow *win)
{
  Wheel &wheel = win->runtime->modifier_wheel;
  if (!wheel.initialized) {
#ifdef __ANDROID__
    wheel.enabled = true;
#endif
    wheel.initialized = true;
    win->runtime->physical_modifiers = win->runtime->eventstate->modifier;
    copy_v2_v2_int(wheel.pointer_xy, win->runtime->eventstate->xy);
  }
  static_cast<GHOST_IWindow *>(win->runtime->ghostwin)
      ->setTouchCaptureCallback(wheel_capture_touch);
}

static bool wheel_has_modal_interaction(const wmWindow *win)
{
  /* The Screen Editing keymap is always present in the modal handler list. */
  for (const wmEventHandler &handler : win->runtime->modalhandlers) {
    if (ELEM(handler.type, WM_HANDLER_TYPE_UI, WM_HANDLER_TYPE_OP)) {
      return true;
    }
  }
  return false;
}

static int wheel_button_modifier(const Button button)
{
  switch (button) {
    case Button::Ctrl:
      return KM_CTRL;
    case Button::Shift:
      return KM_SHIFT;
    case Button::Alt:
      return KM_ALT;
    default:
      return 0;
  }
}

static void wheel_button_activate(wmWindow *win, const Button button, const uint64_t time)
{
  Wheel &wheel = win->runtime->modifier_wheel;
  if (const int modifier = wheel_button_modifier(button)) {
    wm_event_modifier_wheel_set(win, wheel.modifiers ^ modifier, time);
  }
  else if (button == Button::Handle) {
    wheel.collapsed = !wheel.collapsed;
    wheel_clamp(win);
  }
  else if (button == Button::Favorites) {
    /* Open the menu when processing the event queue, where a context is available. */
    wmEvent event = *win->runtime->eventstate;
    event.type = EVT_MODIFIER_WHEEL_MENU;
    event.val = KM_PRESS;
    event.flag = eWM_EventFlag(0);
    event.utf8_buf[0] = '\0';
    WM_event_add(win, &event);
  }
}

static void wheel_contact(wmWindow *win,
                          const uint64_t device,
                          const uint64_t id,
                          const bool pointer,
                          const GHOST_TTouchPhase phase,
                          const int xy[2],
                          const uint64_t time)
{
  Wheel &wheel = win->runtime->modifier_wheel;
  int index = -1;
  for (const int i : wheel.contacts.index_range()) {
    const Wheel::Contact &contact = wheel.contacts[i];
    if (contact.device == device && contact.id == id && contact.pointer == pointer) {
      index = i;
      break;
    }
  }
  if (phase == GHOST_TTouchPhase::Down) {
    if (index != -1) {
      wheel.contacts.remove_and_reorder(index);
    }
    const Button button = wheel_hit_test(wheel, xy);
    if (button != Button::None) {
      /* Moving the handle while another control is held would move its release target. */
      for (const Wheel::Contact &contact : wheel.contacts) {
        if (button == Button::Handle || contact.button == Button::Handle) {
          return;
        }
      }
      Wheel::Contact contact{
          device, id, button, {float(xy[0]), float(xy[1])}, {wheel.center[0], wheel.center[1]}};
      contact.pointer = pointer;
      wheel.contacts.append(contact);
      wheel_redraw(win);
    }
    return;
  }
  if (index == -1) {
    return;
  }
  Wheel::Contact &contact = wheel.contacts[index];
  const float dx = xy[0] - contact.start[0], dy = xy[1] - contact.start[1];
  const float threshold = 5.0f * UI_SCALE_FAC;
  contact.moved |= dx * dx + dy * dy > threshold * threshold;
  if (phase == GHOST_TTouchPhase::Motion && contact.button == Button::Handle && contact.moved) {
    wheel.center[0] = contact.center[0] + dx;
    wheel.center[1] = contact.center[1] + dy;
    wheel_clamp(win);
  }
  if (phase == GHOST_TTouchPhase::Up || phase == GHOST_TTouchPhase::Cancel) {
    const Button button = contact.button;
    const bool activate = phase == GHOST_TTouchPhase::Up && !contact.moved &&
                          wheel_hit_test(wheel, xy) == button;
    wheel.contacts.remove_and_reorder(index);
    if (activate) {
      wheel_button_activate(win, button, time);
    }
  }
  wheel_redraw(win);
}

void wm_modifier_wheel_release(wmWindow *win, const uint64_t time)
{
  Wheel &wheel = win->runtime->modifier_wheel;
  wm_event_modifier_wheel_set(win, 0, time);
  wheel.contacts.clear();
  wheel.hover = Button::None;
  wheel.pointer_buttons = 0;
  wheel_redraw(win);
}

bool wm_modifier_wheel_event(wmWindowManager *wm,
                             wmWindow *win,
                             const GHOST_TEventType type,
                             const void *data,
                             const uint64_t time)
{
  Wheel &wheel = win->runtime->modifier_wheel;
  if (type == GHOST_kEventTouch) {
    const auto &touch = *static_cast<const GHOST_TEventTouchData *>(data);
    int xy[2] = {touch.x, touch.y};
    wm_cursor_position_from_ghost_client_coords(win, &xy[0], &xy[1]);
    wheel_contact(win, touch.device_id, touch.contact_id, false, touch.phase, xy, time);
    return true;
  }
  if (type == GHOST_kEventCursorHoverClear) {
    if (wheel.hover != Button::None) {
      wheel.hover = Button::None;
      wheel_redraw(win);
    }
    return false;
  }
  if (type == GHOST_kEventCursorMove) {
    const auto &cursor = *static_cast<const GHOST_TEventCursorData *>(data);
    if (cursor.is_touch) {
      return false;
    }
    wheel.pointer_xy[0] = cursor.x;
    wheel.pointer_xy[1] = cursor.y;
    wm_cursor_position_from_ghost_screen_coords(win, &wheel.pointer_xy[0], &wheel.pointer_xy[1]);
    if (wheel.captured_buttons) {
      wheel_contact(win, 0, 0, true, GHOST_TTouchPhase::Motion, wheel.pointer_xy, time);
      return true;
    }
    const Button hover = wheel.pointer_buttons ? Button::None :
                                                 wheel_hit_test(wheel, wheel.pointer_xy);
    if (hover != wheel.hover) {
      if (wheel.hover == Button::None && hover != Button::None &&
          !wheel_has_modal_interaction(win) && !win->runtime->touch_drag_active)
      {
        wm_event_add_ghostevent(wm, win, GHOST_kEventCursorHoverClear, nullptr, time);
      }
      wheel.hover = hover;
      wheel_redraw(win);
    }
    return hover != Button::None;
  }
  if (ELEM(type, GHOST_kEventButtonDown, GHOST_kEventButtonUp)) {
    const auto &button = *static_cast<const GHOST_TEventButtonData *>(data);
    if (button.is_touch) {
      return false;
    }
    const uint32_t mask = button.button;
    const bool down = type == GHOST_kEventButtonDown;
    if (down && !wheel.pointer_buttons && wheel_hit_test(wheel, wheel.pointer_xy) != Button::None)
    {
      wheel.captured_buttons |= mask;
      if (mask == GHOST_kButtonMaskLeft) {
        wheel_contact(win, 0, 0, true, GHOST_TTouchPhase::Down, wheel.pointer_xy, time);
      }
      return true;
    }
    if (wheel.captured_buttons & mask) {
      if (!down) {
        wheel.captured_buttons &= ~mask;
        if (mask == GHOST_kButtonMaskLeft) {
          wheel_contact(win, 0, 0, true, GHOST_TTouchPhase::Up, wheel.pointer_xy, time);
        }
      }
      return true;
    }
    SET_FLAG_FROM_TEST(wheel.pointer_buttons, down, mask);
  }
  return false;
}

void wm_modifier_wheel_popup(bContext *C, wmWindow *win, const int xy[2])
{
  const Wheel &wheel = win->runtime->modifier_wheel;
  bScreen *screen = WM_window_get_active_screen(win);
  if (!wheel.enabled || !screen) {
    return;
  }

  /* Wheel input leaves the editor cursor in place. */
  ScrArea *area = BKE_screen_find_area_xy(screen, SPACE_TYPE_ANY, xy);
  if (!area) {
    area = screen->areabase.first();
  }
  if (!area) {
    return;
  }
  ARegion *region = BKE_area_find_region_xy(area, RGN_TYPE_ANY, xy);
  if (!region) {
    region = BKE_area_find_region_type(area, RGN_TYPE_WINDOW);
  }
  if (!region) {
    return;
  }

  wmWindow *previous_window = CTX_wm_window(C);
  ScrArea *previous_area = CTX_wm_area(C);
  ARegion *previous_region = CTX_wm_region(C);
  CTX_wm_window_set(C, win);
  CTX_wm_area_set(C, area);
  CTX_wm_region_set(C, region);
  const int anchor[2] = {int(wheel.center[0] + wheel_radius(wheel) + UI_UNIT_X),
                         int(wheel.center[1])};
  WM_tooltip_clear(C, win);
  ui::popup_menu_invoke(C, "SCREEN_MT_user_menu", nullptr, anchor);
  CTX_wm_window_set(C, previous_window);
  CTX_wm_area_set(C, previous_area);
  CTX_wm_region_set(C, previous_region);
}

static void wheel_draw_icon(const int icon,
                            const float x,
                            const float y,
                            const float size,
                            const uchar color[4],
                            const bool active)
{
  /* Some input icons, such as Ctrl, are wider than a single key. */
  const float offset = ui::event_icon_offset(icon);
  const float width = size * (offset >= 2.0f ? 2.0f : offset >= 1.0f ? 1.5f : 1.0f);
  ui::icon_draw_ex(x - width * 0.5f,
                   y - size * 0.5f,
                   icon,
                   ICON_DEFAULT_HEIGHT / size,
                   color[3] / 255.0f,
                   0.0f,
                   color,
                   false,
                   UI_NO_ICON_OVERLAY_TEXT,
                   active);
}

static void wheel_draw_button(const Wheel &wheel, const Button button, const int icon)
{
  const uiWidgetColors &colors = ui::theme::theme_get()->tui.wcol_toolbar_item;
  const bool active = (wheel.modifiers & wheel_button_modifier(button)) != 0;
  float fill[4], outline[4];
  rgba_uchar_to_float(fill, active ? colors.inner_sel : colors.inner);
  rgba_uchar_to_float(outline, colors.outline);
  bool pressed = false;
  for (const Wheel::Contact &contact : wheel.contacts) {
    pressed |= contact.button == button && !contact.moved;
  }
  if (wheel.hover == button || pressed) {
    for (int i = 0; i < 3; i++) {
      fill[i] = std::min(fill[i] + 0.08f, 1.0f);
    }
  }
  const rctf rect = wheel_button_rect(wheel, button);
  ui::draw_roundbox_corner_set(ui::CNR_ALL);
  ui::draw_roundbox_4fv_ex(
      &rect, fill, nullptr, 0.0f, outline, UI_SCALE_FAC, BLI_rctf_size_x(&rect) * 0.5f);
  const uchar *text = active ? colors.text_sel : colors.text;
  const float x = BLI_rctf_cent_x(&rect);
  const float y = BLI_rctf_cent_y(&rect);
  const float unit = UI_UNIT_X * wheel_scale;
  const bool show_modifiers = button == Button::Handle && wheel.collapsed && wheel.modifiers;
  wheel_draw_icon(icon, x, y + (show_modifiers ? 0.25f * unit : 0.0f), unit, text, active);
  if (show_modifiers) {
    const int icons[] = {ICON_EVENT_SHIFT, ICON_EVENT_CTRL, ICON_EVENT_ALT};
    const int modifiers[] = {KM_SHIFT, KM_CTRL, KM_ALT};
    for (int i = 0; i < 3; i++) {
      if (wheel.modifiers & modifiers[i]) {
        wheel_draw_icon(icons[i],
                        x + (i - 1) * 0.75f * unit,
                        y - 0.65f * unit,
                        0.4f * unit,
                        colors.text_sel,
                        true);
      }
    }
  }
}

void wm_modifier_wheel_draw(wmWindow *win)
{
  const Wheel &wheel = win->runtime->modifier_wheel;
  if (!wheel.enabled) {
    return;
  }
  wheel_clamp(win);
  GPU_blend(GPU_BLEND_ALPHA);
  wheel_draw_button(wheel, Button::Handle, wheel.collapsed ? ICON_ADD : ICON_COLLAPSEMENU);
  if (!wheel.collapsed) {
    wheel_draw_button(wheel, Button::Ctrl, ICON_EVENT_CTRL);
    wheel_draw_button(wheel, Button::Shift, ICON_EVENT_SHIFT);
    wheel_draw_button(wheel, Button::Alt, ICON_EVENT_ALT);
    wheel_draw_button(wheel, Button::Favorites, ICON_SOLO_ON);
  }
  GPU_blend(GPU_BLEND_NONE);
}

static wmOperatorStatus modifier_wheel_toggle_exec(bContext *C, wmOperator * /*op*/)
{
  wmWindow *win = CTX_wm_window(C);
  Wheel &wheel = win->runtime->modifier_wheel;
  wheel.enabled = !wheel.enabled;
  if (!wheel.enabled) {
    wm_modifier_wheel_release(win, GHOST_ISystem::getSystem()->getMilliSeconds());
  }
  wheel_clamp(win);
  wheel_redraw(win);
  return OPERATOR_FINISHED;
}

void WM_OT_modifier_wheel_toggle(wmOperatorType *ot)
{
  ot->name = "Toggle Modifier Wheel";
  ot->idname = "WM_OT_modifier_wheel_toggle";
  ot->description = "Show or hide the floating modifier controls";
  ot->exec = modifier_wheel_toggle_exec;
  ot->poll = WM_operator_winactive;
}

}  // namespace blender
