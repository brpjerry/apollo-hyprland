/**
 * @file tests/unit/platform/test_abs_input.cpp
 * @brief Test the Hyprland absolute input helpers in src/platform/linux/input/abs_input.* and the Linux mouse entry
 * points that use them (src/platform/linux/input/virtualhid.cpp).
 *
 * Layouts are from the development machine: DP-3 3840x2160 at scale 1.5 (logical 2560x1440) at 0,0 and HDMI-A-1
 * 3840x2160 at scale 1.5 with transform 1 (logical 1440x2560) at 2560,-560, plus a 1920x1080 headless output that
 * Hyprland places at 4000,0.
 */
#include "../../tests_common.h"

#ifdef __linux__

  // standard includes
  #include <cmath>

  // local includes
  #include <src/input.h>
  #include <src/platform/linux/input/abs_input.h>
  #include <src/platform/virtualhid_input.h>
  #include <src/video.h>

namespace video {
  input::touch_port_t make_port(platf::display_t *display, const config_t &config);
}

namespace {
  using namespace platf::abs_input;

  struct layout_t {
    int x, y, width, height;
  };

  // DP-3 + HDMI-A-1: 4000x2560 at 0,-560. With the headless output: 5920x2560 at 0,-560.
  constexpr layout_t desk_headless {0, -560, 5920, 2560};

  // The port input.cpp hands to platf::abs_mouse(): the output at x,y (logical w x h) in a layout box.
  platf::touch_port_t port(int x, int y, int w, int h, const layout_t &layout, std::string name = {}) {
    platf::touch_port_t port {x, y, layout.width, layout.height, w, h};
    port.env_offset_x = layout.x;
    port.env_offset_y = layout.y;
    port.output_name = std::move(name);
    return port;
  }

  const auto dp3 = port(0, 0, 2560, 1440, desk_headless);
  const auto hdmi = port(2560, -560, 1440, 2560, desk_headless);
  const auto headless = port(4000, 0, 1920, 1080, desk_headless);

  // layout_target() for a display-local point.
  target_t local_target(const platf::touch_port_t &port, double local_x, double local_y) {
    return layout_target(port, port.offset_x + local_x, port.offset_y + local_y);
  }

  // Where libinput puts a device value: units / (max + 1) of the box.
  double landed(int units, int axis_max, double origin, double size) {
    return origin + double(units) / (axis_max + 1) * size;
  }

  // Where an axis of a target lands in a box of `size` at `origin`.
  double landed_x(const target_t &t, int axis_max, double origin, double size) {
    return landed(libinput_units(t.at.x, t.min.x, t.max.x, axis_max), axis_max, origin, size);
  }

  double landed_y(const target_t &t, int axis_max, double origin, double size) {
    return landed(libinput_units(t.at.y, t.min.y, t.max.y, axis_max), axis_max, origin, size);
  }

  struct fake_compositor_t: compositor_t {
    std::vector<output_rect_t> monitors;
    bool reachable = true;
    bool bind_ok = true;
    std::uint64_t gen = 1;
    std::vector<std::pair<std::string, std::string>> binds;
    std::vector<std::string> flat;
    int refreshes = 0;
    int reads = 0;

    outputs_t outputs(bool refresh) override {
      ++reads;
      refreshes += refresh;
      if (!reachable) {
        return nullptr;
      }
      return std::make_shared<std::vector<output_rect_t>>(monitors);
    }

    bool bind_to_output(const std::string &device, const std::string &output) override {
      binds.emplace_back(device, output);
      return bind_ok;
    }

    bool set_flat_accel(const std::string &device) override {
      flat.emplace_back(device);
      return bind_ok;
    }

    std::uint64_t generation() const override {
      return gen;
    }
  };

  std::shared_ptr<fake_compositor_t> fake_hyprland() {
    auto fake = std::make_shared<fake_compositor_t>();
    fake->monitors = {
      {"DP-3", 0, 0, 2560, 1440},
      {"HDMI-A-1", 2560, -560, 1440, 2560},
      {"APOLLO-TEST-1", 4000, 0, 1920, 1080},
    };
    return fake;
  }

  struct fake_display_t: platf::display_t {
    platf::capture_e capture(const push_captured_image_cb_t &, const pull_free_image_cb_t &, bool *) override {
      return platf::capture_e::capture_error;
    }

