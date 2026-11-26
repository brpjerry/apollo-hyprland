/**
 * @file tests/unit/platform/test_touch_pen.cpp
 * @brief Test the virtual touch screen and pen tablet event logic in src/platform/linux/input/touch_pen.*, and the
 *        capabilities in input_options.h. No uinput devices are created.
 */
#include "../../tests_common.h"

#ifdef __linux__

  #include <src/platform/linux/input/input_options.h>
  #include <src/platform/linux/input/touch_pen.h>

  #include <algorithm>
  #include <cmath>

using namespace platf::touch_pen;

namespace {
  contact_t at(int x, int y) {
    contact_t c;
    c.x = x;
    c.y = y;
    c.pressure = 100;
    return c;
  }

  bool has(const events_t &events, std::uint16_t type, std::uint16_t code, std::int32_t value) {
    return std::ranges::find(events, event_t {type, code, value}) != events.end();
  }

  bool has_code(const events_t &events, std::uint16_t type, std::uint16_t code) {
    return std::ranges::any_of(events, [&](const event_t &e) {
      return e.type == type && e.code == code;
    });
  }

  // The value an axis has after these events, as the kernel tracks it (last write wins), or nullopt.
  std::optional<int> last(const events_t &events, std::uint16_t type, std::uint16_t code) {
    std::optional<int> v;
    for (auto &e : events) {
      if (e.type == type && e.code == code) {
        v = e.value;
      }
    }
    return v;
  }

  bool ends_with_syn(const events_t &events) {
    return !events.empty() && events.back() == event_t {EV_SYN, SYN_REPORT, 0};
  }
}  // namespace

// Bugs caught: 0, False positives: 0
TEST(TouchPenTest, TouchHoverIsNotATouch) {
  touch_state_t touch;
  // Hovering fingers report nothing; the old code put the finger down.
  EXPECT_TRUE(touch.handle(LI_TOUCH_EVENT_HOVER, 7, at(100, 100)).empty());
  EXPECT_TRUE(touch.handle(LI_TOUCH_EVENT_HOVER_LEAVE, 7, {}).empty());
  EXPECT_EQ(touch.contacts(), 0);

  // A hover for a contact that is down means it lifted.
  touch.handle(LI_TOUCH_EVENT_DOWN, 7, at(100, 100));
  auto events = touch.handle(LI_TOUCH_EVENT_HOVER, 7, at(100, 100));
  EXPECT_TRUE(has(events, EV_ABS, ABS_MT_TRACKING_ID, -1));
  EXPECT_TRUE(has(events, EV_KEY, BTN_TOUCH, 0));
  EXPECT_FALSE(has(events, EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_PALM));  // A lift, not a cancel
  EXPECT_EQ(touch.contacts(), 0);
}

// Bugs caught: 0, False positives: 0
TEST(TouchPenTest, TouchDownMoveUp) {
  touch_state_t touch;
  contact_t c = at(1000, 2000);
  c.major = 300;
  c.minor = 200;
  c.orientation = -30;
  auto down = touch.handle(LI_TOUCH_EVENT_DOWN, 42, c);
  EXPECT_EQ(down, (events_t {
                    {EV_ABS, ABS_MT_TRACKING_ID, 0},
                    {EV_ABS, ABS_MT_POSITION_X, 1000},
                    {EV_ABS, ABS_MT_POSITION_Y, 2000},
                    {EV_ABS, ABS_MT_PRESSURE, 100},
                    {EV_ABS, ABS_MT_TOUCH_MAJOR, 300},
                    {EV_ABS, ABS_MT_TOUCH_MINOR, 200},
                    {EV_ABS, ABS_MT_ORIENTATION, -30},
                    {EV_ABS, ABS_X, 1000},
                    {EV_ABS, ABS_Y, 2000},
                    {EV_ABS, ABS_PRESSURE, 100},
                    {EV_KEY, BTN_TOUCH, 1},
                    {EV_SYN, SYN_REPORT, 0},
                  }));

  auto move = touch.handle(LI_TOUCH_EVENT_MOVE, 42, at(1100, 2100));
  EXPECT_FALSE(has_code(move, EV_ABS, ABS_MT_TRACKING_ID));
  EXPECT_FALSE(has_code(move, EV_KEY, BTN_TOUCH));
  EXPECT_TRUE(has(move, EV_ABS, ABS_MT_POSITION_X, 1100));
  EXPECT_TRUE(ends_with_syn(move));

  auto up = touch.handle(LI_TOUCH_EVENT_UP, 42, {});
  EXPECT_EQ(up, (events_t {
                  {EV_ABS, ABS_MT_TRACKING_ID, -1},
                  {EV_KEY, BTN_TOUCH, 0},
                  {EV_ABS, ABS_PRESSURE, 0},
                  {EV_SYN, SYN_REPORT, 0},
                }));
  // An unknown pointer lifting does nothing.
  EXPECT_TRUE(touch.handle(LI_TOUCH_EVENT_UP, 42, {}).empty());
}

