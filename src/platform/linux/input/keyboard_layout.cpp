/**
 * @file src/platform/linux/input/keyboard_layout.cpp
 * @brief Definitions for host keyboard layout lookups.
 */
// local includes
#include "keyboard_layout.h"

#include "abs_input.h"
#include "src/logging.h"
#include "src/platform/linux/hyprland/ipc.h"

// standard includes
#include <array>
#include <utility>

// lib includes
#include <linux/input-event-codes.h>
#include <nlohmann/json.hpp>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

using namespace std::literals;

namespace platf::keyboard_layout {
  std::optional<std::uint32_t> vk_keysym(std::uint16_t vk) {
    if (vk >= 'A' && vk <= 'Z') {
      return XKB_KEY_a + (vk - 'A');
    }
    if (vk >= '0' && vk <= '9') {
      return XKB_KEY_0 + (vk - '0');
    }
    return std::nullopt;
  }

  std::optional<int> evdev_code(xkb_keymap *keymap, std::uint32_t layout, std::uint32_t keysym) {
    if (!keymap) {
      return std::nullopt;
    }
    if (layout >= xkb_keymap_num_layouts(keymap)) {
      layout = 0;
    }
    auto upper = xkb_keysym_to_upper(keysym);
    auto min = xkb_keymap_min_keycode(keymap);
    auto max = xkb_keymap_max_keycode(keymap);
    for (xkb_level_index_t level = 0; level < 4; ++level) {
      for (auto kc = min; kc <= max; ++kc) {
        if (level >= xkb_keymap_num_levels_for_key(keymap, kc, layout)) {
          continue;
        }
        const xkb_keysym_t *syms = nullptr;
        int n = xkb_keymap_key_get_syms_by_level(keymap, kc, layout, level, &syms);
        for (int i = 0; i < n; ++i) {
          // A letter counts in either case (caps-lock layouts put the upper case first on some keys).
          if (syms[i] == keysym || syms[i] == upper) {
            return int(kc) - 8;
          }
        }
      }
    }
    return std::nullopt;
  }

  std::optional<names_t> find_keyboard(const nlohmann::json &devices, std::string_view evdev_name) {
    if (!devices.is_object() || !devices.contains("keyboards") || !devices["keyboards"].is_array()) {
      return std::nullopt;
    }
    auto base = abs_input::compositor_device_name(evdev_name);
    for (const auto &kb : devices["keyboards"]) {
      if (!kb.is_object() || !abs_input::is_instance_of(kb.value("name", ""), base)) {
        continue;
      }
      names_t names;
      names.rules = kb.value("rules", "");
      names.model = kb.value("model", "");
      names.layout = kb.value("layout", "");
      names.variant = kb.value("variant", "");
      names.options = kb.value("options", "");
      names.active_layout = kb.value("active_layout_index", 0u);
      return names;
    }
    return std::nullopt;
  }

  std::unique_ptr<keymap_t> keymap_t::compile(const names_t &names) {
    std::unique_ptr<keymap_t> km {new keymap_t()};
    km->ctx_ = xkb_context_new(XKB_CONTEXT_NO_ENVIRONMENT_NAMES);
    if (!km->ctx_) {
      return nullptr;
    }
    auto opt = [](const std::string &s) {
      return s.empty() ? nullptr : s.c_str();
    };
    xkb_rule_names rmlvo {
      .rules = opt(names.rules),
      .model = opt(names.model),
      .layout = opt(names.layout),
      .variant = opt(names.variant),
      .options = opt(names.options),
    };
    km->keymap_ = xkb_keymap_new_from_names(km->ctx_, &rmlvo, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!km->keymap_) {
      return nullptr;
    }
    return km;
  }

  keymap_t::~keymap_t() {
    if (keymap_) {
      xkb_keymap_unref(keymap_);
    }
    if (ctx_) {
      xkb_context_unref(ctx_);
    }
  }