    std::shared_ptr<platf::img_t> alloc_img() override {
      return nullptr;
    }

    int dummy_img(platf::img_t *) override {
      return 0;
    }
  };

  /**
   * @brief Linux platform input on libvirtualhid's fake backend, with a fake Hyprland (or none) behind it.
   */
  class LinuxMouseTest: public ::testing::Test {
  protected:
    void start(std::shared_ptr<compositor_t> compositor) {
      set_compositor_for_testing(compositor);
      input = platf::input();
      ASSERT_TRUE(input);
      platf::virtualhid::get_input_context(input) = platf::virtualhid::input_context_t {lvh::BackendKind::fake};
      ASSERT_NE(context().mouse, nullptr);
    }

    void TearDown() override {
      input.reset();
      set_compositor_for_testing(nullptr);
    }

    platf::virtualhid::input_context_t &context() {
      return platf::virtualhid::get_input_context(input);
    }

    platf::input_t input;
  };
}  // namespace

// Bugs caught: 0, False positives: 0
TEST(AbsInputTest, LayoutTargetAddsOutputOffsetAndNegativeOrigin) {
  // DP-3's top-left is 560 px below the top of the layout, because HDMI-A-1 reaches up to y = -560.
  auto t = local_target(dp3, 0, 0);
  EXPECT_DOUBLE_EQ(t.at.x, 0.0);
  EXPECT_DOUBLE_EQ(t.at.y, 560.0 / 2560.0);
  EXPECT_DOUBLE_EQ(t.min.y, 560.0 / 2560.0);
  EXPECT_DOUBLE_EQ(t.max.x, 2560.0 / 5920.0);
  EXPECT_DOUBLE_EQ(t.max.y, 2000.0 / 2560.0);

  t = local_target(dp3, 1280, 720);
  EXPECT_DOUBLE_EQ(t.at.x, 1280.0 / 5920.0);
  EXPECT_DOUBLE_EQ(t.at.y, (720.0 + 560.0) / 2560.0);

  // HDMI-A-1 starts at the top of the layout.
  t = local_target(hdmi, 0, 0);
  EXPECT_DOUBLE_EQ(t.at.x, 2560.0 / 5920.0);
  EXPECT_DOUBLE_EQ(t.at.y, 0.0);
  t = local_target(hdmi, 1440, 2560);
  EXPECT_DOUBLE_EQ(t.at.x, 4000.0 / 5920.0);
  EXPECT_DOUBLE_EQ(t.at.y, 1.0);

  // The headless output is the right end of the layout.
  t = local_target(headless, 960, 540);
  EXPECT_DOUBLE_EQ(t.at.x, 4960.0 / 5920.0);
  EXPECT_DOUBLE_EQ(t.at.y, 1100.0 / 2560.0);
  t = local_target(headless, 1920, 1080);
  EXPECT_DOUBLE_EQ(t.at.x, 1.0);
}

// Bugs caught: 0, False positives: 0
TEST(AbsInputTest, LayoutTargetWithOutputLeftOfAndAboveOrigin) {
  // 1920x1080 at -1920,-200 next to a 2560x1440 at 0,0: layout 4480x1640 at -1920,-200.
  const layout_t layout {-1920, -200, 4480, 1640};
  const auto left = port(-1920, -200, 1920, 1080, layout);
  const auto main = port(0, 0, 2560, 1440, layout);

  auto t = local_target(left, 0, 0);
  EXPECT_DOUBLE_EQ(t.at.x, 0.0);
  EXPECT_DOUBLE_EQ(t.at.y, 0.0);
  t = local_target(left, 1920, 1080);
  EXPECT_DOUBLE_EQ(t.at.x, 1920.0 / 4480.0);
  EXPECT_DOUBLE_EQ(t.at.y, 1080.0 / 1640.0);
  t = local_target(main, 0, 0);
  EXPECT_DOUBLE_EQ(t.at.x, 1920.0 / 4480.0);
  EXPECT_DOUBLE_EQ(t.at.y, 200.0 / 1640.0);
}