// Bugs caught: 0, False positives: 0
TEST(TouchPenTest, TouchSlotsAreReusedWithoutCollisions) {
  touch_state_t touch;
  touch.handle(LI_TOUCH_EVENT_DOWN, 1, at(1, 1));  // slot 0
  auto second = touch.handle(LI_TOUCH_EVENT_DOWN, 2, at(2, 2));
  EXPECT_TRUE(has(second, EV_ABS, ABS_MT_SLOT, 1));
  EXPECT_FALSE(has_code(second, EV_KEY, BTN_TOUCH));  // Already down
  touch.handle(LI_TOUCH_EVENT_UP, 1, {});
  EXPECT_FALSE(has_code(touch.handle(LI_TOUCH_EVENT_MOVE, 2, at(3, 3)), EV_KEY, BTN_TOUCH));

  // A new finger takes the free slot 0, not slot 1 of the finger still down (inputtino reused it), with a new
  // tracking ID.
  auto third = touch.handle(LI_TOUCH_EVENT_DOWN, 3, at(4, 4));
  EXPECT_EQ(last(third, EV_ABS, ABS_MT_SLOT), 0);
  EXPECT_EQ(last(third, EV_ABS, ABS_MT_TRACKING_ID), 2);
  EXPECT_EQ(touch.contacts(), 2);

  // Moving finger 2 selects its slot again.
  EXPECT_EQ(last(touch.handle(LI_TOUCH_EVENT_MOVE, 2, at(5, 5)), EV_ABS, ABS_MT_SLOT), 1);

  // More fingers than slots: the extra ones are dropped.
  for (std::uint32_t id = 10; id < 10 + touch_axes::slots; ++id) {
    touch.handle(LI_TOUCH_EVENT_DOWN, id, at(6, 6));
  }
  EXPECT_EQ(touch.contacts(), touch_axes::slots);
  EXPECT_TRUE(touch.handle(LI_TOUCH_EVENT_DOWN, 99, at(7, 7)).empty());
}

// Bugs caught: 0, False positives: 0
TEST(TouchPenTest, TouchCancelAllLiftsEveryFingerAsCancelled) {
  touch_state_t touch;
  touch.handle(LI_TOUCH_EVENT_DOWN, 1, at(1, 1));
  touch.handle(LI_TOUCH_EVENT_DOWN, 2, at(2, 2));
  touch.handle(LI_TOUCH_EVENT_DOWN, 3, at(3, 3));
  touch.handle(LI_TOUCH_EVENT_UP, 2, {});

  auto events = touch.handle(LI_TOUCH_EVENT_CANCEL_ALL, 0, {});
  // Palm first (libinput cancels the touch), then the lift, for slots 0 and 2, in one frame.
  EXPECT_EQ(events, (events_t {
                      {EV_ABS, ABS_MT_SLOT, 0},
                      {EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_PALM},
                      {EV_ABS, ABS_MT_TRACKING_ID, -1},
                      {EV_ABS, ABS_MT_SLOT, 2},
                      {EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_PALM},
                      {EV_ABS, ABS_MT_TRACKING_ID, -1},
                      {EV_KEY, BTN_TOUCH, 0},
                      {EV_ABS, ABS_PRESSURE, 0},
                      {EV_SYN, SYN_REPORT, 0},
                    }));
  EXPECT_EQ(touch.contacts(), 0);
  EXPECT_TRUE(touch.handle(LI_TOUCH_EVENT_CANCEL_ALL, 0, {}).empty());

  // The next touch in a cancelled slot is a finger again, or libinput would ignore it as a palm.
  auto next = touch.handle(LI_TOUCH_EVENT_DOWN, 4, at(4, 4));
  EXPECT_EQ(last(next, EV_ABS, ABS_MT_SLOT), 0);
  auto tool = std::ranges::find(next, event_t {EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_FINGER});
  auto id = std::ranges::find_if(next, [](const event_t &e) {
    return e.code == ABS_MT_TRACKING_ID;
  });
  ASSERT_NE(tool, next.end());
  EXPECT_LT(tool, id);
}

