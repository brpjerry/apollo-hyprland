/**
 * @file tests/unit/platform/linux/test_virtualhid_uinput_devices.cpp
 * @brief Verify what libvirtualhid's Linux touch screen and pen tablet (with Apollo's libvirtualhid patch) send:
 *        palm cancels, contact area, pen rotation and proximity.
 * @details libvirtualhid's test hooks run the uinput devices on a pipe (the events they write) and on a fake libevdev
 *          (the capabilities they declare), so no uinput device is created. What libinput makes of these events is
 *          checked by TouchPenUinputTest, which needs real uinput.
 */

// test includes
#include "../../../tests_common.h"

// standard includes
#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

// platform includes
#include <linux/input.h>

// lib includes
#include <fixtures/linux_backend_test_hooks.hpp>

namespace {
  using lvh::detail::test::LinuxInputEventRecord;
  using frame_t = std::vector<LinuxInputEventRecord>;

  /**
   * @brief Split events into frames, each ending before its SYN_REPORT.
   * @param events Events in the order the device wrote them.
   * @return The frames.
   */
  std::vector<frame_t> frames(const std::vector<LinuxInputEventRecord> &events) {
    std::vector<frame_t> out {{}};
    for (const auto &event : events) {
      if (event.type == EV_SYN && event.code == SYN_REPORT) {
        out.emplace_back();
      } else {
        out.back().push_back(event);
      }
    }
    out.pop_back();
    return out;
  }

  /**
   * @brief Position of an event in a frame.
   * @param frame Frame to search.
   * @param type Event type.
   * @param code Event code.
   * @param value Event value.
   * @return Index of the first match, or -1.
   */
  int find(const frame_t &frame, std::uint16_t type, std::uint16_t code, std::int32_t value) {
    const auto it = std::ranges::find_if(frame, [&](const auto &event) {
      return event.type == type && event.code == code && event.value == value;
    });
    return it == frame.end() ? -1 : static_cast<int>(it - frame.begin());
  }

  /**
   * @brief Value of the last event with a code in a frame.
   * @param frame Frame to search.
   * @param type Event type.
   * @param code Event code.
   * @return The value, or nullopt when the frame has no such event.
   */
  std::optional<std::int32_t> value(const frame_t &frame, std::uint16_t type, std::uint16_t code) {
    std::optional<std::int32_t> out;
    for (const auto &event : frame) {
      if (event.type == type && event.code == code) {
        out = event.value;
      }
    }
    return out;
  }

  /**
   * @brief Absolute axis declared by a fake libevdev device.
   * @param device Device definition.
   * @param code ABS_* code.
   * @return The axis, or nullptr.
   */
  const lvh::detail::test::LinuxLibevdevEventCode *abs_axis(const lvh::detail::test::LinuxLibevdevCreationResult &device, std::uint32_t code) {
    const auto it = std::ranges::find_if(device.event_codes, [code](const auto &event_code) {
      return event_code.type == EV_ABS && event_code.code == code;
    });
    return it == device.event_codes.end() ? nullptr : &*it;
  }

  /**
   * @brief A touch contact in the middle of a 1920x1080 viewport.
   * @param id Contact id.
   * @param major Major axis in viewport pixels.
   * @param minor Minor axis in viewport pixels.
   * @param orientation Degrees clockwise from vertical.
   * @return The contact.
   */
  lvh::TouchContact contact(std::int32_t id, float major = 0.0F, float minor = 0.0F, std::int32_t orientation = 0) {
    lvh::TouchContact out;
    out.id = id;
    out.x = 0.5F;
    out.y = 0.5F;
    out.pressure = 0.5F;
    out.orientation = orientation;
    out.viewport.width = 1920;
    out.viewport.height = 1080;
    out.contact_major_axis = major;
    out.contact_minor_axis = minor;
    return out;
  }

