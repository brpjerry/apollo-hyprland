/**
 * @file src/platform/linux/input/virtual_keyboard.h
 * @brief Unicode text input through zwp_virtual_keyboard_v1 (HYP-14).
 * @details Windows types text with KEYEVENTF_UNICODE, which works in every application. Linux used IBus's
 *          Ctrl+Shift+U sequence, which only works in IBus/GTK clients and can trigger compositor key bindings.
 *          Here a Wayland virtual keyboard uploads a temporary keymap that holds the keysyms of the text, types
 *          them with no modifiers, and then restores the seat's normal keymap (the approach wtype uses). The
 *          keyboard goes to whichever surface has keyboard focus, like the uinput keyboard.
 */
#pragma once

// standard includes
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace platf::keyboard {
  class virtual_keyboard_t {
  public:
    /**
     * @brief Connects to a compositor and creates a virtual keyboard on its first seat.
     * @param display_name Wayland display to connect to; nullptr uses WAYLAND_DISPLAY.
     * @return nullptr when there is no compositor, no seat, or no zwp_virtual_keyboard_manager_v1.
     */
    static std::unique_ptr<virtual_keyboard_t> connect(const char *display_name);

    ~virtual_keyboard_t();
    virtual_keyboard_t(const virtual_keyboard_t &) = delete;
    virtual_keyboard_t &operator=(const virtual_keyboard_t &) = delete;

    /**
     * @brief Types @p text: one keymap upload per batch of up to 247 distinct characters, then the normal
     *        keymap again. Characters that cannot be typed (most control characters) are skipped.
     * @return false if the connection to the compositor is broken (the text may be partly typed).
     */
    bool type(std::u32string_view text);

    /**
     * @brief False once the connection to the compositor failed; reconnect with connect().
     */
    bool alive() const;

    /**
     * @brief Keymaps uploaded so far, including restores (for tests).
     */
    std::size_t keymap_uploads() const;

    /**
     * @brief The keymap type() restores afterwards: the seat's keymap as last sent to us by the compositor, or
     *        xkbcommon's default keymap (XKB_DEFAULT_* environment, else "us") until there was one.
     */
    std::string restore_keymap() const;

    struct impl_t;

  private:
    explicit virtual_keyboard_t(std::unique_ptr<impl_t> impl);
    std::unique_ptr<impl_t> impl;
  };
}  // namespace platf::keyboard
