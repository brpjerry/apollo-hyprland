/**
 * @file src/platform/linux/input/virtualhid.cpp
 * @brief Definitions for libvirtualhid Unix input handling.
 */

// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <format>
#include <map>
#include <memory>
#include <utility>

// platform includes
#ifdef SUNSHINE_BUILD_X11
  #include <X11/Xlib.h>
#endif

// local includes
#include "src/platform/virtualhid_input.h"

#ifdef __linux__
  #include "abs_input.h"
  #include "touch_pen.h"

  #ifdef SUNSHINE_BUILD_WAYLAND
    #include "keyboard_layout.h"
    #include "unicode_keymap.h"
    #include "virtual_keyboard.h"
  #endif
#endif

using namespace std::literals;

namespace platf {
  platform_caps::caps_t get_capabilities() {
    platform_caps::caps_t caps = 0;
    const auto runtime = virtualhid::create_runtime();
    if (!runtime) {
      return caps;
    }

    if (const auto &capabilities = runtime->capabilities(); config::input.native_pen_touch && (capabilities.supports_touchscreen || capabilities.supports_pen_tablet)) {
      caps |= platform_caps::pen_touch;
    }
    if (virtualhid::configured_gamepad_supports_controller_extensions()) {
      caps |= platform_caps::controller_touch;
    }

    return caps;
  }

  std::optional<util::point_t> get_mouse_loc(input_t & /*input*/) {
#ifdef SUNSHINE_BUILD_X11
    auto *display = XOpenDisplay(nullptr);
    if (!display) {
      return std::nullopt;
    }

    const auto root = DefaultRootWindow(display);
    Window root_return {};
    Window child_return {};
    int root_x = 0;
    int root_y = 0;
    int window_x = 0;
    int window_y = 0;
    unsigned int mask = 0;
    const auto queried = XQueryPointer(display, root, &root_return, &child_return, &root_x, &root_y, &window_x, &window_y, &mask);
    XCloseDisplay(display);

    if (!queried) {
      return std::nullopt;
    }

    return util::point_t {
      static_cast<double>(root_x),
      static_cast<double>(root_y)
    };
#else
    return std::nullopt;
#endif
  }

  std::vector<supported_gamepad_t> &supported_gamepads(input_t *input) {
    static std::vector<supported_gamepad_t> gamepads;
    if (!input || !input->get()) {
      gamepads = virtualhid::static_supported_gamepads();
      return gamepads;
    }

    gamepads = virtualhid::supported_gamepads(virtualhid::get_input_context(*input).runtime.get());
    return gamepads;
  }

#ifdef __linux__
  // The platform input entry points for Linux (the other platforms' are in src/platform/virtualhid_input.cpp). On top
  // of libvirtualhid they keep relative motion 1:1, follow the live Hyprland layout for the absolute mouse and bind
  // each client's touch screen and pen tablet to the streamed output (abs_input.h), adjust touch and pen events
  // (touch_pen.h), translate non-normalized keys through the host layout (keyboard_layout.h) and type text through the
  // Wayland virtual keyboard.

  /**
   * @brief Global libvirtualhid devices shared by clients, and what Linux adds to them.
   */
  struct input_raw_t {
    virtualhid::input_context_t virtualhid;  ///< libvirtualhid input context.
    std::shared_ptr<abs_input::compositor_t> compositor = abs_input::hyprland_compositor();  ///< Hyprland, if built.
    abs_input::flat_accel_t mouse_accel {compositor};  ///< Keeps relative motion 1:1.
    abs_input::layout_mapper_t mouse_mapper {compositor};  ///< Places absolute moves in the live layout.
    std::map<std::uint16_t, std::uint16_t> pressed_keys;  ///< Pressed key code -> code sent, so releases match.
  #ifdef SUNSHINE_BUILD_WAYLAND
    std::unique_ptr<keyboard::virtual_keyboard_t> text_keyboard;  ///< Types Unicode text; connected on first use.
    std::chrono::steady_clock::time_point text_keyboard_retry {};  ///< When to try connecting text_keyboard again.
    std::unique_ptr<keyboard_layout::host_layout_t> keyboard_layout;  ///< Host layout of the virtual keyboard.
    std::string keyboard_layout_device;  ///< Device keyboard_layout was made for.
  #endif
  };

