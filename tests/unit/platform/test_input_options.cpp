/**
 * @file tests/unit/platform/test_input_options.cpp
 * @brief Test how the Linux input backend applies keyboard and gamepad options (HYP-25): input_options.h,
 *        keyboard_layout.* and the gamepad rumble callback. No devices are created.
 */
#ifdef __linux__
  // inputtino before Limelight.h (via tests_common.h), whose macros clash with inputtino's enum names; the sources
  // include them in this order too.
  #include <inputtino/input.hpp>
#endif

#include "../../tests_common.h"

#ifdef __linux__

  #include <src/config.h>
  #include <src/platform/linux/input/input_options.h>
  #include <src/platform/linux/input/inputtino_gamepad.h>

  #ifdef SUNSHINE_BUILD_WAYLAND
    #include <nlohmann/json.hpp>
    #include <src/platform/linux/input/keyboard_layout.h>
  #endif

using namespace platf;
using namespace std::literals;

// Bugs caught: 0, False positives: 0
TEST(InputOptionsTest, WindowsGamepadNamesAreAliases) {
  EXPECT_EQ(input_options::gamepad_type("x360"), "xone");
  EXPECT_EQ(input_options::gamepad_type("ds4"), "ds5");
  for (auto name : {"auto"sv, "xone"sv, "ds5"sv, "switch"sv}) {
    EXPECT_EQ(input_options::gamepad_type(name), name);
  }
  // The aliases also decide the controller touchpad capability.
  EXPECT_EQ(input_options::capabilities(false, false, "ds4"), platform_caps::controller_touch);
  EXPECT_EQ(input_options::capabilities(false, false, "x360"), 0u);

  // Configs accept them (the config parser only takes names from this list).
  auto &names = supported_gamepads(nullptr);
  auto has = [&](std::string_view n) {
    return std::ranges::any_of(names, [&](const supported_gamepad_t &g) {
      return g.name == n;
    });
  };
  EXPECT_TRUE(has("x360"));
  EXPECT_TRUE(has("ds4"));
  EXPECT_EQ(names.front().name, "auto");  // Still the default
}

// Bugs caught: 0, False positives: 0
TEST(InputOptionsTest, BackAsTouchpadClickOnManualDualSense) {
  const std::uint32_t back = BACK | A;
  EXPECT_EQ(input_options::ds5_buttons(back, "ds5", true), back | TOUCHPAD_BUTTON);
  EXPECT_EQ(input_options::ds5_buttons(back, "ds4", true), back | TOUCHPAD_BUTTON);
  EXPECT_EQ(input_options::ds5_buttons(back, "ds5", false), back);  // Option off
  EXPECT_EQ(input_options::ds5_buttons(back, "auto", true), back);  // Only when chosen manually, as on Windows
  EXPECT_EQ(input_options::ds5_buttons(A, "ds5", true), A);  // Back not pressed
}

// Bugs caught: 0, False positives: 0
TEST(InputOptionsTest, RumbleForwarding) {
  gamepad_feedback_msg_t none {};
  auto last = gamepad_feedback_msg_t::make_rumble(0, 100, 200);
  EXPECT_FALSE(input_options::forward_rumble(false, none, 100, 200));  // forward_rumble off
  EXPECT_TRUE(input_options::forward_rumble(true, none, 100, 200));
  EXPECT_FALSE(input_options::forward_rumble(true, last, 100, 200));  // Unchanged
  EXPECT_TRUE(input_options::forward_rumble(true, last, 0, 0));
}

// Bugs caught: 0, False positives: 0
TEST(InputOptionsTest, RumbleHandlerRaisesFeedback) {
  auto saved = config::input.forward_rumble;
  auto mail = std::make_shared<safe::mail_raw_t>();
  auto queue = mail->queue<gamepad_feedback_msg_t>("test_gamepad_feedback"sv);
  auto state = std::make_shared<joypad_state>();
  auto rumble = gamepad::rumble_handler(queue, 3, state);

  config::input.forward_rumble = true;
  rumble(1000, 2000);
  auto msg = queue->pop(0ms);
  ASSERT_TRUE(msg);
  EXPECT_EQ(msg->type, gamepad_feedback_e::rumble);
  EXPECT_EQ(msg->id, 3);
  EXPECT_EQ(msg->data.rumble.lowfreq, 1000);
  EXPECT_EQ(msg->data.rumble.highfreq, 2000);
  rumble(1000, 2000);  // Duplicate
  EXPECT_FALSE(queue->peek());

  config::input.forward_rumble = false;
  rumble(0, 0);
  EXPECT_FALSE(queue->peek());

  config::input.forward_rumble = saved;
}

// Bugs caught: 0, False positives: 0
TEST(InputOptionsTest, KeyMappingFollowsFlagsAndOption) {
  using input_options::key_mapping;
  using input_options::key_mapping_e;
  EXPECT_EQ(key_mapping(0, true), key_mapping_e::us_position);  // Normalized
  EXPECT_EQ(key_mapping(0, false), key_mapping_e::us_position);
  EXPECT_EQ(key_mapping(SS_KBE_FLAG_NON_NORMALIZED, true), key_mapping_e::host_layout);
  EXPECT_EQ(key_mapping(SS_KBE_FLAG_NON_NORMALIZED, false), key_mapping_e::us_position);
}

  #ifdef SUNSHINE_BUILD_WAYLAND

