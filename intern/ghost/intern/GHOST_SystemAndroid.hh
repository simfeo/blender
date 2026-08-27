/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 * Declaration of GHOST_SystemAndroid class.
 */

#pragma once

#include "GHOST_Buttons.hh"
#include "GHOST_System.hh"

#include <mutex>
#include <string>
#include <vector>

struct android_app;
struct AInputEvent;
class GHOST_WindowAndroid;

class GHOST_SystemAndroid : public GHOST_System {
 public:
  GHOST_SystemAndroid();
  ~GHOST_SystemAndroid() override;

  /* Set by the NativeActivity glue before GHOST is created. */
  static void setAndroidApp(android_app *app);
  static android_app *getAndroidApp();

  bool processEvents(bool waitForEvent) override;

  bool setConsoleWindowState(GHOST_TConsoleWindowState /*action*/) override
  {
    return false;
  }

  GHOST_TSuccess getModifierKeys(GHOST_ModifierKeys &keys) const override;
  GHOST_TSuccess getButtons(GHOST_Buttons &buttons) const override;
  GHOST_TCapabilityFlag getCapabilities() const override;

  char *getClipboard(bool selection) const override;
  void putClipboard(const char *buffer, bool selection) const override;

  uint64_t getMilliSeconds() const override;

  uint8_t getNumDisplays() const override;
  void getMainDisplayDimensions(uint32_t &width, uint32_t &height) const override;
  void getAllDisplayDimensions(uint32_t &width, uint32_t &height) const override;

  GHOST_TSuccess getCursorPosition(int32_t &x, int32_t &y) const override;
  GHOST_TSuccess setCursorPosition(int32_t x, int32_t y) override;

  GHOST_IContext *createOffscreenContext(GHOST_GPUSettings gpu_settings) override;
  GHOST_TSuccess disposeContext(GHOST_IContext *context) override;

  uint16_t getDPIHint();

  /* Soft keyboard via NativeActivity (no JNI). Note: with NativeActivity the
   * typed text is not delivered back; hardware keys still work as key events. */
  GHOST_TSuccess popupOnScreenKeyboard(GHOST_IWindow *window) override;
  GHOST_TSuccess hideOnScreenKeyboard(GHOST_IWindow *window) override;

  /* Called by the glue when Android (re)creates or destroys the surface. */
  void handleNativeWindowInit(android_app *app);
  void handleNativeWindowResize();
  /** Tell the window manager the window is focused, so it stops treating every press as an
   * entry into an inactive window. */
  void handleWindowFocus(bool gained);
  void handleNativeWindowTerm();

  /* Translate a native input event into GHOST events. Return 1 if handled. */
  int32_t handleInputEvent(AInputEvent *event);

  /* Soft-keyboard input forwarded from the Java InputConnection bridge. */
  void handleTextInput(const char *utf8_string);
  void handleJavaKeyEvent(int32_t keycode, int32_t action, int32_t meta_state);

 private:
  GHOST_TSuccess init() override;

  GHOST_IWindow *createWindow(const char *title,
                              int32_t left,
                              int32_t top,
                              uint32_t width,
                              uint32_t height,
                              GHOST_TWindowState state,
                              GHOST_GPUSettings gpu_settings,
                              const bool exclusive = false,
                              const bool is_dialog = false,
                              const GHOST_IWindow *parent_window = nullptr) override;

  int32_t handleKeyEvent(AInputEvent *event);
  int32_t handleMotionEvent(AInputEvent *event);

  android_app *app_;
  GHOST_WindowAndroid *window_;
  uint64_t start_time_;

  /* Input state tracked from events for the out-of-queue queries. */
  GHOST_Buttons buttons_;
  int32_t meta_state_;
  int32_t cursor_x_, cursor_y_;

  /* S Pen side button, tracked so it works while hovering or touching. */
  bool stylus_button_down_;

  /* Two-finger gesture tracking (pan/pinch), computed from raw pointers. */
  bool gesture_active_;
  float gesture_prev_x_, gesture_prev_y_, gesture_prev_dist_;
  /* Where the two-finger gesture started, so pan and zoom can be told apart
   * from the movement so far rather than from one frame's jitter. */
  float gesture_start_x_, gesture_start_y_, gesture_start_dist_;
  /* Undecided until the gesture has moved far enough to say which it is; then
   * it stays that way until the fingers lift. */
  enum class GestureKind { Undecided, Pan, Zoom };
  GestureKind gesture_kind_;

  /* Three fingers emulate a Shift+middle-mouse drag, Blender's "Move the view" binding, so touch
   * users get a dedicated pan. Two fingers already orbit (a trackpad pan is what the 3D View maps
   * to a rotate), and one-finger drags stay available to Blender's buttons, sliders and editor
   * borders. */
  bool touch_pan_active_;
  /* Whether the synthetic Shift that turns that middle-mouse drag into a pan is held. Tracked so
   * it can be mirrored into meta_state_: with no window-activate event on Android win->active
   * stays 0, so the window manager re-reads getModifierKeys() on every button event and would
   * otherwise cancel the modifier right back out. */
  bool touch_shift_active_;

  /* Finger touch: the button is decided after the press, so that holding still
   * becomes a right-click while moving or a quick tap stays a left-click. */
  void touchLongPressCheck();
  /* Parks the cursor once a stylus has really left, see hover_exit_pending_. */
  void hoverExitCheck();
  void touchSendButton(GHOST_TButton mask, GHOST_TEventType type);
  /* Press/release the synthetic Shift, keeping meta_state_ in step. */
  void touchSendShift(bool down);
  /* Release the three-finger pan, if one is running. */
  void touchEndPan();
  void touchCancelPending();
  bool touch_pending_;          /* Finger down, button not decided yet. */
  bool touch_button_down_;      /* A button was emitted and is still held. */
  GHOST_TButton touch_button_;  /* Which button was emitted. */
  /* Tablet state of the pointer the buttons belong to. Blender picks the drag
   * threshold from this, and a stylus needs the larger tablet one: judged as a
   * mouse, normal tremor between contact and lift is read as a drag, so the
   * release never becomes a click and menu entries do not fire. */
  GHOST_TabletData touch_tablet_;
  /* Android reports a hover exit both when the stylus leaves range and just
   * before its tip touches down, and the two are indistinguishable at the time.
   * Parking the cursor immediately closed any open menu before the press could
   * land on it, so the park waits to see whether contact follows. */
  bool hover_exit_pending_;
  uint64_t hover_exit_time_;
  uint64_t touch_down_time_;
  int32_t touch_down_x_, touch_down_y_;

  /* The Java IME callbacks run on the Android UI thread, while the event queue
   * is only safe to touch from Blender's thread, so they are queued and drained
   * in processEvents(). */
  struct JavaKeyEvent {
    int32_t keycode, action, meta_state;
  };
  void drainJavaInput();
  void dispatchTextInput(const char *utf8_string);
  void dispatchJavaKeyEvent(int32_t keycode, int32_t action, int32_t meta_state);
  std::mutex java_input_mutex_;
  std::vector<std::string> java_text_;
  std::vector<JavaKeyEvent> java_keys_;
};
