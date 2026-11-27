/**
 * @file tests/unit/platform/test_input_options.cpp
 * @brief Test how Linux input applies the keyboard and gamepad options (HYP-24, HYP-25) on libvirtualhid: the gamepad
 *        profiles, Back as touchpad click, forward_rumble, and keyboard_layout.* for non-normalized keys. Devices
 *        come from libvirtualhid's fake backend.
 */
#include "../../tests_common.h"

#ifdef __linux__

  // standard includes
  #include <chrono>
  #include <memory>

  // local includes
  #include <src/config.h>
  #include <src/platform/virtualhid_input.h>
  #include <src/thread_safe.h>

  #ifdef SUNSHINE_BUILD_WAYLAND
    #include <linux/input-event-codes.h>
    #include <nlohmann/json.hpp>
    #include <src/platform/linux/input/keyboard_layout.h>
  #endif

using namespace platf;
using namespace std::literals;

namespace {
  /**
   * @brief A fake libvirtualhid context with one gamepad, restoring the input options afterwards.
   */
  class LinuxGamepadOptionsTest: public ::testing::Test {
  protected:
    void SetUp() override {
      saved_ = config::input;
      context_ = std::make_unique<virtualhid::input_context_t>(lvh::BackendKind::fake);
      queue_ = mail_->queue<gamepad_feedback_msg_t>("linux-gamepad-options-test"sv);
    }

    void TearDown() override {
      if (context_) {
        virtualhid::free_gamepad(*context_, 0);
      }
      context_.reset();
      config::input = saved_;
    }

    lvh::GamepadStateAdapter *gamepad(std::string_view type, std::uint8_t client_type = LI_CTYPE_UNKNOWN) {
      config::input.gamepad = type;
      if (virtualhid::alloc_gamepad(*context_, {0, 2}, {client_type, 0, 0}, queue_) != 0) {
        ADD_FAILURE() << "no fake " << type << " gamepad";
        return nullptr;
      }
      return virtualhid::gamepad_adapter_for_testing(*context_, 0);
    }

    config::input_t saved_;
    std::shared_ptr<safe::mail_raw_t> mail_ = std::make_shared<safe::mail_raw_t>();
    std::unique_ptr<virtualhid::input_context_t> context_;
    feedback_queue_t queue_;
  };
}  // namespace