namespace {
  // The VK code whose US position types the character `vk` stands for on a host with these layouts.
  std::optional<std::uint16_t> host_vk(const char *layout, std::uint16_t vk, std::uint32_t group = 0) {
    keyboard_layout::names_t names;
    names.layout = layout;
    auto keymap = keyboard_layout::keymap_t::compile(names);
    if (!keymap) {
      return std::nullopt;
    }
    auto sym = keyboard_layout::vk_keysym(vk);
    if (!sym) {
      return vk;
    }
    auto code = keyboard_layout::evdev_code(keymap->get(), group, *sym);
    if (!code) {
      return vk;  // Not on this layout: keeps its US position
    }
    return keyboard_layout::vk_for_evdev(*code);
  }
}  // namespace

// Bugs caught: 0, False positives: 0
TEST(KeyboardLayoutTest, NonNormalizedKeysUseTheHostLayout) {
  if (!keyboard_layout::keymap_t::compile({.layout = "fr"})) {
    GTEST_SKIP() << "xkeyboard-config is not installed";
  }
  // US host: letters and digits stay where they are.
  for (std::uint16_t vk : {'A', 'Q', 'Z', 'M', '0', '7'}) {
    EXPECT_EQ(host_vk("us", vk), vk) << char(vk);
  }
  // French host (AZERTY): the key with 'a' is the US Q position, 'm' sits on US ';', digits are shifted on the
  // number row.
  EXPECT_EQ(host_vk("fr", 'A'), 'Q');
  EXPECT_EQ(host_vk("fr", 'Q'), 'A');
  EXPECT_EQ(host_vk("fr", 'Z'), 'W');
  EXPECT_EQ(host_vk("fr", 'M'), 0xBA);  // VK_OEM_1, KEY_SEMICOLON
  EXPECT_EQ(host_vk("fr", '1'), '1');
  // German host (QWERTZ).
  EXPECT_EQ(host_vk("de", 'Z'), 'Y');
  EXPECT_EQ(host_vk("de", 'Y'), 'Z');
  // Non-letter keys are never translated.
  EXPECT_EQ(host_vk("fr", 0xBA), 0xBA);
  // Second group of "us,ru" (Russian active): no Latin letters, so the US position is kept, as on Windows.
  EXPECT_EQ(host_vk("us,ru", 'A', 1), 'A');
  EXPECT_EQ(host_vk("us,ru", 'A', 0), 'A');
}

// Bugs caught: 0, False positives: 0
TEST(KeyboardLayoutTest, FindsTheUinputKeyboardInHyprlandDevices) {
  auto devices = nlohmann::json::parse(R"({
    "mice": [],
    "keyboards": [
      {"name": "at-translated-set-2-keyboard", "rules": "", "model": "", "layout": "us", "variant": "", "options": "", "active_layout_index": 0},
      {"name": "keyboard-passthrough-extra", "layout": "xx"},
      {"name": "keyboard-passthrough-1", "rules": "evdev", "model": "pc105", "layout": "us,de", "variant": ",nodeadkeys", "options": "grp:alt_shift_toggle", "active_layout_index": 1}
    ]
  })");
  auto kb = keyboard_layout::find_keyboard(devices, "Keyboard passthrough");
  ASSERT_TRUE(kb);
  EXPECT_EQ(kb->layout, "us,de");
  EXPECT_EQ(kb->variant, ",nodeadkeys");
  EXPECT_EQ(kb->options, "grp:alt_shift_toggle");
  EXPECT_EQ(kb->rules, "evdev");
  EXPECT_EQ(kb->model, "pc105");
  EXPECT_EQ(kb->active_layout, 1u);

  EXPECT_FALSE(keyboard_layout::find_keyboard(nlohmann::json::parse(R"({"keyboards": []})"), "Keyboard passthrough"));
  EXPECT_FALSE(keyboard_layout::find_keyboard(nlohmann::json::parse("[]"), "Keyboard passthrough"));
}

// Bugs caught: 0, False positives: 0
TEST(KeyboardLayoutTest, OnlyLettersAndDigitsHaveKeysyms) {
  EXPECT_TRUE(keyboard_layout::vk_keysym('A'));
  EXPECT_TRUE(keyboard_layout::vk_keysym('9'));
  EXPECT_FALSE(keyboard_layout::vk_keysym(0x10));  // Shift
  EXPECT_FALSE(keyboard_layout::vk_keysym(0xBA));  // OEM keys mean different characters per client layout
  EXPECT_EQ(keyboard_layout::vk_for_evdev(KEY_Q), 'Q');
  EXPECT_FALSE(keyboard_layout::vk_for_evdev(0x2ff));
}

  #endif
#endif