// Bugs caught: 0, False positives: 0
TEST(AbsInputTest, TargetsAreClampedAndSafe) {
  auto t = local_target(dp3, -50, 99999);
  EXPECT_DOUBLE_EQ(t.at.x, 0.0);
  EXPECT_DOUBLE_EQ(t.at.y, 1.0);

  platf::touch_port_t empty {0, 0, 0, 0};
  t = layout_target(empty, 10, 10);
  EXPECT_DOUBLE_EQ(t.at.x, 0.0);
  EXPECT_DOUBLE_EQ(t.at.y, 0.0);

  // Without the output size the whole layout is allowed.
  platf::touch_port_t sizeless {4000, 0, 5920, 2560};
  t = layout_target(sizeless, 4000, 0);
  EXPECT_DOUBLE_EQ(t.min.x, 0.0);
  EXPECT_DOUBLE_EQ(t.max.x, 1.0);
}

// Bugs caught: 1, False positives: 0
TEST(AbsInputTest, LibinputUnitsLandOnTheTargetPixel) {
  // libinput divides by (max + 1): a naive round(fraction * max) lands up to a pixel short on wide layouts.
  // Rounding up puts an integer target inside its pixel.
  for (int axis_max : {mouse_axis_max, touch_axis_max_x, touch_axis_max_y}) {
    for (double size : {1080.0, 1440.0, 1920.0, 2560.0, 5920.0, 10480.0}) {
      const double unit = size / (axis_max + 1);
      for (double pixel : {0.0, 1.0, 560.0, size / 4, size / 2, size * 0.75, size - 1}) {
        auto at = landed(libinput_units(pixel / size, axis_max), axis_max, 0, size);
        EXPECT_GE(at, pixel) << "max " << axis_max << " size " << size << " pixel " << pixel;
        EXPECT_LT(at, pixel + unit) << "max " << axis_max << " size " << size << " pixel " << pixel;

        // With float noise like the mouse path's (float layout coordinates): off by at most a unit.
        at = landed(libinput_units(double(float(pixel / size)), axis_max), axis_max, 0, size);
        EXPECT_GT(at, pixel - unit / 100) << "max " << axis_max << " size " << size << " pixel " << pixel;
        EXPECT_LT(at, pixel + unit * 1.01) << "max " << axis_max << " size " << size << " pixel " << pixel;
      }
    }
  }

  EXPECT_EQ(libinput_units(0.0, 19200), 0);
  EXPECT_EQ(libinput_units(1.0, 19200), 19200);
  EXPECT_EQ(libinput_units(2.0, 19200), 19200);
  EXPECT_EQ(libinput_units(-1.0, 19200), 0);
  EXPECT_EQ(libinput_units(std::nan(""), 19200), 0);
  EXPECT_EQ(libinput_units(0.25, 19200), 4801);  // 4800.25 rounds up
  EXPECT_FLOAT_EQ(libinput_fraction(1.0, 19200), 1.0f);
  EXPECT_FLOAT_EQ(libinput_fraction(0.25, 19200), 4801.0f / 19200);
}

// Bugs caught: 0, False positives: 0
TEST(AbsInputTest, LibinputUnitsStayOnTheOutput) {
  // 40000 px wide layout: one mouse unit is 0.61 px. The output starts at 2560 (4194.3 units): unit 4194 would be on
  // the neighbouring output.
  const double n = mouse_axis_max + 1;
  const double size = 40000;
  const double start = 2560 / size;
  const double end = 4000 / size;
  int u = libinput_units(start, start, end, mouse_axis_max);
  EXPECT_GE(landed(u, mouse_axis_max, 0, size), 2560);
  EXPECT_LT(landed(u, mouse_axis_max, 0, size), 2560 + size / n);
  // A point slightly left of the output (float noise, or a clamped client coordinate) is pulled onto it.
  u = libinput_units(start - 0.4 / size, start, end, mouse_axis_max);
  EXPECT_GE(landed(u, mouse_axis_max, 0, size), 2560);

  // The far edge belongs to the next output: stay before it.
  u = libinput_units(end, start, end, mouse_axis_max);
  EXPECT_LT(landed(u, mouse_axis_max, 0, size), 4000);
  EXPECT_GE(landed(u, mouse_axis_max, 0, size), 4000 - size / n);

  // Degenerate output narrower than a unit: still a valid value.
  u = libinput_units(0.5, 0.5, 0.5, mouse_axis_max);
  EXPECT_GE(u, 0);
  EXPECT_LE(u, mouse_axis_max);
}

