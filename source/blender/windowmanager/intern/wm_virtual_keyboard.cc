/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * On-screen keyboard for touch devices.
 *
 * A phone has no keyboard, and the platform one can only type: it cannot press Ctrl, it cannot
 * send a numpad view key, and it covers half the screen while doing neither. This one is drawn by
 * Blender, spans the whole window, and turns a tap into a real key event.
 *
 * Why it lives here rather than in an add-on
 * ------------------------------------------
 * A key has to arrive as a key. If the tap that presses it reaches the interface layer as a
 * pointer event, whatever text field was being edited ends there and then: the field installs a UI
 * handler that outranks every modal operator, so it sees the click first and commits. No overlay
 * above it can prevent that, which is why the Python version could never type into a native field.
 *
 * So the tap is answered in #wm_virtual_keyboard_ghost_event, called from `ghost_event_proc()`
 * before the event reaches the queue at all. While the keyboard is open, a pointer event that
 * lands on it is consumed there and a key event is fed back in its place, built the same way
 * #wm_window_update_eventstate_modifiers builds the modifier events it injects. Nothing downstream
 * can tell the difference between this and a hardware keyboard, which is the point: text editing,
 * the keymap, operator dispatch and the search fields all keep working with no special case
 * anywhere.
 *
 * Drawing goes through #WM_draw_cb_activate, which paints in window coordinates after every region
 * and after the editor outlines. One callback covers the whole window, so the keyboard works in
 * every workspace and every editor without knowing anything about either.
 */

#include <cmath>
#include <cstring>

#include "GHOST_ISystem.hh"

#include "DNA_screen_types.h"
#include "DNA_userdef_types.h"
#include "DNA_workspace_types.h"
#include "DNA_windowmanager_types.h"

#include "BLI_listbase.hh"
#include "BLI_math_base_c.hh"
#include "BLI_math_vector_c.hh"
#include "BLI_rect.hh"
#include "BLI_string.hh"
#include "BLI_string_utf8.hh"
#include "BLI_time.hh"
#include "BLI_utildefines.hh"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "BLF_api.hh"

#include "BKE_context.hh"
#include "BKE_screen.hh"