  namespace {
    /**
     * @brief Number for the next client's device names.
     */
    int next_client_id() {
      static std::atomic<int> next_id {1};
      return next_id++;
    }

    /**
     * @brief Name a libvirtualhid device was created with.
     */
    template<class Device>
    std::string device_name(const std::unique_ptr<Device> &device) {
      return device ? device->profile().name : std::string {};
    }

    /**
     * @brief Per-client libvirtualhid devices, kept bound to the streamed output.
     */
    struct client_input_raw_t: client_input_t {
      /**
       * @brief Create per-client libvirtualhid devices with names unique to this client.
       *
       * @param input Platform input backend that receives the event.
       */
      explicit client_input_raw_t(input_t &input):
          virtualhid {input->virtualhid, touch_pen::client_device_suffix(next_client_id())},
          touch_binder {device_name(virtualhid.touch), input->compositor},
          pen_binder {device_name(virtualhid.pen), input->compositor} {}

      client_input_raw_t(const client_input_raw_t &) = delete;
      client_input_raw_t &operator=(const client_input_raw_t &) = delete;

      /**
       * @brief Lift whatever is still down before the devices go away, and drop their output bindings.
       */
      ~client_input_raw_t() override {
        // Devices are closed already when the global input went first (at exit).
        if (virtualhid.touch && virtualhid.touch->is_open() && !virtualhid.active_touches.empty()) {
          virtualhid::touch_update(virtualhid, {}, {.eventType = LI_TOUCH_EVENT_CANCEL_ALL});
        }
        if (virtualhid.pen && virtualhid.pen->is_open() && (pen_state.in_proximity || !virtualhid.pressed_pen_buttons.empty())) {
          virtualhid::pen_update(virtualhid, {}, touch_pen::pen_event({.eventType = LI_TOUCH_EVENT_CANCEL_ALL}, pen_state));
        }
        touch_binder.unbind();
        pen_binder.unbind();
      }

      virtualhid::client_context_t virtualhid;  ///< libvirtualhid client context.
      touch_pen::pen_state_t pen_state;  ///< Pen tool and position between events.
      abs_input::output_binder_t touch_binder;  ///< Binds the touch screen to the streamed output.
      abs_input::output_binder_t pen_binder;  ///< Binds the pen tablet to the streamed output.
    };

    /**
     * @brief Log a failed libvirtualhid operation.
     */
    void log_failure(std::string_view operation, const lvh::OperationStatus &status) {
      if (!status.ok()) {
        BOOST_LOG(warning) << operation << ": "sv << status.message();
      }
    }

    /**
     * @brief The port and position to hand libvirtualhid for a touch or pen event.
     * @details Once Hyprland has bound the device to the output, the output name is dropped (libvirtualhid would try
     *          to assign the device to it through KWin's D-Bus interface on every event) and the position is placed
     *          in libinput units, so it lands on the pixel the client pointed at.
     * @param binder Binder of the device.
     * @param touch_port Port of the event.
     * @param event Touch or pen event whose x and y are adjusted.
     * @return The port.
     */
    template<class Event>
    touch_port_t device_port(abs_input::output_binder_t &binder, const touch_port_t &touch_port, Event &event) {
      auto port = touch_port;
      if (binder.update(touch_port)) {
        port.output_name.clear();
        event.x = abs_input::libinput_fraction(event.x, abs_input::touch_axis_max_x);
        event.y = abs_input::libinput_fraction(event.y, abs_input::touch_axis_max_y);
      }
      return port;
    }

    /**
     * @brief Whether Hyprland answers, so the absolute mouse uses the live layout (and relative motion must not
     *        retarget the mouse to Sunshine's viewport, which would recreate it on every switch).
     */
    bool follows_hyprland(input_raw_t &raw) {
      return raw.compositor && raw.compositor->outputs(false);
    }