// Bugs caught: 0, False positives: 3
TEST(AbsInputTest, ClientPointLandsOnStreamedPixel) {
  // End to end through video::make_port() and the client_to_touchport() / platform_touch_port() /
  // monitor_touch_port() maths of input.cpp, for each output of the layout.
  struct case_t {
    const char *name;
    int frame_w, frame_h, logical_w, logical_h, x, y;
    int stream_w, stream_h;
  };

  const case_t cases[] {
    {"DP-3", 3840, 2160, 2560, 1440, 0, 0, 1920, 1080},
    // Hyprland delivers rotated outputs upright, so the frame is portrait like the logical size.
    {"HDMI-A-1", 2160, 3840, 1440, 2560, 2560, -560, 1080, 1920},
    {"APOLLO-TEST-1", 1920, 1080, 1920, 1080, 4000, 0, 1920, 1080},
    {"APOLLO-TEST-1 at scale 1.5", 1920, 1080, 1280, 720, 4000, 0, 1920, 1080},
  };

  for (const auto &c : cases) {
    fake_display_t display;
    display.width = c.frame_w;
    display.height = c.frame_h;
    display.logical_width = c.logical_w;
    display.logical_height = c.logical_h;
    display.offset_x = c.x;
    display.offset_y = c.y;
    display.env_offset_x = desk_headless.x;
    display.env_offset_y = desk_headless.y;
    display.env_width = desk_headless.width;
    display.env_height = desk_headless.height;
    display.env_logical_width = desk_headless.width;
    display.env_logical_height = desk_headless.height;
    display.output_name = c.name;

    video::config_t config {};
    config.width = c.stream_w;
    config.height = c.stream_h;
    const auto tp = video::make_port(&display, config);
    // input.cpp's platform_touch_port(): the port platf::abs_mouse() gets.
    const auto abs = port(tp.offset_x, tp.offset_y, tp.logical_width, tp.logical_height, {tp.env_offset_x, tp.env_offset_y, tp.env_logical_width, tp.env_logical_height});
    EXPECT_EQ(abs.width, desk_headless.width) << c.name;
    EXPECT_EQ(abs.logical_width, c.logical_w) << c.name;
    EXPECT_EQ(abs.logical_height, c.logical_h) << c.name;

    // The last point is the bottom-right stream pixel.
    const double last_x = (c.stream_w - 1.0) / c.stream_w;
    const double last_y = (c.stream_h - 1.0) / c.stream_h;
    for (auto [sx, sy] : {std::pair {0.0, 0.0}, {0.25, 0.75}, {0.5, 0.5}, {last_x, last_y}}) {
      // What input::client_to_touchport() returns for a client point at (sx, sy) of the stream, in float as there.
      const float x = (float(sx * c.stream_w) - tp.client_offsetX) * tp.scalar_inv / tp.scalar_tpcoords + tp.offset_x;
      const float y = (float(sy * c.stream_h) - tp.client_offsetY) * tp.scalar_inv / tp.scalar_tpcoords + tp.offset_y;
      const double want_x = c.x + sx * c.logical_w;
      const double want_y = c.y + sy * c.logical_h;

      // Absolute mouse: fraction of the layout box, less than a unit past the target (0.09 px here). The layout
      // coordinates carry float noise, hence the hundredth of a pixel.
      auto t = layout_target(abs, x, y);
      const double unit_x = abs.width / (mouse_axis_max + 1.0);
      const double unit_y = abs.height / (mouse_axis_max + 1.0);
      EXPECT_GE(landed_x(t, mouse_axis_max, desk_headless.x, abs.width), want_x - 0.01) << c.name;
      EXPECT_LT(landed_x(t, mouse_axis_max, desk_headless.x, abs.width), want_x + unit_x + 0.01) << c.name;
      EXPECT_GE(landed_y(t, mouse_axis_max, desk_headless.y, abs.height), want_y - 0.01) << c.name;
      EXPECT_LT(landed_y(t, mouse_axis_max, desk_headless.y, abs.height), want_y + unit_y + 0.01) << c.name;

      // Touch and pen bound to the output: input.cpp's monitor_touch_port() normalizes to the output, the input
      // entry points place that in libinput units, and libvirtualhid rounds it to the nearest of 19200 x 10800.
      const double fx = (x - tp.offset_x) / double(c.logical_w);
      const double fy = (y - tp.offset_y) / double(c.logical_h);
      const auto device_x = [](float f, int axis_max) {
        return int(std::lround(std::clamp(f, 0.0f, 1.0f) * float(axis_max)));
      };
      const auto touch_x = landed(device_x(libinput_fraction(fx, touch_axis_max_x), touch_axis_max_x), touch_axis_max_x, c.x, c.logical_w);
      const auto touch_y = landed(device_x(libinput_fraction(fy, touch_axis_max_y), touch_axis_max_y), touch_axis_max_y, c.y, c.logical_h);
      EXPECT_GE(touch_x, want_x - 0.01) << c.name;
      EXPECT_LT(touch_x, want_x + c.logical_w / (touch_axis_max_x + 1.0) + 0.01) << c.name;
      EXPECT_GE(touch_y, want_y - 0.01) << c.name;
      EXPECT_LT(touch_y, want_y + c.logical_h / (touch_axis_max_y + 1.0) + 0.01) << c.name;
      // Unbound, Sunshine's coordinates go to the device as they are: up to one and a half units short.
      const auto plain_y = landed(device_x(float(fy), touch_axis_max_y), touch_axis_max_y, c.y, c.logical_h);
      EXPECT_NEAR(plain_y, want_y, 1.5 * c.logical_h / (touch_axis_max_y + 1.0) + 0.01) << c.name;
    }
  }
}

