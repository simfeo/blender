/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

#include <optional>

#include "BKE_report.hh"

#include "DNA_windowmanager_types.h"

#include "BLI_set.hh"
#include "BLI_vector.hh"

namespace blender {

struct UndoStack;
struct wmMsgBus;
struct wmKeyConfig;
struct wmEvent;
struct wmWindow;
struct wmIMEData;
struct wmGesture;
struct wmJob;
struct wmDrag;
struct wmPaintCursor;
struct WindowDrawCB;

namespace bke {

struct wmNotifierHashForQueue {
  uint64_t operator()(const wmNotifier *note) const;
};
struct wmNotifierEqForQueue {
  bool operator()(const wmNotifier *a, const wmNotifier *b) const;
};
using wmNotifierQueueSet = Set<const wmNotifier *,
                               4,
                               DefaultProbingStrategy,
                               wmNotifierHashForQueue,
                               wmNotifierEqForQueue>;

struct WindowManagerRuntime {
  /** Separate active from drawable. */
  wmWindow *windrawable = nullptr;
  /**
   * \note `CTX_wm_window(C)` is usually preferred.
   * Avoid relying on this where possible as this may become NULL during when handling
   * events that close or replace windows (e.g. opening a file).
   * While this happens rarely in practice, it can cause difficult to reproduce bugs.
   */
  wmWindow *winactive = nullptr;

  /** Indicates whether interface is locked for user interaction. */
  bool is_interface_locked = false;

  /** Indicates whether modified images should be saved when saving the blend file. */
  char save_modified_images_when_file_is_saved = true;

  /**
   * Indicates the main loop (#WM_main()) to stop processing the event queue and move to the next
   * step. The Remaining events will then be processed during the next iteration of the loop.
   *
   * This is used e.g. to avoid handling events immediately after an undo/redo action, when UI has
   * not yet been updated.
   */
  bool break_events_handling = false;

  /** Information and error reports. */
  ReportList reports;

  /**
   * Refresh/redraw #wmNotifier structs.
   * \note Once in the queue, notifiers should be considered read-only.
   * With the exception of clearing notifiers for data which has been removed,
   * see: #NOTE_CATEGORY_TAG_CLEARED.
   */
  ListBaseT<wmNotifier> notifier_queue = {nullptr, nullptr};
  /**
   * For duplicate detection.
   * \note keep in sync with `notifier_queue` adding/removing elements must also update this set.
   */
  wmNotifierQueueSet notifier_queue_set;

  /** The current notifier in the `notifier_queue` being handled (clear instead of freeing). */
  const wmNotifier *notifier_current = nullptr;

  /** Operator registry. */
  ListBaseT<wmOperator> operators = {nullptr, nullptr};

  /** Extra overlay cursors to draw, like circles. */
  ListBaseT<wmPaintCursor> paintcursors = {nullptr, nullptr};

  /**
   * Known key configurations.
   * This includes all the #wmKeyConfig members (`defaultconf`, `addonconf`, etc).
   */
  ListBaseT<wmKeyConfig> keyconfigs = {nullptr, nullptr};

  /** Active timers. */
  ListBaseT<wmTimer> timers = {nullptr, nullptr};

  /** Threaded jobs manager. */
  ListBaseT<wmJob> jobs = {nullptr, nullptr};

  /** Active dragged items. */
  ListBaseT<wmDrag> drags = {nullptr, nullptr};

  /** Default configuration. */
  wmKeyConfig *defaultconf = nullptr;

  /** Addon configuration. */
  wmKeyConfig *addonconf = nullptr;

  /** User configuration. */
  wmKeyConfig *userconf = nullptr;

  /**
   * All undo history.
   *
   * \note This will be null in background mode unless explicitly created.
   */
  UndoStack *undo_stack = nullptr;

  wmMsgBus *message_bus = nullptr;

  WindowManagerRuntime();
  ~WindowManagerRuntime();
};

/**
 * The kind of UI element that began the window's IME session, see #WindowRuntime::ime_owner.
 *
 * Popups store the type of session with the element that began them.
 * Avoids buttons/regions acting on an IME popup it doesn't own.
 */
enum class wmIMEOwnerType : int8_t {
  /**
   * A region's `cursor_ime` callback (via #WM_window_IME_region_refresh),
   * always the screen's active region.
   */
  Region,
  /** A text button being edited (which may be in a popup floating over an unrelated region). */
  Button,
};

struct WindowRuntime {
  /** All events #wmEvent (ghost level events were handled). */
  ListBaseT<wmEvent> event_queue = {nullptr, nullptr};

