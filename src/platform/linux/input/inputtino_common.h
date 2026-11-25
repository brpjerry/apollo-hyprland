/**
 * @file src/platform/linux/input/inputtino_common.h
 * @brief Declarations for inputtino common input handling.
 */
#pragma once

// lib includes
#include <boost/locale.hpp>
#include <inputtino/input.hpp>
#include <libevdev/libevdev.h>

// standard includes
#include <atomic>
#include <chrono>
#include <format>

// local includes
#include "inputtino_abs.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/utility.h"

#ifdef SUNSHINE_BUILD_WAYLAND
  #include "virtual_keyboard.h"
#endif

using namespace std::literals;

namespace platf {

  using joypads_t = std::variant<inputtino::XboxOneJoypad, inputtino::SwitchJoypad, inputtino::PS5Joypad>;

  struct joypad_state {
    std::unique_ptr<joypads_t> joypad;
    gamepad_feedback_msg_t last_rumble;
    gamepad_feedback_msg_t last_rgb_led;
  };

  struct input_raw_t {
    input_raw_t():
        mouse(inputtino::Mouse::create({
          .name = "Mouse passthrough",
          .vendor_id = 0xBEEF,
          .product_id = 0xDEAD,
          .version = 0x111,
        })),
        keyboard(inputtino::Keyboard::create({
          .name = "Keyboard passthrough",
          .vendor_id = 0xBEEF,
          .product_id = 0xDEAD,
          .version = 0x111,
        })),
        gamepads(MAX_GAMEPADS),
        mouse_accel("Mouse passthrough", abs_input::hyprland_compositor()),
        mouse_mapper(abs_input::hyprland_compositor()) {
      if (!mouse) {
        BOOST_LOG(warning) << "Unable to create virtual mouse: " << mouse.getErrorMessage();
      }
      if (!keyboard) {
        BOOST_LOG(warning) << "Unable to create virtual keyboard: " << keyboard.getErrorMessage();
      }
    }

    ~input_raw_t() = default;

    // All devices are wrapped in Result because it might be that we aren't able to create them (ex: udev permission denied)
    inputtino::Result<inputtino::Mouse> mouse;
    inputtino::Result<inputtino::Keyboard> keyboard;

    /**
     * A list of gamepads that are currently connected.
     * The pointer is shared because that state will be shared with background threads that deal with rumble and LED
     */
    std::vector<std::shared_ptr<joypad_state>> gamepads;

    /// Keeps relative motion of the virtual mouse 1:1 regardless of the user's pointer acceleration.
    abs_input::flat_accel_t mouse_accel;

    /// Places absolute mouse moves in the live desktop layout (see inputtino_abs.h).
    abs_input::layout_mapper_t mouse_mapper;

#ifdef SUNSHINE_BUILD_WAYLAND
    /// Types Unicode text (see virtual_keyboard.h); connected on the first text event.
    std::unique_ptr<keyboard::virtual_keyboard_t> text_keyboard;
    /// When to try connecting text_keyboard again after a failure.
    std::chrono::steady_clock::time_point text_keyboard_retry {};
#endif
  };

  /**
   * @brief Name for a per-client device, unique within this process, e.g. "Touch passthrough (client 3)".
   * @details The compositor binds devices to an output by name, so clients must not share names.
   */
  inline std::string client_device_name(std::string_view base, int client_id) {
    return std::format("{} (client {})", base, client_id);
  }

  inline int next_client_device_id() {
    static std::atomic<int> next_id {1};
    return next_id++;
  }

  struct client_input_raw_t: public client_input_t {
    client_input_raw_t(input_t &input):
        client_input_raw_t(input, next_client_device_id()) {
    }

    client_input_raw_t(input_t &input, int client_id):
        touch(inputtino::TouchScreen::create({
          .name = client_device_name("Touch passthrough", client_id),
          .vendor_id = 0xBEEF,
          .product_id = 0xDEAD,
          .version = 0x111,
        })),
        pen(inputtino::PenTablet::create({
          .name = client_device_name("Pen passthrough", client_id),
          .vendor_id = 0xBEEF,
          .product_id = 0xDEAD,
          .version = 0x111,
        })),
        touch_mapper(client_device_name("Touch passthrough", client_id), abs_input::hyprland_compositor()),
        pen_mapper(client_device_name("Pen passthrough", client_id), abs_input::hyprland_compositor()) {
      global = (input_raw_t *) input.get();
      if (!touch) {
        BOOST_LOG(warning) << "Unable to create virtual touch screen: " << touch.getErrorMessage();
      }
      if (!pen) {
        BOOST_LOG(warning) << "Unable to create virtual pen tablet: " << pen.getErrorMessage();
      }
    }

    input_raw_t *global;

    // Device state and handles for pen and touch input must be stored in the per-client
    // input context, because each connected client may be sending their own independent
    // pen/touch events. To maintain separation, we expose separate pen and touch devices
    // for each client.
    inputtino::Result<inputtino::TouchScreen> touch;
    inputtino::Result<inputtino::PenTablet> pen;

    // Keep each device bound to the output being streamed (see inputtino_abs.h).
    abs_input::output_mapper_t touch_mapper;
    abs_input::output_mapper_t pen_mapper;
  };

  inline float deg2rad(float degree) {
    return degree * (M_PI / 180.f);
  }
}  // namespace platf
