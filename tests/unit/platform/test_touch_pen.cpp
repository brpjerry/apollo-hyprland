/**
 * @file tests/unit/platform/test_touch_pen.cpp
 * @brief Test the touch and pen event fixes in src/platform/linux/input/touch_pen.* and the per-client touch screen
 *        and pen tablet of the Linux input entry points (src/platform/linux/input/virtualhid.cpp), on libvirtualhid's
 *        fake backend. No uinput devices are created.
 */
#include "../../tests_common.h"

#ifdef __linux__

  // standard includes
  #include <memory>
  #include <string>
  #include <utility>
  #include <vector>

  // local includes
  #include <src/platform/linux/input/abs_input.h>
  #include <src/platform/linux/input/touch_pen.h>
  #include <src/platform/virtualhid_input.h>

using namespace platf::touch_pen;

namespace {
  platf::touch_input_t touch(std::uint8_t type, std::uint32_t id, float x = 0.5f, float y = 0.5f, float pressure = 0.5f) {
    return {type, LI_ROT_UNKNOWN, id, x, y, pressure, 0, 0};
  }

  platf::pen_input_t pen(std::uint8_t type, std::uint8_t tool, float x = 0.5f, float y = 0.5f, float pressure = 0.5f, std::uint8_t buttons = 0) {
    return {type, tool, buttons, LI_TILT_UNKNOWN, LI_ROT_UNKNOWN, x, y, pressure, 0, 0};
  }

  struct fake_compositor_t: platf::abs_input::compositor_t {
    std::vector<platf::abs_input::output_rect_t> monitors {{"APOLLO-TEST-1", 4000, 0, 1920, 1080}};
    std::vector<std::pair<std::string, std::string>> binds;

    platf::abs_input::outputs_t outputs(bool) override {
      return std::make_shared<std::vector<platf::abs_input::output_rect_t>>(monitors);
    }

    bool bind_to_output(const std::string &device, const std::string &output) override {
      binds.emplace_back(device, output);
      return true;
    }

    bool set_flat_accel(const std::string &) override {
      return true;
    }

    std::uint64_t generation() const override {
      return 1;
    }
  };

  /**
   * @brief A client's devices from the Linux platform input, on libvirtualhid's fake backend.
   */
  class LinuxTouchPenTest: public ::testing::Test {
  protected:
    void start(std::shared_ptr<platf::abs_input::compositor_t> compositor) {
      platf::abs_input::set_compositor_for_testing(std::move(compositor));
      input = platf::input();
      ASSERT_TRUE(input);
      platf::virtualhid::get_input_context(input) = platf::virtualhid::input_context_t {lvh::BackendKind::fake};
      client = platf::allocate_client_input_context(input);
      ASSERT_NE(context().touch, nullptr);
      ASSERT_NE(context().pen, nullptr);
    }

    void TearDown() override {
      client.reset();
      input.reset();
      platf::abs_input::set_compositor_for_testing(nullptr);
    }

    platf::virtualhid::client_context_t &context() {
      return platf::virtualhid::get_client_context(client.get());
    }

    platf::touch_port_t port() const {
      platf::touch_port_t port {4000, 0, 1920, 1080, 1920, 1080};
      port.output_name = "APOLLO-TEST-1";
      return port;
    }

    platf::input_t input;
    std::unique_ptr<platf::client_input_t> client;
  };
}  // namespace

// Bugs caught: 0, False positives: 0
TEST(TouchPenTest, TouchHoverIsNotATouch) {
  // Linux touch screens have no hover: hovering over nothing reports nothing.
  EXPECT_FALSE(touch_event(touch(LI_TOUCH_EVENT_HOVER, 1), false));
  // Hovering where a finger was down means it lifted.
  auto lifted = touch_event(touch(LI_TOUCH_EVENT_HOVER, 1), true);
  ASSERT_TRUE(lifted);
  EXPECT_EQ(lifted->eventType, LI_TOUCH_EVENT_UP);
  EXPECT_EQ(lifted->pointerId, 1u);
}