// Bugs caught: 0, False positives: 0
TEST(AbsInputTest, CompositorDeviceNames) {
  EXPECT_EQ(compositor_device_name("libvirtualhid Mouse (Absolute)"), "libvirtualhid-mouse-(absolute)");
  EXPECT_EQ(compositor_device_name("libvirtualhid Touchscreen (client 3)"), "libvirtualhid-touchscreen-(client-3)");

  const std::string touch = "libvirtualhid-touchscreen-(client-3)";
  EXPECT_TRUE(is_instance_of(touch, touch));
  EXPECT_TRUE(is_instance_of(touch + "-1", touch));
  EXPECT_TRUE(is_instance_of(touch + "-12", touch));
  EXPECT_FALSE(is_instance_of("libvirtualhid-touchscreen-(client-31)", touch));
  EXPECT_FALSE(is_instance_of(touch + "-", touch));
  EXPECT_FALSE(is_instance_of(touch + "-1a", touch));
  // Flat acceleration for the relative mouse must not reach its absolute twin.
  EXPECT_FALSE(is_instance_of("libvirtualhid-mouse-(absolute)", "libvirtualhid-mouse"));
  EXPECT_TRUE(is_instance_of("libvirtualhid-mouse-2", "libvirtualhid-mouse"));
  EXPECT_FALSE(is_instance_of("anything", ""));
}

// Bugs caught: 0, False positives: 0
TEST(AbsInputTest, LogicalRectOfOutputs) {
  auto r = logical_rect("DP-3", 0, 0, 3840, 2160, 1.5, 0);
  EXPECT_EQ(r.width, 2560);
  EXPECT_EQ(r.height, 1440);
  r = logical_rect("HDMI-A-1", 2560, -560, 3840, 2160, 1.5, 1);
  EXPECT_EQ(r.x, 2560);
  EXPECT_EQ(r.y, -560);
  EXPECT_EQ(r.width, 1440);
  EXPECT_EQ(r.height, 2560);
  r = logical_rect("X", 0, 0, 1920, 1080, 0.0, 6);
  EXPECT_EQ(r.width, 1920);
  EXPECT_EQ(r.height, 1080);
}