  #ifdef SUNSHINE_BUILD_WAYLAND
    /**
     * @brief Types text through the Wayland virtual keyboard.
     * @return false when there is no usable virtual keyboard, so the caller falls back to libvirtualhid's IBus
     *         sequence.
     */
    bool virtual_keyboard_unicode(input_raw_t &raw, const char *utf8, int size) {
    #ifdef SUNSHINE_TESTS
      if (!abs_input::virtual_keyboard_for_testing()) {
        return false;
      }
    #endif
      auto now = std::chrono::steady_clock::now();
      if (raw.text_keyboard && !raw.text_keyboard->alive()) {
        raw.text_keyboard.reset();
      }
      if (!raw.text_keyboard) {
        if (now < raw.text_keyboard_retry) {
          return false;
        }
        raw.text_keyboard = keyboard::virtual_keyboard_t::connect(nullptr);
        if (!raw.text_keyboard) {
          // The compositor may not be up yet, or may lack the protocol; don't reconnect for every character.
          raw.text_keyboard_retry = now + 10s;
          BOOST_LOG(info) << "Unicode text falls back to IBus (Ctrl+Shift+U)"sv;
          return false;
        }
      }
      auto text = keyboard::unicode_keymap::decode_utf8({utf8, static_cast<std::size_t>(std::max(size, 0))});
      BOOST_LOG(debug) << "Unicode, typing "sv << text.size() << " characters through the virtual keyboard"sv;
      if (!raw.text_keyboard->type(text)) {
        // Part of the text may be typed already, so don't type it again through IBus; reconnect next time.
        BOOST_LOG(warning) << "Unicode, the virtual keyboard failed while typing"sv;
        raw.text_keyboard.reset();
      }
      return true;
    }

    /**
     * @brief Translate a non-normalized key code through the host layout of the virtual keyboard.
     */
    std::uint16_t host_key(input_raw_t &raw, std::uint16_t modcode) {
      const auto name = device_name(raw.virtualhid.keyboard);
      if (name.empty()) {
        return modcode;
      }
      if (!raw.keyboard_layout || raw.keyboard_layout_device != name) {
        raw.keyboard_layout = std::make_unique<keyboard_layout::host_layout_t>(name);
        raw.keyboard_layout_device = name;
      }
      const auto code = raw.keyboard_layout->translate(modcode);
      if (code != modcode) {
        BOOST_LOG(verbose) << std::format("Non-normalized key {:#04x} is host key {:#04x}", modcode, code);
      }
      return code;
    }
  #endif
  }  // namespace

  input_t input() {
    return input_t {new input_raw_t {}};
  }

  std::unique_ptr<client_input_t> allocate_client_input_context(input_t &input) {
    return std::make_unique<client_input_raw_t>(input);
  }

  void freeInput(input_raw_t *input) {
    std::default_delete<input_raw_t> {}(input);
  }

  virtualhid::input_context_t &virtualhid::get_input_context(input_t &input) {
    return input->virtualhid;
  }

  virtualhid::client_context_t &virtualhid::get_client_context(client_input_t *input) {
    return static_cast<client_input_raw_t *>(input)->virtualhid;
  }

  void move_mouse(input_t &input, int deltaX, int deltaY) {
    auto &raw = *input;
    raw.mouse_accel.ensure(device_name(raw.virtualhid.mouse));
    virtualhid::move_mouse(raw.virtualhid, deltaX, deltaY);
  }

  void move_mouse(input_t &input, const touch_port_t &touch_port, int deltaX, int deltaY) {
    auto &raw = *input;
    if (follows_hyprland(raw)) {
      move_mouse(input, deltaX, deltaY);
      return;
    }
    raw.mouse_accel.ensure(device_name(raw.virtualhid.mouse));
    virtualhid::move_mouse(raw.virtualhid, touch_port, deltaX, deltaY);
  }

