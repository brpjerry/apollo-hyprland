/**
 * @file src/platform/linux/input/input_options.h
 * @brief How the Linux input backend applies the input options (HYP-24, HYP-25), as pure functions for tests.
 */
#pragma once

// standard includes
#include <string_view>

// local includes
#include "src/platform/common.h"

namespace platf::input_options {
  /**
   * @brief Capabilities to advertise to clients.
   * @param uinput_available Whether this process can create uinput devices (the per-client touch screen and pen).
   * @param native_pen_touch The native_pen_touch option; without it clients send pen and touch as mouse input.
   * @param gamepad The gamepad option (already resolved through gamepad_type()).
   */
  inline platform_caps::caps_t capabilities(bool uinput_available, bool native_pen_touch, std::string_view gamepad) {
    platform_caps::caps_t caps = 0;
    if (uinput_available && native_pen_touch) {
      caps |= platform_caps::pen_touch;
    }
    // Controller touchpads only exist on the emulated DualSense.
    if (gamepad == "ds5" || gamepad == "auto") {
      caps |= platform_caps::controller_touch;
    }
    return caps;
  }
}  // namespace platf::input_options