// Bugs caught: 0, False positives: 0
TEST(TouchPenTest, TouchDownMoveUp) {
  auto down = touch_event(touch(LI_TOUCH_EVENT_DOWN, 1, 0.25f, 0.75f, 0.8f), false);
  ASSERT_TRUE(down);
  EXPECT_EQ(down->eventType, LI_TOUCH_EVENT_DOWN);
  EXPECT_FLOAT_EQ(down->x, 0.25f);
  EXPECT_FLOAT_EQ(down->y, 0.75f);
  EXPECT_FLOAT_EQ(down->pressureOrDistance, 0.8f);
  auto up = touch_event(touch(LI_TOUCH_EVENT_UP, 1), true);
  ASSERT_TRUE(up);
  EXPECT_EQ(up->eventType, LI_TOUCH_EVENT_UP);
  auto cancel = touch_event(touch(LI_TOUCH_EVENT_CANCEL_ALL, 0), false);
  ASSERT_TRUE(cancel);
  EXPECT_EQ(cancel->eventType, LI_TOUCH_EVENT_CANCEL_ALL);
}

// Bugs caught: 0, False positives: 0
TEST(TouchPenTest, TouchOrientationAndPressure) {
  // Unknown pressure (0) is half pressure, for contacts only.
  auto down = touch_event(touch(LI_TOUCH_EVENT_DOWN, 1, 0.5f, 0.5f, 0.0f), false);
  ASSERT_TRUE(down);
  EXPECT_FLOAT_EQ(down->pressureOrDistance, 0.5f);
  auto move = touch_event(touch(LI_TOUCH_EVENT_MOVE, 1, 0.5f, 0.5f, 0.0f), true);
  ASSERT_TRUE(move);
  EXPECT_FLOAT_EQ(move->pressureOrDistance, 0.5f);
  auto t = touch(LI_TOUCH_EVENT_MOVE, 1);
  t.rotation = 120;
  auto rotated = touch_event(t, true);
  ASSERT_TRUE(rotated);
  EXPECT_EQ(rotated->rotation, 120);
}

// Bugs caught: 0, False positives: 0
TEST(TouchPenTest, PenKeepsItsToolAndPosition) {
  pen_state_t state;
  // An unknown tool is a pen, so it still comes into proximity.
  EXPECT_FALSE(state.in_proximity);
  auto first = pen_event(pen(LI_TOUCH_EVENT_HOVER, LI_TOOL_TYPE_UNKNOWN, 0.25f, 0.75f), state);
  EXPECT_EQ(first.toolType, LI_TOOL_TYPE_PEN);
  EXPECT_TRUE(state.in_proximity);
  auto eraser = pen_event(pen(LI_TOUCH_EVENT_HOVER, LI_TOOL_TYPE_ERASER, 0.3f, 0.7f), state);
  EXPECT_EQ(eraser.toolType, LI_TOOL_TYPE_ERASER);
  // Then it stays the eraser until the client names another tool.
  auto next = pen_event(pen(LI_TOUCH_EVENT_MOVE, LI_TOOL_TYPE_UNKNOWN, 0.35f, 0.65f), state);
  EXPECT_EQ(next.toolType, LI_TOOL_TYPE_ERASER);

  // Leaving and cancels carry no position: keep the last one.
  auto leave = pen_event(pen(LI_TOUCH_EVENT_HOVER_LEAVE, LI_TOOL_TYPE_UNKNOWN, 0, 0), state);
  EXPECT_FLOAT_EQ(leave.x, 0.35f);
  EXPECT_FLOAT_EQ(leave.y, 0.65f);
  EXPECT_FALSE(state.in_proximity);
  auto cancel = pen_event({.eventType = LI_TOUCH_EVENT_CANCEL_ALL}, state);
  EXPECT_FLOAT_EQ(cancel.x, 0.35f);
  EXPECT_EQ(cancel.toolType, LI_TOOL_TYPE_ERASER);
}