  void abs_mouse(input_t &input, const touch_port_t &touch_port, float x, float y) {
    auto &raw = *input;
    auto &context = raw.virtualhid;
    if (!context.mouse) {
      return;
    }

    const auto target = raw.mouse_mapper.map(touch_port, x, y);
    if (!target) {
      virtualhid::abs_mouse(context, touch_port, x, y);
      return;
    }

    // Hyprland maps the absolute device onto the whole layout box: keep libvirtualhid's default pointer area (the
    // whole axis) and hand it the libinput units of the target.
    if (context.mouse_desktop.width != 0 || context.mouse_desktop.height != 0 || context.mouse_viewport.width != 0 || context.mouse_viewport.height != 0) {
      context.refresh_mouse();
      if (!context.mouse) {
        return;
      }
    }
    const auto units_x = abs_input::libinput_units(target->at.x, target->min.x, target->max.x, abs_input::mouse_axis_max);
    const auto units_y = abs_input::libinput_units(target->at.y, target->min.y, target->max.y, abs_input::mouse_axis_max);
    log_failure("submit libvirtualhid absolute mouse movement"sv, context.mouse->move_absolute(units_x, units_y, abs_input::mouse_axis_max, abs_input::mouse_axis_max));
  }

  void button_mouse(input_t &input, int button, bool release) {
    virtualhid::button_mouse(input->virtualhid, button, release);
  }

  void scroll(input_t &input, int high_res_distance) {
    virtualhid::scroll(input->virtualhid, high_res_distance);
  }

  void hscroll(input_t &input, int high_res_distance) {
    virtualhid::hscroll(input->virtualhid, high_res_distance);
  }

  void keyboard_update(input_t &input, uint16_t modcode, bool release, uint8_t flags, bool extended) {
    auto &raw = *input;
    auto code = modcode;
    if (release) {
      // Release what the press pressed, even if the layout changed in between.
      if (auto it = raw.pressed_keys.find(modcode); it != raw.pressed_keys.end()) {
        code = it->second;
        raw.pressed_keys.erase(it);
      }
    } else {
  #ifdef SUNSHINE_BUILD_WAYLAND
      if (keyboard_layout::key_mapping(flags, config::input.always_send_scancodes) == keyboard_layout::key_mapping_e::host_layout) {
        code = host_key(raw, modcode);
      }
  #endif
      raw.pressed_keys[modcode] = code;
    }
    virtualhid::keyboard_update(raw.virtualhid, code, release, flags, extended);
  }

  void unicode(input_t &input, const char *utf8, int size) {
  #ifdef SUNSHINE_BUILD_WAYLAND
    if (virtual_keyboard_unicode(*input, utf8, size)) {
      return;
    }
  #endif
    virtualhid::unicode(input->virtualhid, utf8, size);
  }

  void touch_update(client_input_t *input, const touch_port_t &touch_port, const touch_input_t &touch) {
    auto &raw = *static_cast<client_input_raw_t *>(input);
    auto &context = raw.virtualhid;
    if (!context.touch) {
      return;
    }
    auto event = touch_pen::touch_event(touch, context.active_touches.contains(static_cast<std::int32_t>(touch.pointerId)));
    if (!event) {
      return;
    }
    const auto port = device_port(raw.touch_binder, touch_port, *event);
    virtualhid::touch_update(context, port, *event);
  }

  void pen_update(client_input_t *input, const touch_port_t &touch_port, const pen_input_t &pen) {
    auto &raw = *static_cast<client_input_raw_t *>(input);
    auto &context = raw.virtualhid;
    if (!context.pen) {
      return;
    }

    if (touch_pen::pen_buttons_only(pen)) {
      const std::array buttons {
        std::pair {lvh::PenButton::primary, (pen.penButtons & LI_PEN_BUTTON_PRIMARY) != 0},
        std::pair {lvh::PenButton::secondary, (pen.penButtons & LI_PEN_BUTTON_SECONDARY) != 0},
        std::pair {lvh::PenButton::tertiary, (pen.penButtons & LI_PEN_BUTTON_TERTIARY) != 0},
      };
      for (const auto &[button, pressed] : buttons) {
        if (pressed == context.pressed_pen_buttons.contains(button)) {
          continue;
        }
        log_failure("submit libvirtualhid pen button"sv, context.pen->button(button, pressed));
        if (pressed) {
          context.pressed_pen_buttons.insert(button);
        } else {
          context.pressed_pen_buttons.erase(button);
        }
      }
      return;
    }

    auto event = touch_pen::pen_event(pen, raw.pen_state);
    const auto port = device_port(raw.pen_binder, touch_port, event);
    virtualhid::pen_update(context, port, event);
  }
#endif
}  // namespace platf