  std::optional<std::uint16_t> vk_for_evdev(int code) {
    // The keys a letter or digit can be on in some layout, with the virtual-key code libvirtualhid's Linux keyboard
    // turns into each of them (key_code_to_linux() in its uhid_backend.cpp).
    static constexpr std::array<std::pair<int, std::uint16_t>, 60> keys {{
      {KEY_A, 0x41},
      {KEY_B, 0x42},
      {KEY_C, 0x43},
      {KEY_D, 0x44},
      {KEY_E, 0x45},
      {KEY_F, 0x46},
      {KEY_G, 0x47},
      {KEY_H, 0x48},
      {KEY_I, 0x49},
      {KEY_J, 0x4A},
      {KEY_K, 0x4B},
      {KEY_L, 0x4C},
      {KEY_M, 0x4D},
      {KEY_N, 0x4E},
      {KEY_O, 0x4F},
      {KEY_P, 0x50},
      {KEY_Q, 0x51},
      {KEY_R, 0x52},
      {KEY_S, 0x53},
      {KEY_T, 0x54},
      {KEY_U, 0x55},
      {KEY_V, 0x56},
      {KEY_W, 0x57},
      {KEY_X, 0x58},
      {KEY_Y, 0x59},
      {KEY_Z, 0x5A},
      {KEY_0, 0x30},
      {KEY_1, 0x31},
      {KEY_2, 0x32},
      {KEY_3, 0x33},
      {KEY_4, 0x34},
      {KEY_5, 0x35},
      {KEY_6, 0x36},
      {KEY_7, 0x37},
      {KEY_8, 0x38},
      {KEY_9, 0x39},
      {KEY_KP0, 0x60},
      {KEY_KP1, 0x61},
      {KEY_KP2, 0x62},
      {KEY_KP3, 0x63},
      {KEY_KP4, 0x64},
      {KEY_KP5, 0x65},
      {KEY_KP6, 0x66},
      {KEY_KP7, 0x67},
      {KEY_KP8, 0x68},
      {KEY_KP9, 0x69},
      {KEY_SPACE, 0x20},
      {KEY_SEMICOLON, 0xBA},
      {KEY_EQUAL, 0xBB},
      {KEY_COMMA, 0xBC},
      {KEY_MINUS, 0xBD},
      {KEY_DOT, 0xBE},
      {KEY_SLASH, 0xBF},
      {KEY_GRAVE, 0xC0},
      {KEY_LEFTBRACE, 0xDB},
      {KEY_BACKSLASH, 0xDC},
      {KEY_RIGHTBRACE, 0xDD},
      {KEY_APOSTROPHE, 0xDE},
      {KEY_102ND, 0xE2},
      {KEY_KPDOT, 0x6E},
    }};
    for (const auto &[evdev, vk] : keys) {
      if (evdev == code) {
        return vk;
      }
    }
    return std::nullopt;
  }

  host_layout_t::host_layout_t(std::string evdev_name):
      evdev_name_ {std::move(evdev_name)} {
  }

  host_layout_t::~host_layout_t() = default;

  void host_layout_t::refresh() {
    auto now = std::chrono::steady_clock::now();
    if (now < next_refresh_) {
      return;
    }
    next_refresh_ = now + 1s;

    std::optional<names_t> names;
    if (auto ipc = hyprland::ipc_t::connect()) {
      if (auto devices = ipc->request_json("j/devices")) {
        names = find_keyboard(*devices, evdev_name_);
      }
    }
    if (!names) {
      names_.reset();
      keymap_.reset();
      return;
    }
    if (!names_ || !names_->same_keymap(*names) || !keymap_) {
      keymap_ = keymap_t::compile(*names);
      BOOST_LOG(info) << "Non-normalized keys follow the host layout ["sv << names->layout << "] of ["sv << evdev_name_ << ']'
                      << (keymap_ ? ""sv : " (the keymap does not compile)"sv);
    }
    names_ = names;
  }

  std::uint16_t host_layout_t::translate(std::uint16_t vk) {
    auto keysym = vk_keysym(vk);
    if (!keysym) {
      return vk;
    }
    refresh();
    if (!keymap_ || !names_) {
      return vk;
    }
    auto code = evdev_code(keymap_->get(), names_->active_layout, *keysym);
    if (!code) {
      return vk;
    }
    return vk_for_evdev(*code).value_or(vk);
  }
}  // namespace platf::keyboard_layout