// Bugs caught: 0, False positives: 0
TEST(TouchPenTest, PenContactWithUnknownPressureTouches) {
  pen_state_t state;
  EXPECT_FLOAT_EQ(pen_event(pen(LI_TOUCH_EVENT_DOWN, LI_TOOL_TYPE_PEN, 0.5f, 0.5f, 0.0f), state).pressureOrDistance, 0.5f);
  EXPECT_FLOAT_EQ(pen_event(pen(LI_TOUCH_EVENT_MOVE, LI_TOOL_TYPE_PEN, 0.5f, 0.5f, 0.0f), state).pressureOrDistance, 0.5f);
  EXPECT_FLOAT_EQ(pen_event(pen(LI_TOUCH_EVENT_MOVE, LI_TOOL_TYPE_PEN, 0.5f, 0.5f, 0.9f), state).pressureOrDistance, 0.9f);
  // A hover's value is a distance: zero stays zero.
  EXPECT_FLOAT_EQ(pen_event(pen(LI_TOUCH_EVENT_HOVER, LI_TOOL_TYPE_PEN, 0.5f, 0.5f, 0.0f), state).pressureOrDistance, 0.0f);
  EXPECT_TRUE(pen_buttons_only(pen(LI_TOUCH_EVENT_BUTTON_ONLY, LI_TOOL_TYPE_PEN)));
  EXPECT_FALSE(pen_buttons_only(pen(LI_TOUCH_EVENT_MOVE, LI_TOOL_TYPE_PEN)));
}

// Bugs caught: 0, False positives: 0
TEST(TouchPenTest, ClientDeviceNamesAreUnique) {
  EXPECT_EQ(client_device_suffix(4242, 3), " (client 4242-3)");
  EXPECT_NE(client_device_suffix(4242, 3), client_device_suffix(4242, 31));
  // The first client of two Apollo processes on one compositor.
  EXPECT_NE(client_device_suffix(4242, 1), client_device_suffix(4243, 1));
}

// Bugs caught: 0, False positives: 0
TEST_F(LinuxTouchPenTest, DevicesAreNamedPerClientAndBoundToTheStreamedOutput) {
  auto fake = std::make_shared<fake_compositor_t>();
  start(fake);
  auto other = platf::allocate_client_input_context(input);
  const auto &other_context = platf::virtualhid::get_client_context(other.get());
  EXPECT_TRUE(context().touch->profile().name.starts_with(lvh::profiles::touchscreen().name + " (client "));
  EXPECT_TRUE(context().pen->profile().name.starts_with(lvh::profiles::pen_tablet().name + " (client "));
  EXPECT_NE(context().touch->profile().name, other_context.touch->profile().name);
  EXPECT_NE(context().pen->profile().name, other_context.pen->profile().name);
  other.reset();
  fake->binds.clear();

  platf::touch_update(client.get(), port(), touch(LI_TOUCH_EVENT_DOWN, 7, 0.25f, 0.5f));
  ASSERT_EQ(fake->binds.size(), 1u);
  EXPECT_EQ(fake->binds[0], (std::pair {context().touch->profile().name, std::string {"APOLLO-TEST-1"}}));
  auto contact = context().touch->last_submitted_contact();
  EXPECT_EQ(contact.id, 7);
  // Bound: the position is in libinput units.
  EXPECT_FLOAT_EQ(contact.x, platf::abs_input::libinput_fraction(0.25, platf::abs_input::touch_axis_max_x));
  EXPECT_FLOAT_EQ(contact.y, platf::abs_input::libinput_fraction(0.5, platf::abs_input::touch_axis_max_y));
  // Bound by Hyprland: libvirtualhid gets no output name, so it does not try KWin on every event.
  EXPECT_TRUE(contact.viewport.output_name.empty());

  platf::pen_update(client.get(), port(), pen(LI_TOUCH_EVENT_HOVER, LI_TOOL_TYPE_PEN));
  ASSERT_EQ(fake->binds.size(), 2u);
  EXPECT_EQ(fake->binds[1].first, context().pen->profile().name);
  EXPECT_TRUE(context().pen->last_submitted_tool().viewport.output_name.empty());

  // Leaving unbinds both.
  const auto touch_name = context().touch->profile().name;
  const auto pen_name = context().pen->profile().name;
  client.reset();
  ASSERT_EQ(fake->binds.size(), 4u);
  EXPECT_EQ(fake->binds[2], (std::pair {touch_name, std::string {}}));
  EXPECT_EQ(fake->binds[3], (std::pair {pen_name, std::string {}}));
}