// Bugs caught: 0, False positives: 0
TEST(AbsInputTest, FindsOutputAndBoundingBox) {
  auto fake = fake_hyprland();
  EXPECT_EQ(find_output(fake->monitors, dp3)->name, "DP-3");
  EXPECT_EQ(find_output(fake->monitors, hdmi)->name, "HDMI-A-1");
  EXPECT_EQ(find_output(fake->monitors, headless)->name, "APOLLO-TEST-1");
  EXPECT_FALSE(find_output(fake->monitors, port(100, 100, 1920, 1080, desk_headless)));
  EXPECT_EQ(find_output(fake->monitors, "HDMI-A-1")->x, 2560);
  EXPECT_FALSE(find_output(fake->monitors, "nope"));

  // The port's output name wins over its position; an unknown name falls back to the position.
  EXPECT_EQ(find_output(fake->monitors, port(0, 0, 2560, 1440, desk_headless, "HDMI-A-1"))->name, "HDMI-A-1");
  EXPECT_EQ(find_output(fake->monitors, port(0, 0, 2560, 1440, desk_headless, "GONE"))->name, "DP-3");

  // A mirror at the same position: the size decides.
  std::vector<output_rect_t> mirrored {{"MIRROR", 0, 0, 1920, 1080}, {"DP-3", 0, 0, 2560, 1440}};
  EXPECT_EQ(find_output(mirrored, dp3)->name, "DP-3");

  auto box = bounding_box(fake->monitors);
  ASSERT_TRUE(box);
  EXPECT_EQ(box->x, 0);
  EXPECT_EQ(box->y, -560);
  EXPECT_EQ(box->width, 5920);
  EXPECT_EQ(box->height, 2560);
  EXPECT_FALSE(bounding_box({}));
}

// Bugs caught: 0, False positives: 0
TEST(AbsInputTest, LayoutMapperFollowsLiveLayout) {
  auto fake = fake_hyprland();
  layout_mapper_t mapper {fake};

  // Matches the port's own layout while nothing changed.
  auto t = mapper.map(headless, 4960, 540);
  ASSERT_TRUE(t);
  EXPECT_EQ(fake->refreshes, 1);
  EXPECT_DOUBLE_EQ(t->at.x, 4960.0 / 5920.0);
  EXPECT_DOUBLE_EQ(t->at.y, 1100.0 / 2560.0);

  // Another client's virtual display appears at 5920,0 and the layout grows, but this stream's port is not rebuilt.
  fake->monitors.push_back({"APOLLO-OTHER", 5920, 0, 1920, 1080});
  t = mapper.map(headless, 4960, 540);
  ASSERT_TRUE(t);
  EXPECT_EQ(fake->refreshes, 1);  // a cached read: no forced refresh for an unchanged port
  EXPECT_DOUBLE_EQ(t->at.x, 4960.0 / 7840.0);
  EXPECT_DOUBLE_EQ(t->min.x, 4000.0 / 7840.0);
  EXPECT_DOUBLE_EQ(t->max.x, 5920.0 / 7840.0);

  // The streamed output is rescaled (1920x1080 at scale 1.5): the old port's coordinates are rescaled with it.
  fake->monitors[2] = {"APOLLO-TEST-1", 4000, 0, 1280, 720};
  t = mapper.map(headless, 4960, 540);
  ASSERT_TRUE(t);
  EXPECT_DOUBLE_EQ(t->at.x, (4000.0 + 640.0) / 7840.0);
  EXPECT_DOUBLE_EQ(t->at.y, (560.0 + 360.0) / 2560.0);

  // The output disappears: fall back to the port.
  fake->monitors.pop_back();
  fake->monitors.pop_back();
  t = mapper.map(headless, 4960, 540);
  ASSERT_TRUE(t);
  EXPECT_DOUBLE_EQ(t->at.x, 4960.0 / 5920.0);

  // A port that names its output is followed by that name, wherever the output is.
  auto named = port(4000, 0, 1920, 1080, desk_headless, "HDMI-A-1");
  t = mapper.map(named, 4000, 0);
  ASSERT_TRUE(t);
  EXPECT_DOUBLE_EQ(t->min.x, 2560.0 / 4000.0);

  // Unreachable compositor or none at all: Sunshine's own mapping.
  fake->reachable = false;
  layout_mapper_t offline {fake};
  EXPECT_FALSE(offline.map(dp3, 0, 0));
  layout_mapper_t none {nullptr};
  EXPECT_FALSE(none.map(dp3, 0, 0));
}