  /**
   * @brief Release a contact.
   * @param id Contact id.
   * @param transition How it ends.
   * @return The step.
   */
  lvh::detail::test::LinuxTouchStep release(std::int32_t id, lvh::PointerTransition transition) {
    return {.contact = contact(id), .release = transition};
  }

  /**
   * @brief A pen tool state.
   * @param tool Tool.
   * @param transition Transition.
   * @param pressure Pressure, or negative for a hover.
   * @param rotation Barrel rotation, or negative when unknown.
   * @return The step.
   */
  lvh::detail::test::LinuxPenStep tool_step(lvh::PenToolType tool, lvh::PointerTransition transition = lvh::PointerTransition::update, float pressure = -1.0F, float rotation = -1.0F) {
    lvh::PenToolState state;
    state.tool = tool;
    state.x = 0.25F;
    state.y = 0.25F;
    state.pressure = pressure;
    state.distance = pressure < 0.0F ? 0.5F : -1.0F;
    state.rotation = rotation;
    state.transition = transition;
    return {.state = state};
  }

  /**
   * @brief A pen button change.
   * @param button Button.
   * @param pressed Whether it is pressed.
   * @return The step.
   */
  lvh::detail::test::LinuxPenStep button_step(lvh::PenButton button, bool pressed) {
    return {.button = std::pair {button, pressed}};
  }
}  // namespace

// Bugs caught: 0, False positives: 0
TEST(VirtualHidUinputDevicesTest, TouchScreenDeclaresContactAreaAndToolType) {
  const auto touchscreen = lvh::detail::test::linux_uinput_create_fake_libevdev_device(lvh::DeviceType::touchscreen);
  ASSERT_TRUE(touchscreen.status.ok()) << touchscreen.status.message();
  for (const auto code : {ABS_MT_TOUCH_MAJOR, ABS_MT_TOUCH_MINOR}) {
    const auto *axis = abs_axis(touchscreen, code);
    ASSERT_NE(axis, nullptr) << code;
    EXPECT_EQ(axis->minimum, 0);
    EXPECT_EQ(axis->maximum, 19200);  // The position units (19200 x 10800)
  }
  const auto *tool_type = abs_axis(touchscreen, ABS_MT_TOOL_TYPE);
  ASSERT_NE(tool_type, nullptr);
  EXPECT_EQ(tool_type->maximum, MT_TOOL_MAX);
  ASSERT_NE(abs_axis(touchscreen, ABS_MT_ORIENTATION), nullptr);

  // The trackpad has no palms or contact area: libinput would use them for its own palm and thumb detection.
  const auto trackpad = lvh::detail::test::linux_uinput_create_fake_libevdev_device(lvh::DeviceType::trackpad);
  ASSERT_TRUE(trackpad.status.ok()) << trackpad.status.message();
  EXPECT_EQ(abs_axis(trackpad, ABS_MT_TOOL_TYPE), nullptr);
  EXPECT_EQ(abs_axis(trackpad, ABS_MT_TOUCH_MAJOR), nullptr);
}

// Bugs caught: 0, False positives: 0
TEST(VirtualHidUinputDevicesTest, PenTabletDeclaresRotation) {
  const auto pen_tablet = lvh::detail::test::linux_uinput_create_fake_libevdev_device(lvh::DeviceType::pen_tablet);
  ASSERT_TRUE(pen_tablet.status.ok()) << pen_tablet.status.message();
  const auto *rotation = abs_axis(pen_tablet, ABS_Z);
  ASSERT_NE(rotation, nullptr);
  EXPECT_EQ(rotation->minimum, 0);
  EXPECT_EQ(rotation->maximum, 359);  // One unit per degree
}