// Bugs caught: 0, False positives: 0
TEST_F(LinuxTouchPenTest, WithoutHyprlandTheOutputNameGoesToLibvirtualhid) {
  start(nullptr);
  platf::touch_update(client.get(), port(), touch(LI_TOUCH_EVENT_DOWN, 1, 0.25f, 0.5f));
  EXPECT_EQ(context().touch->last_submitted_contact().viewport.output_name, "APOLLO-TEST-1");
  EXPECT_FLOAT_EQ(context().touch->last_submitted_contact().x, 0.25f);  // Sunshine's coordinates, unchanged
  platf::pen_update(client.get(), port(), pen(LI_TOUCH_EVENT_HOVER, LI_TOOL_TYPE_PEN));
  EXPECT_EQ(context().pen->last_submitted_tool().viewport.output_name, "APOLLO-TEST-1");
}

// Bugs caught: 0, False positives: 0
TEST_F(LinuxTouchPenTest, TouchHoverDoesNotTouchAndLiftsAContact) {
  start(nullptr);
  const auto count = context().touch->submit_count();
  platf::touch_update(client.get(), port(), touch(LI_TOUCH_EVENT_HOVER, 1));
  EXPECT_EQ(context().touch->submit_count(), count);
  EXPECT_TRUE(context().active_touches.empty());

  platf::touch_update(client.get(), port(), touch(LI_TOUCH_EVENT_DOWN, 1, 0.5f, 0.5f, 0.0f));
  EXPECT_TRUE(context().active_touches.contains(1));
  EXPECT_FLOAT_EQ(context().touch->last_submitted_contact().pressure, 0.5f);
  platf::touch_update(client.get(), port(), touch(LI_TOUCH_EVENT_HOVER, 1));
  EXPECT_FALSE(context().active_touches.contains(1));
}

// Bugs caught: 0, False positives: 0
TEST_F(LinuxTouchPenTest, PenButtonOnlyChangesButtons) {
  start(nullptr);
  platf::pen_update(client.get(), port(), pen(LI_TOUCH_EVENT_DOWN, LI_TOOL_TYPE_PEN, 0.25f, 0.75f, 0.0f));
  auto tool = context().pen->last_submitted_tool();
  EXPECT_EQ(tool.tool, lvh::PenToolType::pen);
  EXPECT_GT(tool.pressure, 0.0f);  // unknown pressure still touches
  const auto tools = context().pen->submit_count();

  // A barrel button press neither moves the pen to 0,0 nor lifts the tip.
  platf::pen_update(client.get(), port(), pen(LI_TOUCH_EVENT_BUTTON_ONLY, LI_TOOL_TYPE_UNKNOWN, 0, 0, 0, LI_PEN_BUTTON_PRIMARY));
  EXPECT_TRUE(context().pressed_pen_buttons.contains(lvh::PenButton::primary));
  EXPECT_EQ(context().pen->submit_count(), tools + 1);
  EXPECT_FLOAT_EQ(context().pen->last_submitted_tool().x, 0.25f);
  EXPECT_GT(context().pen->last_submitted_tool().pressure, 0.0f);
  platf::pen_update(client.get(), port(), pen(LI_TOUCH_EVENT_BUTTON_ONLY, LI_TOOL_TYPE_UNKNOWN, 0, 0, 0, 0));
  EXPECT_TRUE(context().pressed_pen_buttons.empty());

  // Leaving keeps the last position.
  platf::pen_update(client.get(), port(), pen(LI_TOUCH_EVENT_HOVER_LEAVE, LI_TOOL_TYPE_UNKNOWN, 0, 0));
  tool = context().pen->last_submitted_tool();
  EXPECT_EQ(tool.transition, lvh::PointerTransition::leave);
  EXPECT_FLOAT_EQ(tool.x, 0.25f);
  EXPECT_FLOAT_EQ(tool.y, 0.75f);
}