// Bugs caught: 0, False positives: 0
TEST(TouchPenTest, TouchCancelOneFinger) {
  touch_state_t touch;
  touch.handle(LI_TOUCH_EVENT_DOWN, 1, at(1, 1));
  touch.handle(LI_TOUCH_EVENT_DOWN, 2, at(2, 2));
  auto events = touch.handle(LI_TOUCH_EVENT_CANCEL, 1, {});
  EXPECT_TRUE(has(events, EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_PALM));
  EXPECT_TRUE(has(events, EV_ABS, ABS_MT_TRACKING_ID, -1));
  EXPECT_FALSE(has_code(events, EV_KEY, BTN_TOUCH));  // Finger 2 is still down
  EXPECT_EQ(touch.contacts(), 1);
}

// Bugs caught: 0, False positives: 0
TEST(TouchPenTest, TouchOrientationAndPressure) {
  EXPECT_EQ(touch_orientation(LI_ROT_UNKNOWN), 0);  // Was folded to 90 before
  EXPECT_EQ(touch_orientation(0), 0);
  EXPECT_EQ(touch_orientation(45), 45);
  EXPECT_EQ(touch_orientation(90), 90);
  EXPECT_EQ(touch_orientation(135), -45);  // Same ellipse as 315 degrees
  EXPECT_EQ(touch_orientation(180), 0);
  EXPECT_EQ(touch_orientation(270), 90);
  EXPECT_EQ(touch_orientation(300), -60);

  EXPECT_EQ(touch_pressure(0.0f), touch_axes::pressure_max / 2);  // Unknown
  EXPECT_EQ(touch_pressure(1.0f), touch_axes::pressure_max);
  EXPECT_EQ(touch_pressure(0.001f), 1);  // Still a contact
}

// Bugs caught: 0, False positives: 0
TEST(TouchPenTest, ContactAreaInDeviceUnits) {
  // Unknown contact area.
  EXPECT_EQ(contact_area(0, 0, 0, 10, 10), (std::pair {0, 0}));
  EXPECT_EQ(contact_area(20, 0, 0, 10, 10), (std::pair {0, 0}));

  // Same scale on both axes: rotation doesn't matter.
  for (std::uint16_t rot : {0, 30, 90, 200, LI_ROT_UNKNOWN}) {
    auto [major, minor] = contact_area(20, 10, rot, 10, 10);
    EXPECT_EQ(major, 200) << rot;
    EXPECT_EQ(minor, 100) << rot;
  }

  // 10 units per pixel across, 20 down: a vertical major axis uses the vertical scale, the minor the horizontal one.
  EXPECT_EQ(contact_area(20, 10, 0, 10, 20), (std::pair {400, 100}));
  EXPECT_EQ(contact_area(20, 10, 90, 10, 20), (std::pair {200, 200}));

  // A 1920x1080 output bound to the 19200x10800 device: 10 units per pixel both ways.
  auto [major, minor] = contact_area(48, 32, LI_ROT_UNKNOWN, 19200.0 / 1920, 10800.0 / 1080);
  EXPECT_EQ(major, 480);
  EXPECT_EQ(minor, 320);

  // Huge areas are clamped to the axis range.
  EXPECT_EQ(contact_area(1e9f, 1e9f, 0, 10, 10).first, touch_axes::major_max);
}

// Bugs caught: 0, False positives: 0
TEST(TouchPenTest, PenHoverMovesWithoutTouching) {
  pen_state_t pen;
  pen_sample_t s;
  s.tool = BTN_TOOL_PEN;
  s.x = 500;
  s.y = 600;
  s.distance = 0.5f;
  auto events = pen.handle(LI_TOUCH_EVENT_HOVER, s);
  EXPECT_TRUE(has(events, EV_KEY, BTN_TOOL_PEN, 1));
  EXPECT_TRUE(has(events, EV_ABS, ABS_X, 500));
  EXPECT_TRUE(has(events, EV_ABS, ABS_DISTANCE, pen_axes::distance_max / 2));
  EXPECT_TRUE(has(events, EV_ABS, ABS_PRESSURE, 0));
  EXPECT_FALSE(has_code(events, EV_KEY, BTN_TOUCH));
  EXPECT_TRUE(ends_with_syn(events));
  EXPECT_FALSE(pen.touching());

  // Hovering on: no new proximity event.
  EXPECT_FALSE(has_code(pen.handle(LI_TOUCH_EVENT_HOVER, s), EV_KEY, BTN_TOOL_PEN));
}