// Bugs caught: 0, False positives: 0
TEST(VirtualHidUinputDevicesTest, CancelledTouchIsAPalm) {
  const auto result = lvh::detail::test::linux_uinput_touchscreen_steps_pipe({
    {.contact = contact(1)},
    release(1, lvh::PointerTransition::cancel),
    {.contact = contact(2)},
    release(2, lvh::PointerTransition::release),
    {.contact = contact(3)},
    release(3, lvh::PointerTransition::leave),
  });
  ASSERT_TRUE(result.status.ok()) << result.status.message();
  const auto f = frames(result.events);
  ASSERT_EQ(f.size(), 6u);

  // The cancel turns the contact into a palm before it lifts, in one frame: libinput cancels it.
  EXPECT_EQ(find(f[0], EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_PALM), -1);
  const auto palm = find(f[1], EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_PALM);
  const auto lift = find(f[1], EV_ABS, ABS_MT_TRACKING_ID, -1);
  ASSERT_GE(palm, 0);
  ASSERT_GE(lift, 0);
  EXPECT_LT(palm, lift);

  // The next contact in that slot is a finger again, before its tracking id starts.
  const auto finger = find(f[2], EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_FINGER);
  ASSERT_GE(finger, 0);
  const auto tracking = find(f[2], EV_ABS, ABS_MT_TRACKING_ID, 1);
  ASSERT_GE(tracking, 0);
  EXPECT_LT(finger, tracking);

  // A lift and a hover leaving are not palms, and the slot needs no reset afterwards.
  EXPECT_FALSE(value(f[3], EV_ABS, ABS_MT_TOOL_TYPE));
  EXPECT_GE(find(f[3], EV_ABS, ABS_MT_TRACKING_ID, -1), 0);
  EXPECT_FALSE(value(f[4], EV_ABS, ABS_MT_TOOL_TYPE));
  EXPECT_FALSE(value(f[5], EV_ABS, ABS_MT_TOOL_TYPE));
}

// Bugs caught: 0, False positives: 0
TEST(VirtualHidUinputDevicesTest, CancelAllLeavesNoPalmBehind) {
  // Two contacts cancelled: both become palms, and both slots are fingers again for the next contacts.
  const auto result = lvh::detail::test::linux_uinput_touchscreen_steps_pipe({
    {.contact = contact(1)},
    {.contact = contact(2)},
    release(1, lvh::PointerTransition::cancel),
    release(2, lvh::PointerTransition::cancel),
    {.contact = contact(3)},
    {.contact = contact(4)},
  });
  ASSERT_TRUE(result.status.ok()) << result.status.message();
  const auto f = frames(result.events);
  ASSERT_EQ(f.size(), 6u);
  EXPECT_GE(find(f[2], EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_PALM), 0);
  EXPECT_GE(find(f[3], EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_PALM), 0);
  EXPECT_GE(find(f[3], EV_KEY, BTN_TOUCH, 0), 0);
  EXPECT_GE(find(f[4], EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_FINGER), 0);
  EXPECT_GE(find(f[5], EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_FINGER), 0);
}

// Bugs caught: 0, False positives: 0
TEST(VirtualHidUinputDevicesTest, TouchCarriesItsContactArea) {
  const auto result = lvh::detail::test::linux_uinput_touchscreen_steps_pipe({
    // 30 x 20 pixels, major axis vertical: 10 device units per pixel either way on a 16:9 viewport.
    {.contact = contact(1, 30.0F, 20.0F, 0)},
    // Unknown minor axis: a round contact.
    {.contact = contact(1, 30.0F, 0.0F, 0)},
    // Unknown area.
    {.contact = contact(1)},
  });
  ASSERT_TRUE(result.status.ok()) << result.status.message();
  const auto f = frames(result.events);
  ASSERT_EQ(f.size(), 3u);
  EXPECT_EQ(value(f[0], EV_ABS, ABS_MT_TOUCH_MAJOR), 300);
  EXPECT_EQ(value(f[0], EV_ABS, ABS_MT_TOUCH_MINOR), 200);
  EXPECT_EQ(value(f[1], EV_ABS, ABS_MT_TOUCH_MAJOR), 300);
  EXPECT_EQ(value(f[1], EV_ABS, ABS_MT_TOUCH_MINOR), 300);
  EXPECT_EQ(value(f[2], EV_ABS, ABS_MT_TOUCH_MAJOR), 0);
  EXPECT_EQ(value(f[2], EV_ABS, ABS_MT_TOUCH_MINOR), 0);
}