  /**
   * Input Method Editor data - complex character input (especially for Asian character input)
   * Only used when `WITH_INPUT_IME` is defined.
   */
  wmIMEData *ime_data = nullptr;
  /**
   * The UI element this session was begun with.
   * Set by #WM_window_IME_begin, cleared by #WM_window_IME_end.
   */
  std::optional<wmIMEOwnerType> ime_owner;
  /**
   * True while the user is composing text via an IME
   * (set by #WM_IME_COMPOSITE_START, cleared by #WM_IME_COMPOSITE_END or #WM_window_IME_end).
   *
   * Operators
   * =========
   *
   * When true, text input operators that support IME are skipped (returning canceled).
   * This is needed because keys that edit the composition (backspace, enter, arrow-keys... etc)
   * are consumed by the IME but *also* pass-through as regular key events,
   * which the key-map handles as usual: a backspace that removes a pre-edit
   * character would also delete committed text.
   * Theoretically these events could be filtered out before being handled,
   * however only the IME knows which keys it consumes and this isn't exposed by any API,
   * so operators must skip them while composing.
   * Canceling instead of passing the event through prevents other key-map items from matching it.
   *
   * See:
   * - #WM_operator_IME_edit_maybe
   * - #WM_operator_IME_insert_maybe
   */
  bool ime_data_is_composing = false;

  /* Optionally store the size and position using this key in the user's recents file. */
  std::string recents_storage_key = {};

  /** Don't want to include ghost.h stuff. */
  void *ghostwin = nullptr;

  /** Don't want to include gpu stuff. */
  void *gpuctx = nullptr;

  /** Window+screen handlers, handled last. */
  ListBaseT<wmEventHandler> handlers = {nullptr, nullptr};

  /** Priority handlers, handled first. */
  ListBaseT<wmEventHandler> modalhandlers = {nullptr, nullptr};

  /** Custom drawing callbacks. */
  ListBaseT<WindowDrawCB> drawcalls = {nullptr, nullptr};

  /** Gesture stuff. */
  ListBaseT<wmGesture> gesture = {nullptr, nullptr};

  /**
   * Keep the last handled event in `event_queue` here (owned and must be freed).
   *
   * \warning This must only to be used for event queue logic.
   * User interactions should use `eventstate` instead (if the event isn't passed to the function).
   */
  wmEvent *event_last_handled = nullptr;

  /**
   * Storage for event system.
   *
   * For the most part this is storage for `wmEvent.xy` & `wmEvent.modifiers`.
   * newly added key/button events copy the cursor location and modifier state stored here.
   *
   * It's also convenient at times to be able to pass this as if it's a regular event.
   *
   * - This is not simply the current event being handled.
   *   The type and value is always set to the last press/release events
   *   otherwise cursor motion would always clear these values.
   *
   * - The value of `eventstate->modifiers` is set from the last pressed/released modifier key.
   *   This has the down side that the modifier value will be incorrect if users hold both
   *   left/right modifiers then release one. See note in #wm_event_add_ghostevent for details.
   */
  wmEvent *eventstate = nullptr;

  /**
   * A one-finger touch drag generates mouse motion/button events and trackpad-style pan events.
   * This mode determines which events are passed to Blender's handlers.
   * In Navigate2D and Navigate3D modes, mouse events allow dragging gizmos but cannot activate
   * selection tools. If a gizmo takes the drag, view navigation is suppressed.
   */
  enum class TouchDragMode {
    /** Pass both mouse and pan events to the existing UI scrolling and dragging handlers. */
    Default,
    /** Pass mouse events and discard pan events, allowing content and widget dragging. */
    Mouse,
    /** Discard mouse and trackpad events for this touch drag. */
    Ignore,
    /** Navigate 2D editors: one finger pans, two fingers pan and zoom. */
    Navigate2D,
    /** Navigate the 3D viewport: one finger orbits, two fingers pan and zoom. */
    Navigate3D,
  };

  struct ModifierWheel {
    enum class Button { None, Handle, Ctrl, Shift, Alt, Favorites, Confirm, Cancel };
    struct Contact {
      uint64_t device, id;
      Button button;
      float start[2];
      float center[2];
      bool moved = false;
      bool pointer = false;
    };
    bool enabled = false;
    bool initialized = false;
    bool collapsed = false;
    bool show_confirm = false;
    bool show_cancel = false;
    float center[2] = {-1.0f, -1.0f};
    uint8_t modifiers = 0;
    Vector<Contact> contacts;
    int pointer_xy[2] = {};
    uint32_t pointer_buttons = 0;
    /** Consume button releases even after the wheel is disabled. */
    uint32_t captured_buttons = 0;
    Button hover = Button::None;
  } modifier_wheel;

  /** Hardware modifiers are independent of the modifier wheel's latched keys. */
  uint8_t physical_modifiers = 0;

  /** Classification captured when a touch-screen drag begins. */
  bool touch_drag_active = false;
  TouchDragMode touch_drag_mode = TouchDragMode::Default;
  /** A gizmo claimed the touch press, preventing view navigation for the rest of the drag. */
  bool touch_gizmo_claimed = false;

  /** Hover is disabled after touch input or pen proximity leave until the next pointer input. */
  bool cursor_hover_enabled = true;
  /** Whether the last pointer input came from the touchscreen. */
  bool cursor_is_touch = false;

  /**
   * The time when the key is pressed in milliseconds (see #GHOST_IEvent::getTime).
   * Used to detect double-click events.
   */
  uint64_t eventstate_prev_press_time_ms = 0;

  /** Private runtime info to show text in the status bar. */
  void *cursor_keymap_status = nullptr;

  WindowRuntime() = default;
  ~WindowRuntime();
};

}  // namespace bke
}  // namespace blender