// Bugs caught: 0, False positives: 0
TEST(TouchPenTest, PenContactAndProximity) {
  pen_state_t pen;
  pen_sample_t s;
  s.tool = BTN_TOOL_PEN;

  // Down straight from out of proximity, with unknown pressure: still a contact libinput detects.
  auto down = pen.handle(LI_TOUCH_EVENT_DOWN, s);
  EXPECT_TRUE(has(down, EV_KEY, BTN_TOOL_PEN, 1));
  EXPECT_TRUE(has(down, EV_KEY, BTN_TOUCH, 1));
  EXPECT_TRUE(has(down, EV_ABS, ABS_PRESSURE, pen_axes::pressure_max / 2));
  EXPECT_TRUE(has(down, EV_ABS, ABS_DISTANCE, 0));
  EXPECT_TRUE(pen.touching());

  s.pressure = 0.25f;
  auto move = pen.handle(LI_TOUCH_EVENT_MOVE, s);
  EXPECT_TRUE(has(move, EV_ABS, ABS_PRESSURE, pen_axes::pressure_max / 4));
  EXPECT_FALSE(has_code(move, EV_KEY, BTN_TOUCH));

  // Up: the tip lifts, the pen stays in proximity.
  auto up = pen.handle(LI_TOUCH_EVENT_UP, s);
  EXPECT_TRUE(has(up, EV_KEY, BTN_TOUCH, 0));
  EXPECT_TRUE(has(up, EV_ABS, ABS_PRESSURE, 0));
  EXPECT_FALSE(has_code(up, EV_KEY, BTN_TOOL_PEN));
  EXPECT_EQ(pen.tool(), BTN_TOOL_PEN);

  // Leaving: proximity out (inputtino never did this, so the pen stayed in proximity forever).
  auto leave = pen.handle(LI_TOUCH_EVENT_HOVER_LEAVE, s);
  EXPECT_TRUE(has(leave, EV_KEY, BTN_TOOL_PEN, 0));
  EXPECT_TRUE(ends_with_syn(leave));
  EXPECT_EQ(pen.tool(), 0);
  EXPECT_TRUE(pen.handle(LI_TOUCH_EVENT_HOVER_LEAVE, s).empty());
}

// Bugs caught: 0, False positives: 0
TEST(TouchPenTest, PenCancelAllLiftsAndLeaves) {
  pen_state_t pen;
  pen_sample_t s;
  s.tool = BTN_TOOL_PEN;
  s.buttons = LI_PEN_BUTTON_PRIMARY;
  pen.handle(LI_TOUCH_EVENT_DOWN, s);
  auto events = pen.handle(LI_TOUCH_EVENT_CANCEL_ALL, {});
  EXPECT_TRUE(has(events, EV_KEY, BTN_TOUCH, 0));
  EXPECT_TRUE(has(events, EV_KEY, BTN_STYLUS, 0));
  EXPECT_TRUE(has(events, EV_KEY, BTN_TOOL_PEN, 0));
  EXPECT_FALSE(pen.touching());
  EXPECT_EQ(pen.tool(), 0);
}

// Bugs caught: 0, False positives: 0
TEST(TouchPenTest, PenToolSwitchLeavesProximityFirst) {
  pen_state_t pen;
  pen_sample_t s;
  s.tool = BTN_TOOL_PEN;
  pen.handle(LI_TOUCH_EVENT_HOVER, s);
  s.tool = BTN_TOOL_RUBBER;
  auto events = pen.handle(LI_TOUCH_EVENT_HOVER, s);
  // Pen out in one frame, eraser in in the next.
  auto pen_out = std::ranges::find(events, event_t {EV_KEY, BTN_TOOL_PEN, 0});
  auto first_syn = std::ranges::find(events, event_t {EV_SYN, SYN_REPORT, 0});
  auto rubber_in = std::ranges::find(events, event_t {EV_KEY, BTN_TOOL_RUBBER, 1});
  ASSERT_NE(pen_out, events.end());
  ASSERT_NE(rubber_in, events.end());
  EXPECT_LT(pen_out, first_syn);
  EXPECT_GT(rubber_in, first_syn);
  EXPECT_EQ(pen.tool(), BTN_TOOL_RUBBER);

  // Unknown tool type keeps the current tool.
  s.tool = 0;
  EXPECT_FALSE(has_code(pen.handle(LI_TOUCH_EVENT_HOVER, s), EV_KEY, BTN_TOOL_RUBBER));
  EXPECT_EQ(pen.tool(), BTN_TOOL_RUBBER);
}