#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "wm.hh"
#include "wm_event_system.hh"
#include "wm_window.hh"
#include "wm_window_private.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Key tables
 *
 * A row is a list of keys whose widths add up to the same number of units, so the columns line up
 * whatever the window is doing. Text keys carry the characters they produce, plain and shifted;
 * everything else carries none and is recognized by its key code alone.
 * \{ */

enum class VKKind {
  Key,
  Mod,
  /**
   * Caps Lock, which is not a modifier and is deliberately not one of the slots below.
   *
   * Those three are mirrored into the window's real modifier state, and #vk_sync_ghost_mods reads
   * that state back to correct itself. Caps has no counterpart there -- it changes which character
   * a letter types and nothing else -- so a bit for it in the same mask would be a bit the sync
   * could never find on the other side.
   */
  Caps,
  Layer,
  Close,
  Move,
};

/* Modifier slots, in the order they are shown. */
enum {
  VK_MOD_CTRL = 0,
  VK_MOD_SHIFT = 1,
  VK_MOD_ALT = 2,
  VK_MOD_NUM = 3,
};

struct VKKeySpec {
  const char *label;
  VKKind kind;
  /** #GHOST_TKey for #VKKind::Key, a `VK_MOD_*` slot for #VKKind::Mod, a layer for the rest. */
  int code;
  const char *utf8;
  const char *utf8_shift;
  float units;
};

#define VK_KEY(label, key, text, text_shift, units) \
  { \
    label, VKKind::Key, int(key), text, text_shift, units \
  }
#define VK_PLAIN(label, key, units) \
  { \
    label, VKKind::Key, int(key), nullptr, nullptr, units \
  }
#define VK_MOD(label, slot, units) \
  { \
    label, VKKind::Mod, slot, nullptr, nullptr, units \
  }
#define VK_LAYER(label, layer, units) \
  { \
    label, VKKind::Layer, layer, nullptr, nullptr, units \
  }
#define VK_CAPS(label, units) \
  { \
    label, VKKind::Caps, 0, nullptr, nullptr, units \
  }

/* Letters, the number row and the function row: the block that is always shown.
 *
 * Every row totals 14 units, which is what makes the columns line up down the keyboard. Change a
 * width here and the row it is in has to give the same amount back somewhere else, or that row
 * alone comes out staggered against the rest. */

/* Touch: Esc at the head of the function row, where a desktop keyboard puts it.
 *
 * F1 opens the manual for whatever is under the cursor, F2 renames, F3 searches, F9 reopens the
 * last operator's panel, F11 and F12 render. On a desktop each of those is one key; without this
 * row a phone has no way to reach any of them at all. Esc came up from the head of the letters,
 * which is where a real keyboard has Tab -- and that freed the space the rows below now spend on
 * the brackets and punctuation that writing a script needs. */
static const VKKeySpec vk_row_function[] = {
    VK_PLAIN("Esc", GHOST_kKeyEsc, 2.0f),
    VK_PLAIN("F1", GHOST_kKeyF1, 1.0f),
    VK_PLAIN("F2", GHOST_kKeyF2, 1.0f),
    VK_PLAIN("F3", GHOST_kKeyF3, 1.0f),
    VK_PLAIN("F4", GHOST_kKeyF4, 1.0f),
    VK_PLAIN("F5", GHOST_kKeyF5, 1.0f),
    VK_PLAIN("F6", GHOST_kKeyF6, 1.0f),
    VK_PLAIN("F7", GHOST_kKeyF7, 1.0f),
    VK_PLAIN("F8", GHOST_kKeyF8, 1.0f),
    VK_PLAIN("F9", GHOST_kKeyF9, 1.0f),
    VK_PLAIN("F10", GHOST_kKeyF10, 1.0f),
    VK_PLAIN("F11", GHOST_kKeyF11, 1.0f),
    VK_PLAIN("F12", GHOST_kKeyF12, 1.0f),
};

static const VKKeySpec vk_row_digits[] = {
    VK_KEY("`", GHOST_kKeyAccentGrave, "`", "~", 1.0f),
    VK_KEY("1", GHOST_kKey1, "1", "!", 1.0f),
    VK_KEY("2", GHOST_kKey2, "2", "@", 1.0f),
    VK_KEY("3", GHOST_kKey3, "3", "#", 1.0f),
    VK_KEY("4", GHOST_kKey4, "4", "$", 1.0f),
    VK_KEY("5", GHOST_kKey5, "5", "%", 1.0f),
    VK_KEY("6", GHOST_kKey6, "6", "^", 1.0f),
    VK_KEY("7", GHOST_kKey7, "7", "&", 1.0f),
    VK_KEY("8", GHOST_kKey8, "8", "*", 1.0f),
    VK_KEY("9", GHOST_kKey9, "9", "(", 1.0f),
    VK_KEY("0", GHOST_kKey0, "0", ")", 1.0f),
    VK_KEY("-", GHOST_kKeyMinus, "-", "_", 1.0f),
    VK_KEY("=", GHOST_kKeyEqual, "=", "+", 1.0f),
    VK_PLAIN("Bksp", GHOST_kKeyBackSpace, 1.0f),
};

static const VKKeySpec vk_row_q[] = {
    VK_PLAIN("Tab", GHOST_kKeyTab, 1.0f),
    VK_KEY("Q", GHOST_kKeyQ, "q", "Q", 1.0f),
    VK_KEY("W", GHOST_kKeyW, "w", "W", 1.0f),
    VK_KEY("E", GHOST_kKeyE, "e", "E", 1.0f),
    VK_KEY("R", GHOST_kKeyR, "r", "R", 1.0f),
    VK_KEY("T", GHOST_kKeyT, "t", "T", 1.0f),
    VK_KEY("Y", GHOST_kKeyY, "y", "Y", 1.0f),
    VK_KEY("U", GHOST_kKeyU, "u", "U", 1.0f),
    VK_KEY("I", GHOST_kKeyI, "i", "I", 1.0f),
    VK_KEY("O", GHOST_kKeyO, "o", "O", 1.0f),
    VK_KEY("P", GHOST_kKeyP, "p", "P", 1.0f),
    VK_KEY("[", GHOST_kKeyLeftBracket, "[", "{", 1.0f),
    VK_KEY("]", GHOST_kKeyRightBracket, "]", "}", 1.0f),
    VK_KEY("\\", GHOST_kKeyBackslash, "\\", "|", 1.0f),
};

static const VKKeySpec vk_row_a[] = {
    VK_CAPS("Caps", 1.5f),
    VK_KEY("A", GHOST_kKeyA, "a", "A", 1.0f),
    VK_KEY("S", GHOST_kKeyS, "s", "S", 1.0f),
    VK_KEY("D", GHOST_kKeyD, "d", "D", 1.0f),
    VK_KEY("F", GHOST_kKeyF, "f", "F", 1.0f),
    VK_KEY("G", GHOST_kKeyG, "g", "G", 1.0f),
    VK_KEY("H", GHOST_kKeyH, "h", "H", 1.0f),
    VK_KEY("J", GHOST_kKeyJ, "j", "J", 1.0f),
    VK_KEY("K", GHOST_kKeyK, "k", "K", 1.0f),
    VK_KEY("L", GHOST_kKeyL, "l", "L", 1.0f),
    VK_KEY(";", GHOST_kKeySemicolon, ";", ":", 1.0f),
    VK_KEY("'", GHOST_kKeyQuote, "'", "\"", 1.0f),
    VK_PLAIN("Enter", GHOST_kKeyEnter, 1.5f),
};

static const VKKeySpec vk_row_z[] = {
    VK_MOD("Shift", VK_MOD_SHIFT, 2.0f),
    VK_KEY("Z", GHOST_kKeyZ, "z", "Z", 1.0f),
    VK_KEY("X", GHOST_kKeyX, "x", "X", 1.0f),
    VK_KEY("C", GHOST_kKeyC, "c", "C", 1.0f),
    VK_KEY("V", GHOST_kKeyV, "v", "V", 1.0f),
    VK_KEY("B", GHOST_kKeyB, "b", "B", 1.0f),
    VK_KEY("N", GHOST_kKeyN, "n", "N", 1.0f),
    VK_KEY("M", GHOST_kKeyM, "m", "M", 1.0f),
    VK_KEY(",", GHOST_kKeyComma, ",", "<", 1.0f),
    VK_KEY(".", GHOST_kKeyPeriod, ".", ">", 1.0f),
    VK_KEY("/", GHOST_kKeySlash, "/", "?", 1.0f),
    VK_PLAIN("Del", GHOST_kKeyDelete, 2.0f),
};

static const VKKeySpec vk_row_space[] = {
    VK_MOD("Ctrl", VK_MOD_CTRL, 1.5f),
    VK_MOD("Alt", VK_MOD_ALT, 1.5f),
    VK_KEY("Space", GHOST_kKeySpace, " ", " ", 6.0f),
    VK_PLAIN("←", GHOST_kKeyLeftArrow, 1.0f),
    VK_PLAIN("↓", GHOST_kKeyDownArrow, 1.0f),
    VK_PLAIN("↑", GHOST_kKeyUpArrow, 1.0f),
    VK_PLAIN("→", GHOST_kKeyRightArrow, 1.0f),
    VK_LAYER("123", 1, 1.0f),
};

/* The numeric block beside the letters in landscape. Blender maps the numpad to view angles, so
 * these emit numpad keys on purpose and not the number row.
 *
 * Six rows, matching the letters beside it: the two blocks are placed into the same height, so a
 * block with fewer rows comes out with taller keys than its neighbour. Every row here totals 4
 * units for the same reason the letters all total 14. */
static const VKKeySpec vk_pad_row_top[] = {
    VK_PLAIN("PgUp", GHOST_kKeyUpPage, 1.0f),
    VK_PLAIN("PgDn", GHOST_kKeyDownPage, 1.0f),
    VK_PLAIN("Ins", GHOST_kKeyInsert, 1.0f),
    VK_PLAIN("Del", GHOST_kKeyDelete, 1.0f),
};
static const VKKeySpec vk_pad_row0[] = {
    VK_KEY("/", GHOST_kKeyNumpadSlash, "/", "/", 1.0f),
    VK_KEY("*", GHOST_kKeyNumpadAsterisk, "*", "*", 1.0f),
    VK_KEY("-", GHOST_kKeyNumpadMinus, "-", "-", 1.0f),
    VK_KEY("+", GHOST_kKeyNumpadPlus, "+", "+", 1.0f),
};
static const VKKeySpec vk_pad_row1[] = {
    VK_KEY("7", GHOST_kKeyNumpad7, "7", "7", 1.0f),
    VK_KEY("8", GHOST_kKeyNumpad8, "8", "8", 1.0f),
    VK_KEY("9", GHOST_kKeyNumpad9, "9", "9", 1.0f),
    VK_PLAIN("Bksp", GHOST_kKeyBackSpace, 1.0f),
};
static const VKKeySpec vk_pad_row2[] = {
    VK_KEY("4", GHOST_kKeyNumpad4, "4", "4", 1.0f),
    VK_KEY("5", GHOST_kKeyNumpad5, "5", "5", 1.0f),
    VK_KEY("6", GHOST_kKeyNumpad6, "6", "6", 1.0f),
    VK_PLAIN("⏎", GHOST_kKeyNumpadEnter, 1.0f),
};
static const VKKeySpec vk_pad_row3[] = {
    VK_KEY("1", GHOST_kKeyNumpad1, "1", "1", 1.0f),
    VK_KEY("2", GHOST_kKeyNumpad2, "2", "2", 1.0f),
    VK_KEY("3", GHOST_kKeyNumpad3, "3", "3", 1.0f),
    VK_PLAIN("Home", GHOST_kKeyHome, 1.0f),
};
static const VKKeySpec vk_pad_row4[] = {
    VK_KEY("0", GHOST_kKeyNumpad0, "0", "0", 2.0f),
    VK_KEY(".", GHOST_kKeyNumpadPeriod, ".", ".", 1.0f),
    VK_PLAIN("End", GHOST_kKeyEnd, 1.0f),
};

/* Portrait has no room for a side block, so the same keys become a layer. Six rows here as well,
 * so switching between the two layers does not change the height of every key underneath the
 * thumb that switched them. The extra row is the bracket and quote block: they are on the letter
 * layer too, but reaching them from the numbers otherwise means two layer switches. */
static const VKKeySpec vk_num_row_sym[] = {
    VK_KEY("[", GHOST_kKeyLeftBracket, "[", "{", 1.0f),
    VK_KEY("]", GHOST_kKeyRightBracket, "]", "}", 1.0f),
    VK_KEY("\\", GHOST_kKeyBackslash, "\\", "|", 1.0f),
    VK_KEY(";", GHOST_kKeySemicolon, ";", ":", 1.0f),
    VK_KEY("'", GHOST_kKeyQuote, "'", "\"", 1.0f),
};
static const VKKeySpec vk_num_row0[] = {
    VK_KEY("7", GHOST_kKeyNumpad7, "7", "7", 1.0f),
    VK_KEY("8", GHOST_kKeyNumpad8, "8", "8", 1.0f),
    VK_KEY("9", GHOST_kKeyNumpad9, "9", "9", 1.0f),
    VK_KEY("/", GHOST_kKeyNumpadSlash, "/", "/", 1.0f),
    VK_PLAIN("Bksp", GHOST_kKeyBackSpace, 1.0f),
};
static const VKKeySpec vk_num_row1[] = {
    VK_KEY("4", GHOST_kKeyNumpad4, "4", "4", 1.0f),
    VK_KEY("5", GHOST_kKeyNumpad5, "5", "5", 1.0f),
    VK_KEY("6", GHOST_kKeyNumpad6, "6", "6", 1.0f),
    VK_KEY("*", GHOST_kKeyNumpadAsterisk, "*", "*", 1.0f),
    VK_PLAIN("⏎", GHOST_kKeyNumpadEnter, 1.0f),
};
static const VKKeySpec vk_num_row2[] = {
    VK_KEY("1", GHOST_kKeyNumpad1, "1", "1", 1.0f),
    VK_KEY("2", GHOST_kKeyNumpad2, "2", "2", 1.0f),
    VK_KEY("3", GHOST_kKeyNumpad3, "3", "3", 1.0f),
    VK_KEY("-", GHOST_kKeyNumpadMinus, "-", "-", 1.0f),
    VK_PLAIN("↑", GHOST_kKeyUpArrow, 1.0f),
};
static const VKKeySpec vk_num_row3[] = {
    VK_KEY("0", GHOST_kKeyNumpad0, "0", "0", 2.0f),
    VK_KEY(".", GHOST_kKeyNumpadPeriod, ".", ".", 1.0f),
    VK_KEY("+", GHOST_kKeyNumpadPlus, "+", "+", 1.0f),
    VK_PLAIN("↓", GHOST_kKeyDownArrow, 1.0f),
};
static const VKKeySpec vk_num_row4[] = {
    VK_LAYER("ABC", 0, 1.0f),
    VK_MOD("Ctrl", VK_MOD_CTRL, 1.0f),
    VK_MOD("Shift", VK_MOD_SHIFT, 1.0f),
    VK_MOD("Alt", VK_MOD_ALT, 1.0f),
    VK_PLAIN("Esc", GHOST_kKeyEsc, 1.0f),
};

#undef VK_KEY
#undef VK_PLAIN
#undef VK_MOD
#undef VK_LAYER

struct VKRow {
  const VKKeySpec *keys;
  int keys_num;
};

#define VK_ROW(array) \
  { \
    array, ARRAY_SIZE(array) \
  }

/* All three blocks are six rows deep. Two of them share a height in landscape, and the other two
 * swap places in portrait, so a block that is one row shorter than its neighbour shows it. */
static const VKRow vk_main_rows[] = {
    VK_ROW(vk_row_function),
    VK_ROW(vk_row_digits),
    VK_ROW(vk_row_q),
    VK_ROW(vk_row_a),
    VK_ROW(vk_row_z),
    VK_ROW(vk_row_space),
};
static const VKRow vk_pad_rows[] = {
    VK_ROW(vk_pad_row_top),
    VK_ROW(vk_pad_row0),
    VK_ROW(vk_pad_row1),
    VK_ROW(vk_pad_row2),
    VK_ROW(vk_pad_row3),
    VK_ROW(vk_pad_row4),
};
static const VKRow vk_number_rows[] = {
    VK_ROW(vk_num_row_sym),
    VK_ROW(vk_num_row0),
    VK_ROW(vk_num_row1),
    VK_ROW(vk_num_row2),
    VK_ROW(vk_num_row3),
    VK_ROW(vk_num_row4),
};

#undef VK_ROW

/** \} */

/* -------------------------------------------------------------------- */
/** \name State
 * \{ */

struct VKPlacedKey {
  const VKKeySpec *spec;
  rcti rect;
};

/** Never build a keyboard shorter than this, before the drawable band clamps it. */
static const float VK_MIN_HEIGHT = 150.0f;

struct VirtualKeyboard {
  bool open = false;
  wmWindow *win = nullptr;
  void *draw_handle = nullptr;

  /** The panel, in window coordinates. */
  rcti rect = {0, 0, 0, 0};
  /** How far the panel has been slid up from the bottom of the drawable band. */
  float offset = 0.0f;
  /** 0 letters, 1 numbers. Only used in portrait; landscape shows both at once. */
  int layer = 0;

  /**
   * The modifiers that are on, which is to say held down.
   *
   * They latch: tapping one turns it on and it stays on until it is tapped again. That is not
   * how it started -- a tapped modifier used to clear after the next key -- and the difference
   * is the whole point. Growing a face selection means Ctrl and NumpadPlus a dozen times, and
   * re-tapping Ctrl before every one of them is what made it not worth doing.
   *
   * Shift is the exception, and only for keys that type: it clears after a letter or a digit, so
   * typing a capital does not leave the next letter capital too. Ctrl and Alt never do that,
   * because nothing types with them.
   */
  uint8_t mods = 0;

  /**
   * Caps Lock, kept out of #mods on purpose: see #VKKind::Caps.
   *
   * It exists because a latched Shift is cleared by the first character it types, which is right
   * for one capital and useless for a word of them. Caps is the one that stays. It reaches letters
   * only, the way the key on a real keyboard does, so it does not turn 1 into !.
   */
  bool caps = false;

  Vector<VKPlacedKey> keys;

  /**
   * The string of the native text field being edited, read live so the bar shows what is in the
   * field even while the keyboard covers it. Null whenever nothing is being edited.
   */
  const char *const *text_edit = nullptr;
  /** The last key sent and when, for the moment of feedback the bar gives after a tap. */
  char last_key[64] = "";
  double last_key_time = 0.0;

  int hover = -1;
  int pressed = -1;
  bool moving = false;

  /** Last pointer position, in window coordinates. */
  int cursor[2] = {0, 0};
  /** Last position that was outside the keyboard, which injected events are attributed to. */
  int pinned[2] = {0, 0};
  int drag_prev[2] = {0, 0};
};

/* Android hands out exactly one window, and a second keyboard would have nothing to attach to. */
static VirtualKeyboard g_vk;

/** The interface resolution scale, which the keyboard sizes its text and its bar against. */
static float vk_scale()
{
  return (U.scale_factor > 0.0f) ? U.scale_factor : 1.0f;
}

static void vk_tag_redraw(wmWindow *win)
{
  /* The overlay is painted while the window is composited, so it is enough to ask for that rather
   * than to tag every region and make the editors redraw themselves. */
  bScreen *screen = WM_window_get_active_screen(win);
  if (screen) {
    screen->do_draw = true;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Layout
 * \{ */

/**
 * The window rows the keyboard is allowed to occupy.
 *
 * The top bar and the status bar are global areas: the keyboard is kept clear of both, so the
 * button that opens it and the editor menus stay reachable while it is up.
 */
static void vk_drawable_band(const wmWindow *win, int *r_bottom, int *r_top)
{
  const bScreen *screen = WM_window_get_active_screen(const_cast<wmWindow *>(win));
  int bottom = 0;
  int top = win->sizey;
  bool found = false;

  if (screen) {
    for (const ScrArea &area : screen->areabase) {
      if (ELEM(area.spacetype, SPACE_TOPBAR, SPACE_STATUSBAR)) {
        continue;
      }
      const int area_bottom = area.totrct.ymin;
      const int area_top = area.totrct.ymax;
      if (!found) {
        bottom = area_bottom;
        top = area_top;
        found = true;
      }
      else {
        bottom = min_ii(bottom, area_bottom);
        top = max_ii(top, area_top);
      }
    }
  }

  if (!found || (top - bottom) < 80) {
    bottom = 0;
    top = win->sizey;
  }
  *r_bottom = bottom;
  *r_top = top;
}

/**
 * Whether this point is inside an editor, and so worth remembering as where injected keys go.
 *
 * A stylus that leaves proximity parks the cursor outside the window, and a shortcut attributed to
 * that point reaches no editor at all: the key is delivered and nothing happens. Keeping the last
 * position that was actually over an editor is what makes a shortcut land where the user last
 * worked rather than wherever the pen happened to leave the screen.
 */
static bool vk_position_in_area(const wmWindow *win, const int xy[2])
{
  const bScreen *screen = WM_window_get_active_screen(const_cast<wmWindow *>(win));
  if (screen == nullptr) {
    return false;
  }
  for (const ScrArea &area : screen->areabase) {
    if (ELEM(area.spacetype, SPACE_TOPBAR, SPACE_STATUSBAR)) {
      continue;
    }
    if (BLI_rcti_isect_pt_v(&area.totrct, xy)) {
      return true;
    }
  }
  return false;
}

/**
 * Where an injected key should be aimed.
 *
 * The remembered position is preferred, but it cannot be trusted on its own: rotating the device
 * rearranges every area, and a point remembered in portrait can land outside the window entirely,
 * which delivers the key to no editor at all. When it no longer points at anything, the largest
 * editor is used instead, and a 3D viewport outranks a bigger flat one since that is what a
 * shortcut is usually meant for.
 */
static void vk_target_position(const VirtualKeyboard &vk, const wmWindow *win, int r_xy[2])
{
  if (vk_position_in_area(win, vk.pinned)) {
    copy_v2_v2_int(r_xy, vk.pinned);
    return;
  }

  r_xy[0] = win->sizex / 2;
  r_xy[1] = win->sizey / 2;

  const bScreen *screen = WM_window_get_active_screen(const_cast<wmWindow *>(win));
  if (screen == nullptr) {
    return;
  }
  const ScrArea *best = nullptr;
  float best_score = -1.0f;
  for (const ScrArea &area : screen->areabase) {
    if (ELEM(area.spacetype, SPACE_TOPBAR, SPACE_STATUSBAR)) {
      continue;
    }
    float score = float(BLI_rcti_size_x(&area.totrct)) * float(BLI_rcti_size_y(&area.totrct));
    if (area.spacetype == SPACE_VIEW3D) {
      score *= 4.0f;
    }
    if (score > best_score) {
      best = &area;
      best_score = score;
    }
  }
  if (best != nullptr) {
    r_xy[0] = BLI_rcti_cent_x(&best->totrct);
    r_xy[1] = BLI_rcti_cent_y(&best->totrct);
  }
}

static void vk_place_row(Vector<VKPlacedKey> &keys,
                         const VKRow &row,
                         float x,
                         float y,
                         float width,
                         float height,
                         float gap)
{
  float total = 0.0f;
  for (int i = 0; i < row.keys_num; i++) {
    total += row.keys[i].units;
  }
  if (total <= 0.0f) {
    return;
  }
  const float usable = width - gap * float(row.keys_num - 1);
  float cursor = x;
  for (int i = 0; i < row.keys_num; i++) {
    const float w = usable * (row.keys[i].units / total);
    VKPlacedKey placed;
    placed.spec = &row.keys[i];
    placed.rect.xmin = int(cursor);
    placed.rect.xmax = int(cursor + w);
    placed.rect.ymin = int(y);
    placed.rect.ymax = int(y + height);
    keys.append(placed);
    cursor += w + gap;
  }
}

static void vk_place_block(Vector<VKPlacedKey> &keys,
                           const VKRow *rows,
                           int rows_num,
                           float x,
                           float y,
                           float width,
                           float height,
                           float gap)
{
  const float row_h = (height - gap * float(rows_num - 1)) / float(rows_num);
  float cursor = y + height - row_h;
  for (int i = 0; i < rows_num; i++) {
    vk_place_row(keys, rows[i], x, cursor, width, row_h, gap);
    cursor -= row_h + gap;
  }
}

/* Two keys the layout owns rather than the tables: they exist only while the keyboard is up. */
static const VKKeySpec vk_spec_close = {"✕", VKKind::Close, 0, nullptr, nullptr, 1.0f};
static const VKKeySpec vk_spec_move = {"↕", VKKind::Move, 0, nullptr, nullptr, 1.0f};

static void vk_build_layout(VirtualKeyboard &vk, wmWindow *win)
{
  const int win_w = win->sizex;
  const int win_h = win->sizey;
  if (win_w <= 0 || win_h <= 0) {
    return;
  }

  int band_bottom, band_top;
  vk_drawable_band(win, &band_bottom, &band_top);
  const float band_h = float(band_top - band_bottom);

  const float scale = vk_scale();
  const bool portrait = win_w < win_h;

  /* A share of the window rather than of the band between the areas, so that a rotation really
   * recomputes it. Landscape takes the larger share on purpose: the window is short, and the share
   * that suits a tall one leaves rows too thin to hit and labels too small to read. */
  const float fraction = portrait ? 0.32f : 0.45f;

  float height = float(win_h) * fraction;
  height = min_ff(height, band_h * 0.9f);
  height = max_ff(height, min_ff(VK_MIN_HEIGHT * scale, band_h));
  height = min_ff(height, band_h);

  const float room = max_ff(0.0f, band_h - height);
  vk.offset = clamp_f(vk.offset, 0.0f, room);
  const float base = float(band_bottom) + vk.offset;

  vk.rect.xmin = 0;
  vk.rect.xmax = win_w;
  vk.rect.ymin = int(base);
  vk.rect.ymax = int(base + height);

  const float pad = max_ff(4.0f, height * 0.022f);
  const float gap = max_ff(2.0f, height * 0.013f);

  vk.keys.clear();

  /* The bar along the top carries the handle and the close button. Bounded rather than scaled with
   * the keyboard: a tall portrait keyboard turned it into a banner, while it only has to stay
   * large enough to hit. */
  const float bar_h = clamp_f(height * 0.10f, 28.0f * scale, 44.0f * scale);
  const float bar_y = base + height - pad - bar_h;

  /* Both bar buttons are square, so the handle reads as a button rather than as a rail. */
  const float button_w = bar_h;

  VKPlacedKey close_key;
  close_key.spec = &vk_spec_close;
  close_key.rect.xmax = int(float(win_w) - pad);
  close_key.rect.xmin = int(float(win_w) - pad - button_w);
  close_key.rect.ymin = int(bar_y);
  close_key.rect.ymax = int(bar_y + bar_h);
  vk.keys.append(close_key);

  VKPlacedKey move_key;
  move_key.spec = &vk_spec_move;
  move_key.rect.xmin = int(pad);
  move_key.rect.xmax = int(pad + button_w);
  move_key.rect.ymin = int(bar_y);
  move_key.rect.ymax = int(bar_y + bar_h);
  vk.keys.append(move_key);

  const float keys_top = bar_y - gap;
  const float keys_h = max_ff(60.0f, keys_top - (base + pad));

  if (!portrait) {
    const float pad_w = min_ff(float(win_w) * 0.27f, keys_h * 1.05f);
    const float main_w = float(win_w) - pad * 2.0f - pad_w - gap * 2.0f;
    vk_place_block(
        vk.keys, vk_main_rows, ARRAY_SIZE(vk_main_rows), pad, base + pad, main_w, keys_h, gap);
    vk_place_block(vk.keys,
                   vk_pad_rows,
                   ARRAY_SIZE(vk_pad_rows),
                   pad + main_w + gap * 2.0f,
                   base + pad,
                   pad_w,
                   keys_h,
                   gap);
  }
  else if (vk.layer == 1) {
    vk_place_block(vk.keys,
                   vk_number_rows,
                   ARRAY_SIZE(vk_number_rows),
                   pad,
                   base + pad,
                   float(win_w) - pad * 2.0f,
                   keys_h,
                   gap);
  }
  else {
    vk_place_block(vk.keys,
                   vk_main_rows,
                   ARRAY_SIZE(vk_main_rows),
                   pad,
                   base + pad,
                   float(win_w) - pad * 2.0f,
                   keys_h,
                   gap);
  }
}

static void vk_ensure_layout(VirtualKeyboard &vk, wmWindow *win)
{
  /* Rebuilt rather than cached against the window size. Rotation changes the size, the areas and
   * therefore the band all at once, and a layout that survives any of that keeps the proportion of
   * the orientation it was built in. The arithmetic is a few dozen multiplications. */
  vk_build_layout(vk, win);
}

static int vk_key_at(const VirtualKeyboard &vk, const int xy[2])
{
  for (const int i : vk.keys.index_range()) {
    if (BLI_rcti_isect_pt_v(&vk.keys[i].rect, xy)) {
      return i;
    }
  }
  return -1;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Drawing
 * \{ */

static void vk_rect_verts(uint pos, const rctf &rect, float radius, int segments)
{
  /* A rounded rectangle as a triangle fan from the middle. The corners are the only curved part,
   * so a handful of segments each is enough at any size a finger can hit. */
  radius = min_ff(radius, min_ff(BLI_rctf_size_x(&rect), BLI_rctf_size_y(&rect)) * 0.5f);
  const float cx = BLI_rctf_cent_x(&rect);
  const float cy = BLI_rctf_cent_y(&rect);

  const float corner_x[4] = {rect.xmax - radius, rect.xmax - radius, rect.xmin + radius, rect.xmin + radius};
  const float corner_y[4] = {rect.ymin + radius, rect.ymax - radius, rect.ymax - radius, rect.ymin + radius};
  const float corner_start[4] = {-float(M_PI) * 0.5f, 0.0f, float(M_PI) * 0.5f, float(M_PI)};

  float prev_x = 0.0f, prev_y = 0.0f;
  bool has_prev = false;
  float first_x = 0.0f, first_y = 0.0f;

  for (int corner = 0; corner < 4; corner++) {
    for (int i = 0; i <= segments; i++) {
      const float angle = corner_start[corner] + (float(M_PI) * 0.5f) * (float(i) / float(segments));
      const float x = corner_x[corner] + cosf(angle) * radius;
      const float y = corner_y[corner] + sinf(angle) * radius;
      if (has_prev) {
        immVertex2f(pos, cx, cy);
        immVertex2f(pos, prev_x, prev_y);
        immVertex2f(pos, x, y);
      }
      else {
        first_x = x;
        first_y = y;
        has_prev = true;
      }
      prev_x = x;
      prev_y = y;
    }
  }
  immVertex2f(pos, cx, cy);
  immVertex2f(pos, prev_x, prev_y);
  immVertex2f(pos, first_x, first_y);
}

static int vk_round_rect_tris(int segments)
{
  /* One triangle per outline point, the closing one included, over four corners of `segments + 1`
   * points each. Promising fewer than are emitted is what left every key with a torn notch along
   * its bottom edge, which is where the fan closes. */
  return 4 * (segments + 1) * 3;
}

static void vk_draw_round_rect(uint pos, const rctf &rect, float radius, const float color[4])
{
  const int segments = 4;
  immUniformColor4fv(color);
  immBegin(GPU_PRIM_TRIS, uint(vk_round_rect_tris(segments)));
  vk_rect_verts(pos, rect, radius, segments);
  immEnd();
}

static void vk_draw_rect(uint pos, const rctf &rect, const float color[4])
{
  immUniformColor4fv(color);
  immBegin(GPU_PRIM_TRIS, 6);
  immVertex2f(pos, rect.xmin, rect.ymin);
  immVertex2f(pos, rect.xmax, rect.ymin);
  immVertex2f(pos, rect.xmax, rect.ymax);
  immVertex2f(pos, rect.xmin, rect.ymin);
  immVertex2f(pos, rect.xmax, rect.ymax);
  immVertex2f(pos, rect.xmin, rect.ymax);
  immEnd();
}

/* One palette rather than the theme: the keyboard sits above every editor, so it has to read the
 * same whatever theme is behind it. These are Blender's own interface greys and its selection
 * blue. */
static const float VK_COL_PANEL[4] = {0.106f, 0.106f, 0.106f, 0.96f};
static const float VK_COL_PANEL_EDGE[4] = {0.24f, 0.24f, 0.24f, 1.0f};
/* Flat: one colour per cap, and it is the lighter one the old top half carried rather than the
 * darker body underneath it. The keys used to be drawn in three passes -- a dropped shadow, the
 * cap, then a white wash over the top 45% -- which gave them a moulded look that nothing else in
 * Blender has. These values are each of those caps with the wash already folded in, so a key
 * reads at the brightness it always did, in one pass. */
static const float VK_COL_CAP[4] = {0.26f, 0.26f, 0.26f, 1.0f};
static const float VK_COL_CAP_MOD[4] = {0.20f, 0.20f, 0.20f, 1.0f};
static const float VK_COL_CAP_PRESS[4] = {0.314f, 0.475f, 0.717f, 1.0f};
static const float VK_COL_CAP_LOCK[4] = {0.383f, 0.573f, 0.839f, 1.0f};
static const float VK_COL_CAP_CLOSE[4] = {0.478f, 0.202f, 0.183f, 1.0f};
static const float VK_COL_CAP_MOVE[4] = {0.32f, 0.32f, 0.32f, 1.0f};
static const float VK_COL_TEXT[4] = {0.85f, 0.85f, 0.85f, 1.0f};
static const float VK_COL_TEXT_ON[4] = {1.0f, 1.0f, 1.0f, 1.0f};
static const float VK_COL_TEXT_DIM[4] = {0.6f, 0.6f, 0.6f, 1.0f};
/** The character Shift is offering, shown in place of the usual one. */
static const float VK_COL_TEXT_SHIFT[4] = {0.45f, 0.68f, 1.0f, 1.0f};

static bool vk_mod_is_on(const VirtualKeyboard &vk, int slot)
{
  return (vk.mods & (1 << slot)) != 0;
}

static const float *vk_cap_color(const VirtualKeyboard &vk, int index)
{
  const VKKeySpec *spec = vk.keys[index].spec;
  if (index == vk.pressed) {
    return VK_COL_CAP_PRESS;
  }
  switch (spec->kind) {
    case VKKind::Close:
      return VK_COL_CAP_CLOSE;
    case VKKind::Move:
      return vk.moving ? VK_COL_CAP_PRESS : VK_COL_CAP_MOVE;
    case VKKind::Mod:
      /* Touch: the lock colour, because on is now always a lock -- a modifier stays down until it
       * is tapped off. Worth being loud about: a Ctrl left on by accident changes what every
       * other key does. */
      if (vk_mod_is_on(vk, spec->code)) {
        return VK_COL_CAP_LOCK;
      }
      return VK_COL_CAP_MOD;
    case VKKind::Caps:
      return vk.caps ? VK_COL_CAP_LOCK : VK_COL_CAP_MOD;
    case VKKind::Layer:
      return VK_COL_CAP_MOD;
    case VKKind::Key:
      break;
  }
  return VK_COL_CAP;
}

/** A key whose character is a single lowercase letter, which is all Caps Lock reaches. */
static bool vk_key_is_letter(const VKKeySpec &spec)
{
  return spec.utf8 != nullptr && spec.utf8[0] >= 'a' && spec.utf8[0] <= 'z' &&
         spec.utf8[1] == '\0';
}

/** Whether this key would type its shifted character right now. */
static bool vk_shift_for_key(const VirtualKeyboard &vk, const VKKeySpec &spec)
{
  if (vk_mod_is_on(vk, VK_MOD_SHIFT)) {
    return true;
  }
  return vk.caps && vk_key_is_letter(spec);
}

/**
 * The label a key shows right now.
 *
 * With Shift on, a key that types shows the character it will actually produce rather than the one
 * it usually does, and the caller draws it in blue. The alternative was printing both characters
 * on every cap the way a physical keyboard does, which at this size means two glyphs where one is
 * already small: swapping keeps every label in the same place and the same size, and turns the
 * question "what does Shift give me here" into something the keyboard answers by itself.
 *
 * Caps deliberately does not do this. It only reaches letters, whose caps already read as capitals,
 * so there would be nothing to swap and nothing to say.
 */
static const char *vk_key_label(const VirtualKeyboard &vk, const VKKeySpec &spec, bool *r_shifted)
{
  *r_shifted = false;
  if (spec.utf8 == nullptr || spec.utf8_shift == nullptr) {
    return spec.label;
  }
  if (!vk_mod_is_on(vk, VK_MOD_SHIFT)) {
    return spec.label;
  }
  /* Space and the numpad carry the same character either way; there is no second one to show. */
  if (STREQ(spec.utf8, spec.utf8_shift)) {
    return spec.label;
  }
  /* Nor do the letters, whose caps are already printed as capitals: Shift on Q gives the Q that is
   * drawn on it. Compared against the label rather than against the unshifted character for
   * exactly this reason -- the two differ ("q" against "Q") while what the key shows does not, and
   * colouring those would turn the whole keyboard blue and say nothing. */
  if (STREQ(spec.label, spec.utf8_shift)) {
    return spec.label;
  }
  *r_shifted = true;
  return spec.utf8_shift;
}

static const char *vk_mod_name(int slot)
{
  switch (slot) {
    case VK_MOD_CTRL:
      return "Ctrl";
    case VK_MOD_SHIFT:
      return "Shift";
    case VK_MOD_ALT:
      return "Alt";
    default:
      return "";
  }
}

static size_t vk_modifier_text(const VirtualKeyboard &vk, char *buf, size_t buf_size)
{
  buf[0] = '\0';
  size_t offset = 0;
  for (int slot = 0; slot < VK_MOD_NUM; slot++) {
    if (!vk_mod_is_on(vk, slot)) {
      continue;
    }
    if (offset != 0) {
      offset += BLI_strncpy_rlen(buf + offset, " + ", buf_size - offset);
    }
    offset += BLI_strncpy_rlen(buf + offset, vk_mod_name(slot), buf_size - offset);
  }
  return offset;
}

/**
 * What the bar along the top says.
 *
 * While a native field is being edited it shows what is in the field, so typing behind the
 * keyboard is never blind; the drawing follows the tail of it once the text outgrows the bar.
 * Otherwise it names the combination that was just sent, for a moment, then the modifiers being
 * held, and finally the workspace, so the bar always says something about where keys are going.
 */
static void vk_status_text(const VirtualKeyboard &vk,
                           const wmWindow *win,
                           char *buf,
                           size_t buf_size)
{
  if (vk.text_edit != nullptr && *vk.text_edit != nullptr) {
    char mods[64];
    if (vk_modifier_text(vk, mods, sizeof(mods)) != 0) {
      BLI_snprintf(buf, buf_size, "%s + ...   %s|", mods, *vk.text_edit);
    }
    else {
      BLI_snprintf(buf, buf_size, "%s|", *vk.text_edit);
    }
    return;
  }

  if (vk.last_key[0] != '\0' && (BLI_time_now_seconds() - vk.last_key_time) < 1.5) {
    BLI_strncpy(buf, vk.last_key, buf_size);
    return;
  }

  const size_t offset = vk_modifier_text(vk, buf, buf_size);
  if (offset != 0) {
    BLI_strncpy(buf + offset, " + ...", buf_size - offset);
    return;
  }

  const WorkSpace *workspace = WM_window_get_active_workspace(const_cast<wmWindow *>(win));
  if (workspace != nullptr) {
    BLI_strncpy(buf, workspace->id.name + 2, buf_size);
  }
  else {
    buf[0] = '\0';
  }
}

static void vk_draw_cb(const wmWindow *win, void * /*customdata*/)
{
  VirtualKeyboard &vk = g_vk;
  if (!vk.open || vk.win != win) {
    return;
  }
  vk_ensure_layout(vk, const_cast<wmWindow *>(win));
  if (vk.keys.is_empty()) {
    return;
  }

  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);

  GPU_blend(GPU_BLEND_ALPHA);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  rctf panel;
  BLI_rctf_rcti_copy(&panel, &vk.rect);
  vk_draw_rect(pos, panel, VK_COL_PANEL);

  rctf edge = panel;
  edge.ymin = edge.ymax - 2.0f;
  vk_draw_rect(pos, edge, VK_COL_PANEL_EDGE);

  for (const int i : vk.keys.index_range()) {
    rctf cap;
    BLI_rctf_rcti_copy(&cap, &vk.keys[i].rect);
    const float cap_h = BLI_rctf_size_y(&cap);
    const float radius = cap_h * 0.18f;

    vk_draw_round_rect(pos, cap, radius, vk_cap_color(vk, i));
  }

  immUnbindProgram();

  /* Labels. */
  const int font_id = BLF_default();
  for (const int i : vk.keys.index_range()) {
    const VKPlacedKey &key = vk.keys[i];
    if (key.spec->label == nullptr) {
      continue;
    }
    const float cap_h = float(BLI_rcti_size_y(&key.rect));
    /* Bounded above as well as below: a tall portrait key would otherwise carry a label larger
     * than anything else on screen. */
    const float size = clamp_f(cap_h * 0.34f, 9.0f, 16.0f * vk_scale());
    BLF_size(font_id, size);

    const bool on = (i == vk.pressed) ||
                    (key.spec->kind == VKKind::Mod && vk_mod_is_on(vk, key.spec->code)) ||
                    (key.spec->kind == VKKind::Caps && vk.caps);

    bool shifted = false;
    const char *label = vk_key_label(vk, *key.spec, &shifted);
    /* Blue for the second character a key gives, and only for that: what Shift is offering has to
     * be told apart at a glance from what the key says the rest of the time. */
    BLF_color4fv(font_id,
                 on          ? VK_COL_TEXT_ON :
                 shifted     ? VK_COL_TEXT_SHIFT :
                               VK_COL_TEXT);

    const size_t label_len = strlen(label);
    const float text_w = BLF_width(font_id, label, label_len);
    const float x = float(key.rect.xmin) + (float(BLI_rcti_size_x(&key.rect)) - text_w) * 0.5f;
    const float y = float(key.rect.ymin) + (cap_h - size) * 0.5f + size * 0.12f;
    BLF_position(font_id, x, y, 0.0f);
    BLF_draw(font_id, label, label_len);
  }

  /* The bar between the handle and the close button. */
  {
    char status[512];
    vk_status_text(vk, win, status, sizeof(status));

    const VKPlacedKey &handle = vk.keys[1];
    const VKPlacedKey &close = vk.keys[0];
    const float bar_h = float(BLI_rcti_size_y(&handle.rect));
    /* Small on purpose: this is a hint about where the keys are going, not a heading. */
    const float size = min_ff(bar_h * 0.40f, 11.0f * vk_scale());
    BLF_size(font_id, size);
    BLF_color4fv(font_id, (vk.text_edit != nullptr) ? VK_COL_TEXT_ON : VK_COL_TEXT_DIM);

    const float text_x = float(handle.rect.xmax) + bar_h * 0.5f;
    const float avail = float(close.rect.xmin) - bar_h * 0.5f - text_x;

    /* Follow the end of the text once it no longer fits, so what was just typed stays visible and
     * nothing ever runs under the close button. */
    const char *text = status;
    while (*text != '\0' && BLF_width(font_id, text, strlen(text)) > avail) {
      text += BLI_str_utf8_size_safe(text);
    }

    BLF_position(font_id,
                 text_x,
                 float(handle.rect.ymin) + (bar_h - size) * 0.5f + size * 0.12f,
                 0.0f);
    BLF_draw(font_id, text, strlen(text));
  }

  BLF_batch_draw_flush();
  GPU_blend(GPU_BLEND_NONE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sending keys
 * \{ */

static GHOST_TKey vk_modifier_ghost_key(int slot)
{
  switch (slot) {
    case VK_MOD_CTRL:
      return GHOST_kKeyLeftControl;
    case VK_MOD_SHIFT:
      return GHOST_kKeyLeftShift;
    case VK_MOD_ALT:
      return GHOST_kKeyLeftAlt;
    default:
      return GHOST_kKeyUnknown;
  }
}

/**
 * The timestamp injected keys carry.
 *
 * Stepping past the double click window every time, because tapping the same key twice in a row is
 * ordinary on a touch keyboard while two presses inside #UserDef.dbl_click_time turn the second
 * into #KM_DBL_CLICK, which almost no keymap item matches. That is what made a shortcut work one
 * tap and do nothing the next.
 */
static uint64_t vk_event_time_ms()
{
  static uint64_t clock = 0;
  clock += uint64_t(U.dbl_click_time) + 100;
  return clock;
}

static void vk_send_ghost_key(wmWindowManager *wm,
                              wmWindow *win,
                              GHOST_TKey key,
                              const char *utf8,
                              bool down)
{
  GHOST_TEventKeyData kdata = {};
  kdata.key = key;
  kdata.is_repeat = false;
  if (down && utf8 != nullptr) {
    BLI_strncpy(kdata.utf8_buf, utf8, sizeof(kdata.utf8_buf));
  }
  else {
    kdata.utf8_buf[0] = '\0';
  }
  wm_event_add_ghostevent(
      wm, win, down ? GHOST_kEventKeyDown : GHOST_kEventKeyUp, &kdata, vk_event_time_ms());
}

/**
 * Put the window manager's modifier state where #VirtualKeyboard::mods says it should be.
 *
 * One key event per modifier that changed, and then it is left alone: a modifier turned on stays
 * down until it is turned off. #wmEvent::modifier is carried forward from the event state, so
 * from that moment every event carries it -- the next key from this keyboard, and equally the
 * next touch anywhere else. That is what makes Shift and a tap on a face extend a selection, and
 * Ctrl and a tap on an object add to one.
 *
 * The one thing that undoes it from outside is wm_window_update_eventstate_modifiers(), which
 * re-reads the real modifier state from GHOST and releases anything the event state holds that
 * the hardware does not. It runs when the window is activated, on a completed drag and drop, and
 * on a button event that arrives while the window is inactive. None of those happen in the middle
 * of ordinary use, and if one does the modifier simply lets go, which is the safe direction.
 */
static uint8_t vk_mods_from_event_state(const wmWindow *win)
{
  const uint8_t held = (win->runtime->eventstate != nullptr) ?
                           uint8_t(win->runtime->eventstate->modifier) :
                           uint8_t(0);
  uint8_t bits = 0;
  if (held & KM_CTRL) {
    bits |= uint8_t(1 << VK_MOD_CTRL);
  }
  if (held & KM_SHIFT) {
    bits |= uint8_t(1 << VK_MOD_SHIFT);
  }
  if (held & KM_ALT) {
    bits |= uint8_t(1 << VK_MOD_ALT);
  }
  return bits;
}

static void vk_sync_ghost_mods(VirtualKeyboard &vk, wmWindowManager *wm, wmWindow *win)
{
  /* Read what is actually held rather than remembering what was sent.
   *
   * The keyboard is not the only thing that presses a modifier. The three finger viewport pan
   * sends Shift through GHOST as well (touchSendShift(), GHOST_SystemAndroid.cc), and anything
   * that leaves one down -- a gesture that ended by a path that did not release it, a window
   * activation that re-read the hardware state -- used to be invisible here.
   *
   * A cache of what this keyboard had sent could only ever release what this keyboard had
   * pressed, so a Shift left down by anything else was unreachable: the caps showed it off, the
   * sync agreed there was nothing to release, and every click in the program carried it. That is
   * the state someone had to make a new file to escape.
   *
   * Asking the window instead makes this self-correcting in both directions. */
  const uint8_t actual = vk_mods_from_event_state(win);
  if (actual == vk.mods) {
    return;
  }
  /* Press before release, so a combination is never briefly empty. */
  for (int slot = 0; slot < VK_MOD_NUM; slot++) {
    const uint8_t bit = uint8_t(1 << slot);
    if ((vk.mods & bit) && !(actual & bit)) {
      vk_send_ghost_key(wm, win, vk_modifier_ghost_key(slot), nullptr, true);
    }
  }
  for (int slot = VK_MOD_NUM - 1; slot >= 0; slot--) {
    const uint8_t bit = uint8_t(1 << slot);
    if (!(vk.mods & bit) && (actual & bit)) {
      vk_send_ghost_key(wm, win, vk_modifier_ghost_key(slot), nullptr, false);
    }
  }
}

/**
 * Let go of every modifier the window is holding, whoever pressed it.
 *
 * Called when the keyboard opens as well as when it closes, which is what makes opening it the
 * way out of a stuck modifier: a Ctrl left down with nothing on screen to turn it off changes
 * what every click in the program does, and with "Emulate 3 Button Mouse" on a stuck Alt turns
 * every tap into a middle click, which reads as the interface having died.
 */
static void vk_release_all_mods(VirtualKeyboard &vk, wmWindowManager *wm, wmWindow *win)
{
  vk.mods = 0;
  if (wm != nullptr) {
    vk_sync_ghost_mods(vk, wm, win);
  }
}

/**
 * Turn a tapped key into the press and release a hardware keyboard would have sent.
 *
 * The modifiers are already down by the time this runs -- see vk_sync_ghost_mods() -- so the key
 * event carries them and the keymap resolves the combination exactly as it would for a physical
 * one. They stay down afterwards, which is the difference between tapping Ctrl once and growing
 * a selection, and tapping it again before every single NumpadPlus.
 */
static void vk_send_key(VirtualKeyboard &vk, wmWindowManager *wm, wmWindow *win, const VKKeySpec &spec)
{
  /* A key event inherits the cursor position, and that is what decides which editor the shortcut
   * reaches. Attribute it to the last place the user actually touched, never to the keyboard. */
  int target[2];
  vk_target_position(vk, win, target);
  copy_v2_v2_int(win->runtime->eventstate->xy, target);

  const bool shift = vk_shift_for_key(vk, spec);
  const char *utf8 = shift ? spec.utf8_shift : spec.utf8;
  /* A key that types is the one case where a latched Shift is a nuisance rather than a help. */
  const bool key_types_text = (spec.utf8 != nullptr);

  /* Name it for the bar, so a tap says what it sent even when the key is under a finger. */
  {
    char mods[64];
    if (vk_modifier_text(vk, mods, sizeof(mods)) != 0) {
      BLI_snprintf(vk.last_key, sizeof(vk.last_key), "%s + %s", mods, spec.label);
    }
    else {
      BLI_strncpy(vk.last_key, spec.label, sizeof(vk.last_key));
    }
    vk.last_key_time = BLI_time_now_seconds();
  }

  vk_sync_ghost_mods(vk, wm, win);

  vk_send_ghost_key(wm, win, GHOST_TKey(spec.code), utf8, true);
  vk_send_ghost_key(wm, win, GHOST_TKey(spec.code), nullptr, false);

  if (key_types_text && vk_mod_is_on(vk, VK_MOD_SHIFT)) {
    vk.mods &= uint8_t(~(1 << VK_MOD_SHIFT));
    vk_sync_ghost_mods(vk, wm, win);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Open and close
 * \{ */

static void vk_close(wmWindowManager *wm, wmWindow *win)
{
  VirtualKeyboard &vk = g_vk;
  if (!vk.open) {
    return;
  }
  vk_release_all_mods(vk, wm, win);
  if (vk.draw_handle != nullptr) {
    WM_draw_cb_exit(vk.win, vk.draw_handle);
    vk.draw_handle = nullptr;
  }
  vk.open = false;
  vk.win = nullptr;
  /* Caps holds for as long as the keyboard is up, and no longer: coming back to a keyboard that
   * types capitals because of a tap from an earlier session would be a puzzle with no clue on
   * screen until the first letter arrives wrong. */
  vk.caps = false;
  vk.keys.clear();
  vk.text_edit = nullptr;
  vk.last_key[0] = '\0';
  vk.hover = -1;
  vk.pressed = -1;
  vk.moving = false;
  vk_tag_redraw(win);
}

static void vk_open(wmWindowManager *wm, wmWindow *win)
{
  VirtualKeyboard &vk = g_vk;
  if (vk.open) {
    return;
  }
  /* Opening the keyboard is the way out of a stuck modifier, whoever left it down. Nothing else
   * on a phone can release one, and the state it produces looks like the interface has stopped
   * responding rather than like a key being held. */
  vk_release_all_mods(vk, wm, win);
  vk.open = true;
  vk.win = win;
  vk.layer = 0;
  vk.offset = 0.0f;
  vk.text_edit = nullptr;
  vk.last_key[0] = '\0';
  vk.mods = 0;
  vk.hover = -1;
  vk.pressed = -1;
  vk.moving = false;
  copy_v2_v2_int(vk.pinned, win->runtime->eventstate->xy);
  vk_build_layout(vk, win);
  vk.draw_handle = WM_draw_cb_activate(win, vk_draw_cb, nullptr);
  vk_tag_redraw(win);
}

void WM_virtual_keyboard_text_edit_begin(const wmWindow *win, const char *const *string)
{
  if (g_vk.open && g_vk.win == win) {
    g_vk.text_edit = string;
    vk_tag_redraw(g_vk.win);
  }
}

void WM_virtual_keyboard_text_edit_end(const wmWindow *win)
{
  if (g_vk.text_edit != nullptr && g_vk.win == win) {
    g_vk.text_edit = nullptr;
    vk_tag_redraw(g_vk.win);
  }
}

bool WM_virtual_keyboard_is_open(const wmWindow *win)
{
  return g_vk.open && (win == nullptr || g_vk.win == win);
}

bool WM_virtual_keyboard_rect_get(const wmWindow *win, rcti *r_rect)
{
  if (!WM_virtual_keyboard_is_open(win)) {
    return false;
  }
  *r_rect = g_vk.rect;
  return true;
}

void WM_virtual_keyboard_toggle(wmWindowManager *wm, wmWindow *win)
{
  if (g_vk.open) {
    vk_close(wm, win);
  }
  else {
    vk_open(wm, win);
  }
}

void wm_virtual_keyboard_window_close(wmWindow *win)
{
  if (g_vk.open && g_vk.win == win) {
    /* The draw callback belongs to a window that is going away, so drop it without touching it. */
    g_vk.draw_handle = nullptr;
    g_vk.open = false;
    g_vk.win = nullptr;
    /* The window is going away and takes its event state with it, so there is nothing to
     * release the modifiers to. */
    g_vk.mods = 0;
    g_vk.keys.clear();
    g_vk.text_edit = nullptr;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Input
 * \{ */

static void vk_press(VirtualKeyboard &vk, wmWindow *win, int index)
{
  vk.pressed = index;
  vk.moving = (index != -1) && (vk.keys[index].spec->kind == VKKind::Move);
  copy_v2_v2_int(vk.drag_prev, vk.cursor);
  vk_tag_redraw(win);
}

static void vk_release(VirtualKeyboard &vk, wmWindowManager *wm, wmWindow *win)
{
  const int index = vk.pressed;
  const bool was_moving = vk.moving;
  vk.pressed = -1;
  vk.moving = false;
  if (index == -1) {
    vk_tag_redraw(win);
    return;
  }

  const VKKeySpec &spec = *vk.keys[index].spec;

  /* A finger that slid off the key it started on cancels, as it does on any keyboard. */
  if (!was_moving && BLI_rcti_isect_pt_v(&vk.keys[index].rect, vk.cursor)) {
    switch (spec.kind) {
      case VKKind::Close:
        vk_close(wm, win);
        return;
      case VKKind::Move:
        break;
      case VKKind::Layer:
        vk.layer = spec.code;
        break;
      case VKKind::Mod: {
        /* Touch: a tap toggles, and it stays. Ctrl once and then NumpadPlus as many times as the
         * selection needs; Ctrl again to let go. Two or three can be on together, and each is
         * drawn in the lock colour while it is. */
        vk.mods ^= uint8_t(1 << spec.code);
        vk_sync_ghost_mods(vk, wm, win);
        break;
      }
      case VKKind::Caps:
        /* Nothing to sync: it never reaches the window's modifier state, only the character a
         * letter key hands over. */
        vk.caps = !vk.caps;
        break;
      case VKKind::Key:
        vk_send_key(vk, wm, win, spec);
        break;
    }
  }
  vk_tag_redraw(win);
}

/**
 * Whether a popup drawn over the keyboard owns this point.
 *
 * A menu or a pie opened while the keyboard is up is drawn above it, and the part of it that falls
 * over the panel has to stay reachable. Answered by position rather than by the mere presence of a
 * popup, so a search box opened while typing takes only the taps that land on it and the keys
 * around it keep working.
 *
 * A running modal operator deliberately does *not* count, which was learned the hard way. Standing
 * aside for one meant that starting, say, circle select from the keyboard left the whole interface
 * unusable: the operator holds every pointer event until it is confirmed or cancelled, and Esc is
 * on the keyboard that had just gone inert. On a device with no other keyboard the only way out
 * was to kill the application. Losing part of a control that draws under the panel is a nuisance;
 * being unable to leave it is not.
 *
 * Text editing is not included either. It runs as a UI handler rather than as an operator, which
 * is exactly why the keyboard can type into it.
 */
static bool vk_point_is_owned(const wmWindow *win, const int xy[2])
{
  const bScreen *screen = WM_window_get_active_screen(const_cast<wmWindow *>(win));
  if (screen != nullptr) {
    for (const ARegion &region : screen->regionbase) {
      if (region.runtime->visible && BLI_rcti_isect_pt_v(&region.winrct, xy)) {
        return true;
      }
    }
  }
  return false;
}

bool wm_virtual_keyboard_ghost_event(wmWindowManager *wm,
                                     wmWindow *win,
                                     const int type,
                                     const void *customdata)
{
  VirtualKeyboard &vk = g_vk;
  if (!vk.open || vk.win != win) {
    return false;
  }
  vk_ensure_layout(vk, win);

  switch (type) {
    case GHOST_kEventCursorMove: {
      const GHOST_TEventCursorData *cd = static_cast<const GHOST_TEventCursorData *>(customdata);
      int xy[2] = {cd->x, cd->y};
      wm_cursor_position_from_ghost_screen_coords(win, &xy[0], &xy[1]);
      copy_v2_v2_int(vk.cursor, xy);

      if (vk.moving) {
        const int dy = xy[1] - vk.drag_prev[1];
        copy_v2_v2_int(vk.drag_prev, xy);
        if (dy != 0) {
          vk.offset += float(dy);
          vk_ensure_layout(vk, win);
          vk_tag_redraw(win);
        }
        return true;
      }

      if (!BLI_rcti_isect_pt_v(&vk.rect, xy)) {
        /* Outside: remember where, so an injected key lands in that editor, and let the move
         * through untouched. */
        if (vk_position_in_area(win, xy)) {
          copy_v2_v2_int(vk.pinned, xy);
        }
        return false;
      }
      if (vk_point_is_owned(win, xy)) {
        return false;
      }

      const int hover = vk_key_at(vk, xy);
      if (hover != vk.hover) {
        vk.hover = hover;
        vk_tag_redraw(win);
      }
      return true;
    }
    case GHOST_kEventButtonDown:
    case GHOST_kEventButtonUp: {
      const GHOST_TEventButtonData *bd = static_cast<const GHOST_TEventButtonData *>(customdata);
      const bool on_panel = BLI_rcti_isect_pt_v(&vk.rect, vk.cursor) &&
                            !vk_point_is_owned(win, vk.cursor);
      /* Touch: only the left button presses a key.
       *
       * A finger held still arrives as a right click instead --
       * GHOST_SystemAndroid::touchLongPressCheck() turns a stationary press into one at
       * TOUCH_LONG_PRESS_MS and cancels the pending left press to do it -- so a held key does
       * nothing here. That is deliberate. Taking the right click as a key press was tried and
       * removed: it made press-and-hold work and it also made the keyboard unpredictable enough
       * to be worse than not having the gesture. A tap toggles a modifier and it stays on, which
       * is all the gesture was ever reaching for. */
      if (bd->button != GHOST_kButtonMaskLeft) {
        return on_panel;
      }
      if (type == GHOST_kEventButtonDown) {
        if (!on_panel) {
          return false;
        }
        vk_press(vk, win, vk_key_at(vk, vk.cursor));
        return true;
      }
      /* A release always ends the press this keyboard owns, even if the finger has wandered off
       * the panel: dropping it would leave a key stuck down. Anything else is only ours if the
       * press was, which is what keeps a menu drawn over the panel usable: its items act on the
       * release, and swallowing that left them highlighted but never chosen. */
      if (vk.pressed == -1 && !vk.moving) {
        return on_panel;
      }
      vk_release(vk, wm, win);
      return true;
    }
    case GHOST_kEventTrackpad: {
      /* A two finger gesture that starts on the keyboard must not scroll the editor behind it. */
      return BLI_rcti_isect_pt_v(&vk.rect, vk.cursor) && !vk_point_is_owned(win, vk.cursor);
    }
    default:
      break;
  }
  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operator
 * \{ */

static wmOperatorStatus wm_virtual_keyboard_toggle_exec(bContext *C, wmOperator * /*op*/)
{
  wmWindow *win = CTX_wm_window(C);
  if (win == nullptr) {
    return OPERATOR_CANCELLED;
  }
  WM_virtual_keyboard_toggle(CTX_wm_manager(C), win);
  return OPERATOR_FINISHED;
}

void WM_OT_virtual_keyboard_toggle(wmOperatorType *ot)
{
  ot->name = "Toggle Virtual Keyboard";
  ot->idname = "WM_OT_virtual_keyboard_toggle";
  ot->description =
      "Show or hide the on-screen keyboard. While it is open it replaces the platform keyboard "
      "and can send shortcuts and modifiers as well as text";

  ot->exec = wm_virtual_keyboard_toggle_exec;
  ot->poll = WM_operator_winactive;

  ot->flag = OPTYPE_INTERNAL;
}

/** \} */

}  // namespace blender