// Bugs caught: 0, False positives: 0
TEST_F(LinuxTouchPenTest, CancelAllLiftsEverything) {
  // What a leaving client's devices get before they close.
  start(nullptr);
  platf::touch_update(client.get(), port(), touch(LI_TOUCH_EVENT_DOWN, 1));
  platf::touch_update(client.get(), port(), touch(LI_TOUCH_EVENT_DOWN, 2));
  platf::pen_update(client.get(), port(), pen(LI_TOUCH_EVENT_DOWN, LI_TOOL_TYPE_PEN, 0.5f, 0.5f, 0.5f, LI_PEN_BUTTON_SECONDARY));
  ASSERT_EQ(context().active_touches.size(), 2u);
  ASSERT_FALSE(context().pressed_pen_buttons.empty());

  const auto touches = context().touch->submit_count();
  platf::touch_update(client.get(), port(), touch(LI_TOUCH_EVENT_CANCEL_ALL, 0));
  EXPECT_EQ(context().touch->submit_count(), touches + 2);
  EXPECT_TRUE(context().active_touches.empty());
  platf::pen_update(client.get(), port(), pen(LI_TOUCH_EVENT_CANCEL_ALL, LI_TOOL_TYPE_UNKNOWN, 0, 0, 0));
  EXPECT_EQ(context().pen->last_submitted_tool().transition, lvh::PointerTransition::cancel);
  EXPECT_FLOAT_EQ(context().pen->last_submitted_tool().x, 0.5f);
  EXPECT_TRUE(context().pressed_pen_buttons.empty());

  // And the destructor does the same with contacts still down.
  platf::touch_update(client.get(), port(), touch(LI_TOUCH_EVENT_DOWN, 3));
  client.reset();
}

// Bugs caught: 0, False positives: 0
TEST_F(LinuxTouchPenTest, PenRotationAndContactAreaReachLibvirtualhid) {
  // Bound by Hyprland, so the positions change units; rotation and contact area must still arrive.
  start(std::make_shared<fake_compositor_t>());
  auto rotated = pen(LI_TOUCH_EVENT_HOVER, LI_TOOL_TYPE_PEN);
  rotated.tilt = 45;
  rotated.rotation = 390;
  platf::pen_update(client.get(), port(), rotated);
  auto tool = context().pen->last_submitted_tool();
  EXPECT_FLOAT_EQ(tool.rotation, 30.0f);
  EXPECT_NE(tool.tilt_x, 0.0f);
  rotated.rotation = LI_ROT_UNKNOWN;
  platf::pen_update(client.get(), port(), rotated);
  EXPECT_LT(context().pen->last_submitted_tool().rotation, 0.0f);

  platf::touch_update(client.get(), port(), {LI_TOUCH_EVENT_DOWN, 90, 1, 0.5f, 0.5f, 0.5f, 30.0f, 20.0f});
  const auto contact = context().touch->last_submitted_contact();
  EXPECT_FLOAT_EQ(contact.contact_major_axis, 30.0f);
  EXPECT_FLOAT_EQ(contact.contact_minor_axis, 20.0f);
  EXPECT_EQ(contact.orientation, 90);
  // libvirtualhid scales the axes from the viewport's pixels.
  EXPECT_EQ(contact.viewport.width, 1920);
  EXPECT_EQ(contact.viewport.height, 1080);
}

#endif