// Bugs caught: 0, False positives: 0
TEST_F(LinuxGamepadOptionsTest, WindowsGamepadNamesAreAliases) {
  // With inputtino, x360 and ds4 (Windows names in configs or per-app settings) meant xone and ds5. libvirtualhid
  // emulates both on Linux, so they are their own controllers now.
  auto *x360 = gamepad("x360"sv);
  ASSERT_NE(x360, nullptr);
  EXPECT_EQ(x360->gamepad()->profile().gamepad_kind, lvh::GamepadProfileKind::xbox_360);
  virtualhid::free_gamepad(*context_, 0);
  auto *ds4 = gamepad("ds4"sv);
  ASSERT_NE(ds4, nullptr);
  EXPECT_EQ(ds4->gamepad()->profile().gamepad_kind, lvh::GamepadProfileKind::dualshock4);

  // The controller touchpad capability follows the emulated controller.
  config::input.gamepad = "ds4";
  EXPECT_TRUE(virtualhid::configured_gamepad_supports_controller_extensions());
  config::input.gamepad = "x360";
  EXPECT_FALSE(virtualhid::configured_gamepad_supports_controller_extensions());

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
TEST_F(LinuxGamepadOptionsTest, BackAsTouchpadClickOnDualSense) {
  config::input.ds4_back_as_touchpad_click = true;
  auto *ds5 = gamepad("ds5"sv);
  ASSERT_NE(ds5, nullptr);
  virtualhid::gamepad_update(*context_, 0, {BACK | A, 0, 0, 0, 0, 0, 0});
  EXPECT_TRUE(ds5->state().buttons.test(lvh::GamepadButton::touchpad));
  EXPECT_TRUE(ds5->state().buttons.test(lvh::GamepadButton::back));
  virtualhid::gamepad_update(*context_, 0, {A, 0, 0, 0, 0, 0, 0});  // Back not pressed
  EXPECT_FALSE(ds5->state().buttons.test(lvh::GamepadButton::touchpad));

  config::input.ds4_back_as_touchpad_click = false;  // Option off
  virtualhid::gamepad_update(*context_, 0, {BACK, 0, 0, 0, 0, 0, 0});
  EXPECT_FALSE(ds5->state().buttons.test(lvh::GamepadButton::touchpad));
}

// Bugs caught: 0, False positives: 0
TEST_F(LinuxGamepadOptionsTest, RumbleForwarding) {
  auto *pad = gamepad("ds5"sv);
  ASSERT_NE(pad, nullptr);
  while (queue_->pop(0ms)) {
    // Motion sensor requests from the allocation.
  }

  lvh::GamepadOutput rumble;
  rumble.kind = lvh::GamepadOutputKind::rumble;
  rumble.low_frequency_rumble = 1000;
  rumble.high_frequency_rumble = 2000;
  lvh::GamepadOutput triggers;
  triggers.kind = lvh::GamepadOutputKind::trigger_rumble;
  triggers.left_trigger_rumble = 300;
  triggers.right_trigger_rumble = 400;

  // forward_rumble off: games' rumble stays on the host.
  config::input.forward_rumble = false;
  ASSERT_TRUE(pad->dispatch_output(rumble).ok());
  ASSERT_TRUE(pad->dispatch_output(triggers).ok());
  EXPECT_FALSE(queue_->pop(0ms));

  // On: forwarded with the client's controller number, and not resent when unchanged.
  config::input.forward_rumble = true;
  ASSERT_TRUE(pad->dispatch_output(rumble).ok());
  auto msg = queue_->pop(10ms);
  ASSERT_TRUE(msg);
  EXPECT_EQ(msg->type, gamepad_feedback_e::rumble);
  EXPECT_EQ(msg->id, 2);
  EXPECT_EQ(msg->data.rumble.lowfreq, 1000);
  EXPECT_EQ(msg->data.rumble.highfreq, 2000);
  ASSERT_TRUE(pad->dispatch_output(rumble).ok());
  EXPECT_FALSE(queue_->pop(0ms));
  ASSERT_TRUE(pad->dispatch_output(triggers).ok());
  msg = queue_->pop(10ms);
  ASSERT_TRUE(msg);
  EXPECT_EQ(msg->type, gamepad_feedback_e::rumble_triggers);
}

  #ifdef SUNSHINE_BUILD_WAYLAND

// Bugs caught: 0, False positives: 0
TEST(InputOptionsTest, KeyMappingFollowsFlagsAndOption) {
  using keyboard_layout::key_mapping;
  using keyboard_layout::key_mapping_e;
  EXPECT_EQ(key_mapping(0, true), key_mapping_e::us_position);  // Normalized
  EXPECT_EQ(key_mapping(0, false), key_mapping_e::us_position);
  EXPECT_EQ(key_mapping(SS_KBE_FLAG_NON_NORMALIZED, true), key_mapping_e::host_layout);
  EXPECT_EQ(key_mapping(SS_KBE_FLAG_NON_NORMALIZED, false), key_mapping_e::us_position);
}

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
  // Dvorak puts letters on punctuation keys of the US layout.
  EXPECT_EQ(host_vk("us(dvorak)", 'S'), 0xBA);  // KEY_SEMICOLON
  EXPECT_EQ(host_vk("us(dvorak)", 'Z'), 0xBF);  // KEY_SLASH
  // Non-letter keys are never translated.
  EXPECT_EQ(host_vk("fr", 0xBA), 0xBA);
  // Second group of "us,ru" (Russian active): no Latin letters, so the US position is kept, as on Windows.
  EXPECT_EQ(host_vk("us,ru", 'A', 1), 'A');
  EXPECT_EQ(host_vk("us,ru", 'A', 0), 'A');
}