// Bugs caught: 0, False positives: 0
TEST(VirtualHidUinputDevicesTest, ContactAreaFollowsOrientationOnASquareViewport) {
  // A square viewport has more device units per pixel on X (19200 / 1000) than on Y (10800 / 1000), so the axes
  // depend on which way the ellipse points.
  auto vertical = contact(1, 30.0F, 20.0F, 0);
  vertical.viewport.width = vertical.viewport.height = 1000;
  auto horizontal = vertical;
  horizontal.orientation = 90;
  const auto result = lvh::detail::test::linux_uinput_touchscreen_steps_pipe({{.contact = vertical}, {.contact = horizontal}});
  ASSERT_TRUE(result.status.ok()) << result.status.message();
  const auto f = frames(result.events);
  ASSERT_EQ(f.size(), 2u);
  // Vertical: major 30 * 10.8 = 324 and minor 20 * 19.2 = 384, reported larger first.
  EXPECT_EQ(value(f[0], EV_ABS, ABS_MT_TOUCH_MAJOR), 384);
  EXPECT_EQ(value(f[0], EV_ABS, ABS_MT_TOUCH_MINOR), 324);
  // Horizontal: major 30 * 19.2 = 576 and minor 20 * 10.8 = 216.
  EXPECT_EQ(value(f[1], EV_ABS, ABS_MT_TOUCH_MAJOR), 576);
  EXPECT_EQ(value(f[1], EV_ABS, ABS_MT_TOUCH_MINOR), 216);
}

// Bugs caught: 0, False positives: 0
TEST(VirtualHidUinputDevicesTest, PenRotationIsAbsZ) {
  using enum lvh::PenToolType;
  const auto result = lvh::detail::test::linux_uinput_pen_tablet_steps_pipe({
    tool_step(pen, lvh::PointerTransition::update, -1.0F, 30.0F),
    tool_step(pen, lvh::PointerTransition::update, -1.0F, 359.6F),
    tool_step(pen, lvh::PointerTransition::update, -1.0F, -1.0F),
  });
  ASSERT_TRUE(result.status.ok()) << result.status.message();
  const auto f = frames(result.events);
  ASSERT_EQ(f.size(), 3u);
  // libinput reports ABS_Z as (value + 90) mod 360 degrees.
  EXPECT_EQ(value(f[0], EV_ABS, ABS_Z), 300);
  EXPECT_EQ(value(f[1], EV_ABS, ABS_Z), 270);  // 360 is 0
  // Unknown rotation leaves the axis alone.
  EXPECT_FALSE(value(f[2], EV_ABS, ABS_Z));
}

// Bugs caught: 0, False positives: 0
TEST(VirtualHidUinputDevicesTest, PenLeavesProximity) {
  using enum lvh::PenToolType;
  using enum lvh::PointerTransition;
  const auto result = lvh::detail::test::linux_uinput_pen_tablet_steps_pipe({
    tool_step(pen),
    tool_step(pen, leave),
    tool_step(unchanged),
    tool_step(unchanged, update, 0.5F),
    tool_step(unchanged, cancel),
    tool_step(unchanged, cancel),
  });
  ASSERT_TRUE(result.status.ok()) << result.status.message();
  const auto f = frames(result.events);
  // The second cancel finds the pen out of proximity already and sends nothing.
  ASSERT_EQ(f.size(), 5u);

  EXPECT_GE(find(f[0], EV_KEY, BTN_TOOL_PEN, 1), 0);
  // Leaving: the tool goes out of proximity, without moving it.
  EXPECT_GE(find(f[1], EV_KEY, BTN_TOOL_PEN, 0), 0);
  EXPECT_FALSE(value(f[1], EV_ABS, ABS_X));
  // The next event brings the same tool back, though the client did not name it.
  EXPECT_GE(find(f[2], EV_KEY, BTN_TOOL_PEN, 1), 0);
  EXPECT_GE(find(f[3], EV_KEY, BTN_TOUCH, 1), 0);
  EXPECT_FALSE(value(f[3], EV_KEY, BTN_TOOL_PEN));
  // A cancel during contact lifts the tip and leaves proximity.
  EXPECT_GE(find(f[4], EV_KEY, BTN_TOUCH, 0), 0);
  EXPECT_GE(find(f[4], EV_ABS, ABS_PRESSURE, 0), 0);
  EXPECT_GE(find(f[4], EV_KEY, BTN_TOOL_PEN, 0), 0);
}

