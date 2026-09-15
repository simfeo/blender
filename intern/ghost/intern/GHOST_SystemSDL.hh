/* SPDX-FileCopyrightText: 2011-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 * Declaration of GHOST_SystemSDL class.
 */

#pragma once

#include "../GHOST_Types.hh"
#include "GHOST_Event.hh"
#include "GHOST_System.hh"
#include "GHOST_TimerManager.hh"
#include "GHOST_WindowSDL.hh"

#include <SDL3/SDL.h>

class GHOST_WindowSDL;

class GHOST_SystemSDL : public GHOST_System {
 public:
  void addDirtyWindow(GHOST_WindowSDL *bad_wind);

  GHOST_SystemSDL();
  ~GHOST_SystemSDL();

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

  GHOST_TSuccess getCursorPosition(int32_t &x, int32_t &y) const override;

  GHOST_TSuccess setCursorPosition(int32_t x, int32_t y) override;

  void getAllDisplayDimensions(uint32_t &width, uint32_t &height) const override;

  void getMainDisplayDimensions(uint32_t &width, uint32_t &height) const override;

  GHOST_IContext *createOffscreenContext(GHOST_GPUSettings gpu_settings) override;

  GHOST_TSuccess disposeContext(GHOST_IContext *context) override;

 private:
#ifdef WITH_VULKAN_BACKEND
  static bool SDLCALL vulkanLifecycleEventWatch(void *userdata, SDL_Event *event);

  Uint32 vulkan_surface_recreation_event_ = 0;
#endif

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

  /* SDL specific */
  GHOST_WindowSDL *findGhostWindow(SDL_Window *sdl_win);

  bool generateWindowExposeEvents();

  void processEvent(SDL_Event *sdl_event);
  void processPenEvent(const SDL_Event &sdl_event);

  GHOST_TabletData pen_tablet_data_ = {};

#ifdef __ANDROID__
  void processTouchEvent(const SDL_TouchFingerEvent &sdl_sub_evt);

  struct TouchPoint {
    SDL_TouchID touch_id;
    SDL_FingerID finger_id;
    float x;
    float y;
    float start_x;
    float start_y;
  };

  enum class TouchGestureMode {
    None,
    TapPending,
    MultiFingerTapPending,
    OneFingerDrag,
    OneFingerOrbit,
    TwoFingerPan,
    Suppressed,
  };

  struct CapturedTouch {
    SDL_TouchID touch_id;
    SDL_FingerID finger_id;
    SDL_WindowID window_id;
  };
  std::vector<CapturedTouch> captured_touches_;
  std::vector<TouchPoint> active_touches_;
  TouchGestureMode touch_gesture_mode_ = TouchGestureMode::None;
  SDL_WindowID primary_touch_window_id_ = 0;
  uint64_t multi_finger_tap_start_time_ms_ = 0;
  uint8_t multi_finger_tap_count_ = 0;
  float touch_start_x_ = 0.0f;
  float touch_start_y_ = 0.0f;
  float touch_previous_x_ = 0.0f;
  float touch_previous_y_ = 0.0f;
  float touch_previous_distance_ = 0.0f;
  int32_t touch_cursor_x_ = 0;
  int32_t touch_cursor_y_ = 0;
  /** Use the last touch position until mouse or pen input updates the cursor. */
  bool use_touch_cursor_position_ = false;
#endif

  /** The vector of windows that need to be updated. */
  std::vector<GHOST_WindowSDL *> dirty_windows_;
};
