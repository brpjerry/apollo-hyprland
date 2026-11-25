/**
 * @file src/platform/linux/input/unicode_keymap.h
 * @brief Temporary XKB keymaps that type arbitrary Unicode text through a virtual keyboard (the wtype approach).
 * @details A Wayland virtual keyboard sends key codes together with its own keymap. To type text that no layout
 *          has, we upload a keymap whose keys produce exactly the keysyms of the text, press those keys, then
 *          restore a normal keymap. These helpers are the compositor-independent part: decoding, keysym choice and
 *          keymap text. virtual_keyboard.h sends the result.
 */
#pragma once

// standard includes
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace platf::keyboard::unicode_keymap {
  /**
   * @brief Name of the keycodes section in our keymaps, so a keymap we uploaded can be recognised when the
   *        compositor sends it back to us.
   */
  inline constexpr std::string_view section_name = "apollo-unicode";

  /**
   * @brief Most keys in one keymap. XKB keycodes 9..255 (evdev 1..247); X11 clients (Xwayland) cannot use
   *        keycodes above 255.
   */
  inline constexpr std::size_t max_keys = 247;

  /**
   * @brief Decodes UTF-8, dropping invalid sequences, surrogates and code points above U+10FFFF.
   */
  std::u32string decode_utf8(std::string_view utf8);

  /**
   * @brief The keysym that types @p cp, or 0 (XKB_KEY_NoSymbol) for a code point that cannot be typed.
   * @details Newline and carriage return become Return, tab Tab, backspace BackSpace, DEL Delete and escape Escape;
   *          other control characters are dropped. Everything else uses the keysym xkbcommon assigns
   *          (a legacy keysym such as eacute where one exists, otherwise the 0x01000000 + code point form).
   */
  std::uint32_t keysym_for(char32_t cp);

  /**
   * @brief Text for one keymap upload and the keys to press with it.
   */
  struct batch_t {
    std::vector<std::uint32_t> keysyms;  ///< Distinct keysyms; keysyms[i] is on evdev key code i + 1.
    std::vector<std::uint32_t> keys;  ///< Evdev key codes to press and release, in typing order.
  };

  /**
   * @brief Splits @p text into as few keymap uploads as possible.
   * @details Each batch holds at most @p limit distinct keysyms. Characters that cannot be typed are skipped.
   * @return Batches in typing order; empty when there is nothing to type.
   */
  std::vector<batch_t> plan(std::u32string_view text, std::size_t limit = max_keys);

  /**
   * @brief XKB keymap text (xkb_v1 format) that maps evdev key code i + 1 to keysyms[i], one level, no modifiers.
   */
  std::string keymap_text(const std::vector<std::uint32_t> &keysyms);

  /**
   * @brief True if @p keymap is one of ours (see section_name), as the compositor may echo it back.
   */
  bool is_ours(std::string_view keymap);
}  // namespace platf::keyboard::unicode_keymap