// Bugs caught: 0, False positives: 0
TEST(AbsInputTest, OutputBinderBindsToStreamedOutput) {
  auto fake = fake_hyprland();
  output_binder_t binder {"libvirtualhid Touchscreen (client 1)", fake};

  // input.cpp's touch port: monitor-local, named after the output.
  auto touch_port = [](std::string name, int x, int y, int w, int h) {
    platf::touch_port_t port {x, y, w, h, w, h};
    port.output_name = std::move(name);
    return port;
  };
  const auto test1 = touch_port("APOLLO-TEST-1", 4000, 0, 1920, 1080);
  EXPECT_TRUE(binder.update(test1));
  EXPECT_EQ(binder.bound_output(), "APOLLO-TEST-1");
  ASSERT_EQ(fake->binds.size(), 1u);
  EXPECT_EQ(fake->binds[0], (std::pair<std::string, std::string> {"libvirtualhid Touchscreen (client 1)", "APOLLO-TEST-1"}));

  // Same port: no more compositor traffic.
  EXPECT_TRUE(binder.update(test1));
  EXPECT_TRUE(binder.update(test1));
  EXPECT_EQ(fake->binds.size(), 1u);
  EXPECT_EQ(fake->reads, 1);

  // The stream moves to another output: rebind.
  EXPECT_TRUE(binder.update(touch_port("HDMI-A-1", 2560, -560, 1440, 2560)));
  EXPECT_EQ(binder.bound_output(), "HDMI-A-1");
  EXPECT_EQ(fake->binds.size(), 2u);

  // Settings may have been lost (config reload): bind again.
  fake->gen++;
  binder.update(touch_port("HDMI-A-1", 2560, -560, 1440, 2560));
  EXPECT_EQ(fake->binds.size(), 3u);
  EXPECT_EQ(fake->binds.back().second, "HDMI-A-1");

  // A port without a name (another capture backend) is found by position.
  output_binder_t unnamed {"libvirtualhid Pen Tablet (client 1)", fake};
  EXPECT_TRUE(unnamed.update(touch_port({}, 0, 0, 2560, 1440)));
  EXPECT_EQ(unnamed.bound_output(), "DP-3");

  // The client goes away: the binding is removed.
  unnamed.unbind();
  EXPECT_TRUE(unnamed.bound_output().empty());
  EXPECT_EQ(fake->binds.back(), (std::pair<std::string, std::string> {"libvirtualhid Pen Tablet (client 1)", ""}));
  const auto binds = fake->binds.size();
  unnamed.unbind();
  EXPECT_EQ(fake->binds.size(), binds);
}

// Bugs caught: 0, False positives: 0
TEST(AbsInputTest, OutputBinderFailures) {
  platf::touch_port_t test1 {4000, 0, 1920, 1080, 1920, 1080};
  test1.output_name = "APOLLO-TEST-1";

  // No compositor (X11, or no Wayland support compiled in), or no device.
  output_binder_t none {"libvirtualhid Pen Tablet (client 1)", nullptr};
  EXPECT_FALSE(none.update(test1));
  output_binder_t nameless {"", fake_hyprland()};
  EXPECT_FALSE(nameless.update(test1));

  // A compositor that cannot bind: not bound, and no retry before retry_interval.
  auto fake = fake_hyprland();
  fake->bind_ok = false;
  output_binder_t failing {"libvirtualhid Pen Tablet (client 2)", fake};
  EXPECT_FALSE(failing.update(test1));
  EXPECT_TRUE(failing.bound_output().empty());
  EXPECT_FALSE(failing.update(test1));
  EXPECT_EQ(fake->binds.size(), 1u);

  // No such output (e.g. a stale port): not bound, and an earlier binding is removed.
  fake->bind_ok = true;
  output_binder_t binder {"libvirtualhid Pen Tablet (client 3)", fake};
  EXPECT_TRUE(binder.update(test1));
  fake->binds.clear();
  platf::touch_port_t stale {9000, 0, 1920, 1080, 1920, 1080};
  stale.output_name = "APOLLO-GONE";
  EXPECT_FALSE(binder.update(stale));
  EXPECT_TRUE(binder.bound_output().empty());
  ASSERT_EQ(fake->binds.size(), 1u);
  EXPECT_EQ(fake->binds[0].second, "");
}

