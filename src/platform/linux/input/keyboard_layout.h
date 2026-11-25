/**
 * @file src/platform/linux/input/keyboard_layout.h
 * @brief Host keyboard layout lookups for key codes that clients did not normalize to US English (HYP-25).
 * @details Moonlight clients normally send Windows virtual-key codes for US-layout key positions. With
 *          SS_KBE_FLAG_NON_NORMALIZED the code names the character in the client's layout instead (VK_A is "the key
 *          that types a"). Windows then asks the host layout which key that is (MapVirtualKey); here the Hyprland
 *          keymap of Apollo's uinput keyboard answers the same question for letters and digits.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

// lib includes
#include <nlohmann/json_fwd.hpp>

struct xkb_context;
struct xkb_keymap;

namespace platf::keyboard_layout {
  /**
   * @brief The keysym a non-normalized virtual-key code stands for: letters (lower case) and digits only, whose
   *        virtual-key codes are layout independent. Other keys keep their US position.
   */
  std::optional<std::uint32_t> vk_keysym(std::uint16_t vk);

  /**
   * @brief The evdev key code that produces @p keysym in layout (group) @p layout of @p keymap, preferring keys
   *        that have it on the first level (unshifted), then the second, and so on.
   */
  std::optional<int> evdev_code(xkb_keymap *keymap, std::uint32_t layout, std::uint32_t keysym);

  /**
   * @brief RMLVO names and the active layout of a keyboard, as Hyprland reports them.
   */
  struct names_t {
    std::string rules;
    std::string model;
    std::string layout;
    std::string variant;
    std::string options;
    std::uint32_t active_layout = 0;

    bool same_keymap(const names_t &other) const {
      return rules == other.rules && model == other.model && layout == other.layout && variant == other.variant && options == other.options;
    }
  };

  /**
   * @brief Finds the keyboard created as @p evdev_name in `hyprctl devices -j` output.
   */
  std::optional<names_t> find_keyboard(const nlohmann::json &devices, std::string_view evdev_name);

  /**
   * @brief Owns an xkbcommon keymap compiled from RMLVO names.
   */
  class keymap_t {
  public:
    static std::unique_ptr<keymap_t> compile(const names_t &names);
    ~keymap_t();
    keymap_t(const keymap_t &) = delete;
    keymap_t &operator=(const keymap_t &) = delete;

    xkb_keymap *get() const {
      return keymap_;
    }

  private:
    keymap_t() = default;
    xkb_context *ctx_ = nullptr;
    xkb_keymap *keymap_ = nullptr;
  };

  /**
   * @brief Translates non-normalized virtual-key codes through the host layout of one uinput keyboard.
   * @details Asks Hyprland for the keyboard's layout at most once a second and recompiles the keymap only when the
   *          names change. Without Hyprland or a matching key, the code keeps its US position.
   */
  class host_layout_t {
  public:
    explicit host_layout_t(std::string evdev_name);
    ~host_layout_t();

    /**
     * @brief The virtual-key code whose US position is the host key that types what @p vk stands for, or @p vk.
     */
    std::uint16_t translate(std::uint16_t vk);

  private:
    void refresh();

    std::string evdev_name_;
    std::optional<names_t> names_;
    std::unique_ptr<keymap_t> keymap_;
    std::chrono::steady_clock::time_point next_refresh_ {};
  };

  /**
   * @brief The virtual-key code that inputtino's keyboard maps to evdev key code @p code, if any.
   */
  std::optional<std::uint16_t> vk_for_evdev(int code);
}  // namespace platf::keyboard_layout
