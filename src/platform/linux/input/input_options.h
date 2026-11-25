/**
 * @file src/platform/linux/input/input_options.h
 * @brief How the Linux input backend applies the input options (HYP-24, HYP-25), as pure functions for tests.
 */
#pragma once

// standard includes
#include <cstdint>
#include <string_view>

// local includes
#include "src/platform/common.h"

namespace platf::input_options {
  /**
   * @brief The Linux gamepad type for a configured one: the Windows names x360 and ds4 (in configs or per-app
   *        settings carried over from Windows) mean xone and ds5.
   */
  inline std::string_view gamepad_type(std::string_view configured) {
    if (configured == "x360") {
      return "xone";
    }
    if (configured == "ds4") {
      return "ds5";
    }
    return configured;
  }

  /**
   * @brief Buttons for the emulated DualSense: with a manually chosen DualSense (ds5 or ds4) and
   *        ds4_back_as_touchpad_click, Back/Select also clicks the touchpad, as with DS4 emulation on Windows.
   */
  inline std::uint32_t ds5_buttons(std::uint32_t buttons, std::string_view configured, bool back_as_touchpad_click) {
    if (back_as_touchpad_click && gamepad_type(configured) == "ds5" && (buttons & BACK)) {
      buttons |= TOUCHPAD_BUTTON;
    }
    return buttons;
  }

  /**
   * @brief Whether a rumble request from a game goes to the client: forward_rumble is on and it differs from the
   *        last one sent.
   */
  inline bool forward_rumble(bool enabled, const gamepad_feedback_msg_t &last, std::uint16_t low, std::uint16_t high) {
    if (!enabled) {
      return false;
    }
    return !(last.type == gamepad_feedback_e::rumble && last.data.rumble.lowfreq == low && last.data.rumble.highfreq == high);
  }

  /**
   * @brief How a keyboard event's virtual-key code becomes a host key.
   */
  enum class key_mapping_e {
    us_position,  ///< The code is a US-layout key position (normalized codes, the default)
    host_layout,  ///< The code names a character; type it with the host key that has it (keyboard_layout.h)
  };

  /**
   * @brief Non-normalized codes (SS_KBE_FLAG_NON_NORMALIZED) go through the host layout when always_send_scancodes
   *        is on, like MapVirtualKey on Windows; with it off, they keep their US position.
   */
  inline key_mapping_e key_mapping(std::uint8_t flags, bool always_send_scancodes) {
    return (flags & SS_KBE_FLAG_NON_NORMALIZED) && always_send_scancodes ? key_mapping_e::host_layout : key_mapping_e::us_position;
  }

  /**
   * @brief Capabilities to advertise to clients.
   * @param uinput_available Whether this process can create uinput devices (the per-client touch screen and pen).
   * @param native_pen_touch The native_pen_touch option; without it clients send pen and touch as mouse input.
   * @param gamepad The gamepad option.
   */
  inline platform_caps::caps_t capabilities(bool uinput_available, bool native_pen_touch, std::string_view gamepad) {
    platform_caps::caps_t caps = 0;
    if (uinput_available && native_pen_touch) {
      caps |= platform_caps::pen_touch;
    }
    // Controller touchpads only exist on the emulated DualSense.
    gamepad = gamepad_type(gamepad);
    if (gamepad == "ds5" || gamepad == "auto") {
      caps |= platform_caps::controller_touch;
    }
    return caps;
  }
}  // namespace platf::input_options