// Bugs caught: 0, False positives: 0
TEST(AbsInputTest, FlatAccelOncePerGeneration) {
  auto fake = fake_hyprland();
  flat_accel_t accel {fake};
  accel.ensure("libvirtualhid Mouse");
  accel.ensure("libvirtualhid Mouse");
  ASSERT_EQ(fake->flat.size(), 1u);
  EXPECT_EQ(fake->flat[0], "libvirtualhid Mouse");

  fake->gen++;
  accel.ensure("libvirtualhid Mouse");
  accel.ensure("libvirtualhid Mouse");
  EXPECT_EQ(fake->flat.size(), 2u);

  // A new device name is set up at once.
  accel.ensure("libvirtualhid Mouse (seat1)");
  EXPECT_EQ(fake->flat.size(), 3u);
  EXPECT_EQ(fake->flat.back(), "libvirtualhid Mouse (seat1)");

  // Failure: retried only after retry_interval.
  fake->bind_ok = false;
  fake->gen++;
  accel.ensure("libvirtualhid Mouse (seat1)");
  accel.ensure("libvirtualhid Mouse (seat1)");
  EXPECT_EQ(fake->flat.size(), 4u);

  // No device or no compositor: nothing to do.
  accel.ensure("");
  EXPECT_EQ(fake->flat.size(), 4u);
  flat_accel_t none {nullptr};
  none.ensure("libvirtualhid Mouse");
}

// Bugs caught: 0, False positives: 0
TEST_F(LinuxMouseTest, AbsoluteMouseFollowsTheLiveHyprlandLayout) {
  auto fake = fake_hyprland();
  start(fake);

  // A point on the left edge of the headless output, which starts at 4000 of 5920 px.
  platf::abs_mouse(input, headless, 4000.0F, 540.0F);
  auto event = context().mouse->last_submitted_event();
  EXPECT_EQ(event.kind, lvh::MouseEventKind::absolute_motion);
  EXPECT_FALSE(event.has_fractional_absolute_coordinates);
  EXPECT_EQ(event.width, mouse_axis_max);
  EXPECT_EQ(event.height, mouse_axis_max);
  EXPECT_EQ(event.x, libinput_units(4000.0 / 5920.0, 4000.0 / 5920.0, 1.0, mouse_axis_max));
  EXPECT_GE(landed(event.x, mouse_axis_max, 0, 5920), 4000.0);  // never on HDMI-A-1
  EXPECT_EQ(event.y, libinput_units(1100.0 / 2560.0, mouse_axis_max));
  // The device keeps libvirtualhid's default pointer area, the whole layout.
  EXPECT_EQ(context().mouse_viewport.width, 0);
  EXPECT_EQ(context().mouse_desktop.width, 0);

  // Relative motion does not retarget (and so recreate) the mouse, and is made 1:1.
  const auto mouse_id = context().mouse->device_id();
  platf::move_mouse(input, headless, 5, -3);
  EXPECT_EQ(context().mouse->device_id(), mouse_id);
  EXPECT_EQ(context().mouse->last_submitted_event().kind, lvh::MouseEventKind::relative_motion);
  ASSERT_EQ(fake->flat.size(), 1u);
  EXPECT_EQ(fake->flat[0], context().mouse->profile().name);

  // The layout grows; the port does not change, the mapping does.
  fake->monitors.push_back({"APOLLO-OTHER", 5920, 0, 1920, 1080});
  platf::abs_mouse(input, headless, 4960.0F, 540.0F);
  EXPECT_EQ(context().mouse->last_submitted_event().x, libinput_units(4960.0 / 7840.0, 4000.0 / 7840.0, 5920.0 / 7840.0, mouse_axis_max));
  EXPECT_EQ(context().mouse->device_id(), mouse_id);
}

// Bugs caught: 0, False positives: 0
TEST_F(LinuxMouseTest, MouseUsesSunshineMappingWithoutHyprland) {
  auto fake = fake_hyprland();
  fake->reachable = false;
  start(fake);

  platf::abs_mouse(input, headless, 4960.0F, 540.0F);
  auto event = context().mouse->last_submitted_event();
  EXPECT_TRUE(event.has_fractional_absolute_coordinates);
  EXPECT_FLOAT_EQ(event.absolute_x, 960.0F);
  EXPECT_EQ(event.width, 1920);
  EXPECT_EQ(context().mouse_viewport.offset_x, 4000);
  EXPECT_EQ(context().mouse_desktop.width, 5920);

  // Hyprland comes up: the mouse goes back to the default pointer area once, then stays.
  fake->reachable = true;
  platf::abs_mouse(input, headless, 4960.0F, 540.0F);
  EXPECT_EQ(context().mouse_viewport.width, 0);
  const auto mouse_id = context().mouse->device_id();
  platf::abs_mouse(input, headless, 4000.0F, 0.0F);
  platf::move_mouse(input, headless, 1, 1);
  EXPECT_EQ(context().mouse->device_id(), mouse_id);
}

#endif