// Bugs caught: 0, False positives: 0
TEST(VirtualHidUinputDevicesTest, PenButtonsFollowProximity) {
  using enum lvh::PenToolType;
  using enum lvh::PointerTransition;
  const auto result = lvh::detail::test::linux_uinput_pen_tablet_steps_pipe({
    // Out of proximity: the press waits for the tool.
    button_step(lvh::PenButton::secondary, true),
    tool_step(pen),
    button_step(lvh::PenButton::primary, true),
    tool_step(pen, leave),
    tool_step(pen),
    button_step(lvh::PenButton::primary, false),
  });
  ASSERT_TRUE(result.status.ok()) << result.status.message();
  const auto f = frames(result.events);
  ASSERT_EQ(f.size(), 5u);

  // In: the held button is pressed in the tool's first frame, after the tool.
  const auto tool_in = find(f[0], EV_KEY, BTN_TOOL_PEN, 1);
  const auto held = find(f[0], EV_KEY, BTN_STYLUS2, 1);
  ASSERT_GE(tool_in, 0);
  ASSERT_GE(held, 0);
  EXPECT_LT(tool_in, held);
  EXPECT_GE(find(f[1], EV_KEY, BTN_STYLUS, 1), 0);
  // Out: the buttons are released before the tool goes.
  const auto tool_out = find(f[2], EV_KEY, BTN_TOOL_PEN, 0);
  ASSERT_GE(tool_out, 0);
  EXPECT_LT(find(f[2], EV_KEY, BTN_STYLUS, 0), tool_out);
  EXPECT_LT(find(f[2], EV_KEY, BTN_STYLUS2, 0), tool_out);
  EXPECT_GE(find(f[2], EV_KEY, BTN_STYLUS, 0), 0);
  EXPECT_GE(find(f[2], EV_KEY, BTN_STYLUS2, 0), 0);
  // Back in: still held, so pressed again.
  EXPECT_GE(find(f[3], EV_KEY, BTN_STYLUS, 1), 0);
  EXPECT_GE(find(f[3], EV_KEY, BTN_STYLUS2, 1), 0);
  ASSERT_EQ(f[4].size(), 1u);
  EXPECT_EQ(find(f[4], EV_KEY, BTN_STYLUS, 0), 0);
}

// Bugs caught: 0, False positives: 0
TEST(VirtualHidUinputDevicesTest, PenToolChangeLeavesProximityFirst) {
  using enum lvh::PenToolType;
  const auto result = lvh::detail::test::linux_uinput_pen_tablet_steps_pipe({tool_step(pen), tool_step(eraser)});
  ASSERT_TRUE(result.status.ok()) << result.status.message();
  const auto f = frames(result.events);
  ASSERT_EQ(f.size(), 3u);
  // libinput needs the pen out in its own frame before the eraser comes in.
  EXPECT_GE(find(f[1], EV_KEY, BTN_TOOL_PEN, 0), 0);
  EXPECT_FALSE(value(f[1], EV_KEY, BTN_TOOL_RUBBER));
  EXPECT_GE(find(f[2], EV_KEY, BTN_TOOL_RUBBER, 1), 0);
  EXPECT_FALSE(value(f[2], EV_KEY, BTN_TOOL_PEN));
}