// Bugs caught: 0, False positives: 0
TEST(KeyboardLayoutTest, FindsTheVirtualKeyboardInHyprlandDevices) {
  auto devices = nlohmann::json::parse(R"({
    "mice": [],
    "keyboards": [
      {"name": "at-translated-set-2-keyboard", "rules": "", "model": "", "layout": "us", "variant": "", "options": "", "active_layout_index": 0},
      {"name": "libvirtualhid-keyboard-extra", "layout": "xx"},
      {"name": "libvirtualhid-keyboard-1", "rules": "evdev", "model": "pc105", "layout": "us,de", "variant": ",nodeadkeys", "options": "grp:alt_shift_toggle", "active_layout_index": 1}
    ]
  })");
  auto kb = keyboard_layout::find_keyboard(devices, lvh::profiles::keyboard().name);
  ASSERT_TRUE(kb);
  EXPECT_EQ(kb->layout, "us,de");
  EXPECT_EQ(kb->variant, ",nodeadkeys");
  EXPECT_EQ(kb->options, "grp:alt_shift_toggle");
  EXPECT_EQ(kb->rules, "evdev");
  EXPECT_EQ(kb->model, "pc105");
  EXPECT_EQ(kb->active_layout, 1u);

  EXPECT_FALSE(keyboard_layout::find_keyboard(nlohmann::json::parse(R"({"keyboards": []})"), lvh::profiles::keyboard().name));
  EXPECT_FALSE(keyboard_layout::find_keyboard(nlohmann::json::parse("[]"), lvh::profiles::keyboard().name));
}

// Bugs caught: 0, False positives: 0
TEST(KeyboardLayoutTest, OnlyLettersAndDigitsHaveKeysyms) {
  EXPECT_TRUE(keyboard_layout::vk_keysym('A'));
  EXPECT_TRUE(keyboard_layout::vk_keysym('9'));
  EXPECT_FALSE(keyboard_layout::vk_keysym(0x10));  // Shift
  EXPECT_FALSE(keyboard_layout::vk_keysym(0xBA));  // OEM keys mean different characters per client layout
  EXPECT_EQ(keyboard_layout::vk_for_evdev(KEY_Q), 'Q');
  EXPECT_EQ(keyboard_layout::vk_for_evdev(KEY_7), '7');
  EXPECT_EQ(keyboard_layout::vk_for_evdev(KEY_102ND), 0xE2);
  EXPECT_FALSE(keyboard_layout::vk_for_evdev(0x2ff));
  EXPECT_FALSE(keyboard_layout::vk_for_evdev(KEY_LEFTSHIFT));
}

// Bugs caught: 0, False positives: 0
TEST(KeyboardLayoutTest, ReleasesMatchTheirPress) {
  // A non-normalized key goes to the keyboard as pressed and released; without Hyprland it keeps its US position.
  auto saved = config::input;
  config::input.always_send_scancodes = true;
  auto input = platf::input();
  ASSERT_TRUE(input);
  auto &context = virtualhid::get_input_context(input);
  context = virtualhid::input_context_t {lvh::BackendKind::fake};
  ASSERT_NE(context.keyboard, nullptr);
  platf::keyboard_update(input, 'A', false, SS_KBE_FLAG_NON_NORMALIZED);
  EXPECT_EQ(context.keyboard->last_submitted_event().key_code, 'A');
  EXPECT_TRUE(context.keyboard->last_submitted_event().pressed);
  platf::keyboard_update(input, 'A', true, SS_KBE_FLAG_NON_NORMALIZED);
  EXPECT_EQ(context.keyboard->last_submitted_event().key_code, 'A');
  EXPECT_FALSE(context.keyboard->last_submitted_event().pressed);
  // A release without a press still reaches the keyboard.
  platf::keyboard_update(input, 'B', true, 0);
  EXPECT_EQ(context.keyboard->last_submitted_event().key_code, 'B');
  config::input = saved;
}

  #endif
#endif