// Bugs caught: 0, False positives: 0
TEST(TouchPenTest, PenButtons) {
  pen_state_t pen;
  pen_sample_t s;
  s.tool = BTN_TOOL_PEN;
  pen.handle(LI_TOUCH_EVENT_HOVER, s);
  s.buttons = LI_PEN_BUTTON_PRIMARY | LI_PEN_BUTTON_TERTIARY;
  EXPECT_EQ(pen.handle(LI_TOUCH_EVENT_BUTTON_ONLY, s), (events_t {
                                                         {EV_KEY, BTN_STYLUS, 1},
                                                         {EV_KEY, BTN_STYLUS3, 1},
                                                         {EV_SYN, SYN_REPORT, 0},
                                                       }));
  EXPECT_TRUE(pen.handle(LI_TOUCH_EVENT_BUTTON_ONLY, s).empty());  // No change
  s.buttons = LI_PEN_BUTTON_SECONDARY;
  auto events = pen.handle(LI_TOUCH_EVENT_BUTTON_ONLY, s);
  EXPECT_TRUE(has(events, EV_KEY, BTN_STYLUS, 0));
  EXPECT_TRUE(has(events, EV_KEY, BTN_STYLUS2, 1));
  EXPECT_TRUE(has(events, EV_KEY, BTN_STYLUS3, 0));
}

// Bugs caught: 0, False positives: 0
TEST(TouchPenTest, PenTiltAndRotation) {
  EXPECT_EQ(pen_tilt(LI_TILT_UNKNOWN, 0), (std::pair {0, 0}));
  EXPECT_EQ(pen_tilt(45, LI_ROT_UNKNOWN), (std::pair {0, 0}));
  EXPECT_EQ(pen_tilt(0, 123), (std::pair {0, 0}));
  EXPECT_EQ(pen_tilt(45, 0), (std::pair {0, 45}));
  EXPECT_EQ(pen_tilt(45, 90), (std::pair {-45, 0}));
  EXPECT_EQ(pen_tilt(45, 180), (std::pair {0, -45}));

  // Rotation goes to ABS_Z such that libinput, which reports (value / 360 * 360 + 90) mod 360, gives it back.
  pen_state_t pen;
  for (int rotation : {0, 1, 90, 180, 269, 270, 359}) {
    pen_sample_t s;
    s.tool = BTN_TOOL_PEN;
    s.rotation = rotation;
    auto v = last(pen.handle(LI_TOUCH_EVENT_HOVER, s), EV_ABS, ABS_Z);
    ASSERT_TRUE(v) << rotation;
    EXPECT_GE(*v, 0);
    EXPECT_LE(*v, pen_axes::rotation_max);
    EXPECT_DOUBLE_EQ(std::fmod(double(*v) / (pen_axes::rotation_max + 1) * 360.0 + 90.0, 360.0), double(rotation));
  }
  // Unknown rotation: ABS_Z is left alone.
  pen_sample_t s;
  s.tool = BTN_TOOL_PEN;
  EXPECT_FALSE(has_code(pen.handle(LI_TOUCH_EVENT_HOVER, s), EV_ABS, ABS_Z));
}

// Bugs caught: 0, False positives: 0
TEST(TouchPenTest, PenTouchCapabilityNeedsUinputAndTheOption) {
  using namespace platf;
  EXPECT_EQ(input_options::capabilities(true, true, "xone"), platform_caps::pen_touch);
  EXPECT_EQ(input_options::capabilities(false, true, "xone"), 0u);  // No uinput: clients fall back to the mouse
  EXPECT_EQ(input_options::capabilities(true, false, "xone"), 0u);  // native_pen_touch off
  EXPECT_EQ(input_options::capabilities(false, false, "ds5"), platform_caps::controller_touch);
  EXPECT_EQ(input_options::capabilities(true, true, "auto"), platform_caps::pen_touch | platform_caps::controller_touch);
  EXPECT_EQ(input_options::capabilities(true, true, "switch"), platform_caps::pen_touch);
}

#endif
