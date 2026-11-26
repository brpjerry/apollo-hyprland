/**
 * @file tests/unit/platform/test_inputtino_abs.cpp
 * @brief Test the absolute input mapping in src/platform/linux/input/inputtino_abs.*.
 *
 * Layouts are from the development machine: DP-3 3840x2160 at scale 1.5 (logical 2560x1440) at 0,0 and HDMI-A-1
 * 3840x2160 at scale 1.5 with transform 1 (logical 1440x2560) at 2560,-560, plus a 1920x1080 headless output that
 * Hyprland places at 4000,0.
 */
#include "../../tests_common.h"

#ifdef __linux__

  #include <src/input.h>
  #include <src/platform/linux/input/inputtino_abs.h>
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

  platf::touch_port_t port(int x, int y, int w, int h, const layout_t &layout) {
    platf::touch_port_t port {x, y, layout.width, layout.height};
    port.layout_x = layout.x;
    port.layout_y = layout.y;
    port.output_width = w;
    port.output_height = h;
    return port;
  }

  const auto dp3 = port(0, 0, 2560, 1440, desk_headless);
  const auto hdmi = port(2560, -560, 1440, 2560, desk_headless);
  const auto headless = port(4000, 0, 1920, 1080, desk_headless);

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
}  // namespace

// Bugs caught: 0, False positives: 0
TEST(AbsInputTest, LayoutTargetAddsOutputOffsetAndNegativeOrigin) {
  // DP-3's top-left is 560 px below the top of the layout, because HDMI-A-1 reaches up to y = -560.
  auto t = layout_target(dp3, 0, 0);
  EXPECT_DOUBLE_EQ(t.at.x, 0.0);
  EXPECT_DOUBLE_EQ(t.at.y, 560.0 / 2560.0);
  EXPECT_DOUBLE_EQ(t.min.y, 560.0 / 2560.0);
  EXPECT_DOUBLE_EQ(t.max.x, 2560.0 / 5920.0);
  EXPECT_DOUBLE_EQ(t.max.y, 2000.0 / 2560.0);

  t = layout_target(dp3, 1280, 720);
  EXPECT_DOUBLE_EQ(t.at.x, 1280.0 / 5920.0);
  EXPECT_DOUBLE_EQ(t.at.y, (720.0 + 560.0) / 2560.0);

  // HDMI-A-1 starts at the top of the layout.
  t = layout_target(hdmi, 0, 0);
  EXPECT_DOUBLE_EQ(t.at.x, 2560.0 / 5920.0);
  EXPECT_DOUBLE_EQ(t.at.y, 0.0);
  t = layout_target(hdmi, 1440, 2560);
  EXPECT_DOUBLE_EQ(t.at.x, 4000.0 / 5920.0);
  EXPECT_DOUBLE_EQ(t.at.y, 1.0);

  // The headless output is the right end of the layout.
  t = layout_target(headless, 960, 540);
  EXPECT_DOUBLE_EQ(t.at.x, 4960.0 / 5920.0);
  EXPECT_DOUBLE_EQ(t.at.y, 1100.0 / 2560.0);
  t = layout_target(headless, 1920, 1080);
  EXPECT_DOUBLE_EQ(t.at.x, 1.0);
}

// Bugs caught: 0, False positives: 0
TEST(AbsInputTest, LayoutTargetWithOutputLeftOfAndAboveOrigin) {
  // 1920x1080 at -1920,-200 next to a 2560x1440 at 0,0: layout 4480x1640 at -1920,-200.
  const layout_t layout {-1920, -200, 4480, 1640};
  const auto left = port(-1920, -200, 1920, 1080, layout);
  const auto main = port(0, 0, 2560, 1440, layout);

  auto t = layout_target(left, 0, 0);
  EXPECT_DOUBLE_EQ(t.at.x, 0.0);
  EXPECT_DOUBLE_EQ(t.at.y, 0.0);
  t = layout_target(left, 1920, 1080);
  EXPECT_DOUBLE_EQ(t.at.x, 1920.0 / 4480.0);
  EXPECT_DOUBLE_EQ(t.at.y, 1080.0 / 1640.0);
  t = layout_target(main, 0, 0);
  EXPECT_DOUBLE_EQ(t.at.x, 1920.0 / 4480.0);
  EXPECT_DOUBLE_EQ(t.at.y, 200.0 / 1640.0);
}

// Bugs caught: 0, False positives: 0
TEST(AbsInputTest, TargetsAreClampedAndSafe) {
  auto t = layout_target(dp3, -50, 99999);
  EXPECT_DOUBLE_EQ(t.at.x, 0.0);
  EXPECT_DOUBLE_EQ(t.at.y, 1.0);

  platf::touch_port_t empty {0, 0, 0, 0};
  t = layout_target(empty, 10, 10);
  EXPECT_DOUBLE_EQ(t.at.x, 0.0);
  EXPECT_DOUBLE_EQ(t.at.y, 0.0);
  EXPECT_FALSE(output_target(empty, 10, 10));

  // Without the output size the whole layout is allowed.
  platf::touch_port_t sizeless {4000, 0, 5920, 2560};
  t = layout_target(sizeless, 0, 0);
  EXPECT_DOUBLE_EQ(t.min.x, 0.0);
  EXPECT_DOUBLE_EQ(t.max.x, 1.0);

  auto o = output_target(headless, 960, 2000);
  ASSERT_TRUE(o);
  EXPECT_DOUBLE_EQ(o->at.x, 0.5);
  EXPECT_DOUBLE_EQ(o->at.y, 1.0);
}

// Bugs caught: 1, False positives: 0
TEST(AbsInputTest, LibinputUnitsLandOnTheTargetPixel) {
  // libinput divides by (max + 1): a naive round(fraction * max) lands up to a pixel short on wide layouts.
  // Rounding up puts an integer target inside its pixel.
  for (int axis_max : {mouse_max_x, mouse_max_y, touch_max_y}) {
    for (double size : {1080.0, 1440.0, 1920.0, 2560.0, 5920.0, 10480.0}) {
      const double unit = size / (axis_max + 1);
      for (double pixel : {0.0, 1.0, 560.0, size / 4, size / 2, size * 0.75, size - 1}) {
        auto at = landed(libinput_units(pixel / size, axis_max), axis_max, 0, size);
        EXPECT_GE(at, pixel) << "max " << axis_max << " size " << size << " pixel " << pixel;
        EXPECT_LT(at, pixel + unit) << "max " << axis_max << " size " << size << " pixel " << pixel;

        // With float noise like the touch path's (local / layout width * layout width): off by at most a unit.
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
  EXPECT_FLOAT_EQ(libinput_fraction(1.0, 0.0, 1.0, 19200), 1.0f);
}

// Bugs caught: 0, False positives: 0
TEST(AbsInputTest, LibinputUnitsStayOnTheOutput) {
  // 10480 px wide layout: one mouse unit is 0.55 px. The output starts at 2560 (4690.3 units): unit 4690 would be on
  // the neighbouring output.
  const double n = mouse_max_x + 1;
  const double size = 10480;
  const double start = 2560 / size;
  const double end = 4000 / size;
  int u = libinput_units(start, start, end, mouse_max_x);
  EXPECT_GE(landed(u, mouse_max_x, 0, size), 2560);
  EXPECT_LT(landed(u, mouse_max_x, 0, size), 2560 + size / n);
  // A point slightly left of the output (float noise, or a clamped client coordinate) is pulled onto it.
  u = libinput_units(start - 0.4 / size, start, end, mouse_max_x);
  EXPECT_GE(landed(u, mouse_max_x, 0, size), 2560);

  // The far edge belongs to the next output: stay before it.
  u = libinput_units(end, start, end, mouse_max_x);
  EXPECT_LT(landed(u, mouse_max_x, 0, size), 4000);
  EXPECT_GE(landed(u, mouse_max_x, 0, size), 4000 - size / n);

  // Degenerate output narrower than a unit: still a valid value.
  u = libinput_units(0.5, 0.5, 0.5, mouse_max_x);
  EXPECT_GE(u, 0);
  EXPECT_LE(u, mouse_max_x);
}

// Bugs caught: 0, False positives: 3
TEST(AbsInputTest, ClientPointLandsOnStreamedPixel) {
  // End to end through video::make_port() and the client_to_touchport() maths, for each output of the layout.
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
    display.env_x = desk_headless.x;
    display.env_y = desk_headless.y;
    display.env_width = desk_headless.width;
    display.env_height = desk_headless.height;

    video::config_t config {};
    config.width = c.stream_w;
    config.height = c.stream_h;
    const auto tp = video::make_port(&display, config);
    const auto abs = input::make_abs_port(tp);
    EXPECT_EQ(abs.layout_x, desk_headless.x) << c.name;
    EXPECT_EQ(abs.layout_y, desk_headless.y) << c.name;
    EXPECT_EQ(abs.output_width, c.logical_w) << c.name;
    EXPECT_EQ(abs.output_height, c.logical_h) << c.name;

    // The last point is the bottom-right stream pixel.
    const double last_x = (c.stream_w - 1.0) / c.stream_w;
    const double last_y = (c.stream_h - 1.0) / c.stream_h;
    for (auto [sx, sy] : {std::pair {0.0, 0.0}, {0.25, 0.75}, {0.5, 0.5}, {last_x, last_y}}) {
      // What input::client_to_touchport() returns for a client point at (sx, sy) of the stream.
      double local_x = (sx * c.stream_w - tp.client_offsetX) * tp.scalar_inv;
      double local_y = (sy * c.stream_h - tp.client_offsetY) * tp.scalar_inv;
      const double want_x = c.x + sx * c.logical_w;
      const double want_y = c.y + sy * c.logical_h;

      // Absolute mouse: fraction of the layout box, less than a unit past the target (0.31 px here). The local
      // coordinates carry float noise, hence the hundredth of a pixel.
      auto t = layout_target(abs, local_x, local_y);
      EXPECT_GE(landed_x(t, mouse_max_x, abs.layout_x, abs.width), want_x - 0.01) << c.name;
      EXPECT_LT(landed_x(t, mouse_max_x, abs.layout_x, abs.width), want_x + 0.31) << c.name;
      EXPECT_GE(landed_y(t, mouse_max_y, abs.layout_y, abs.height), want_y - 0.01) << c.name;
      EXPECT_LT(landed_y(t, mouse_max_y, abs.layout_y, abs.height), want_y + 0.22) << c.name;

      // Touch and pen bound to the output: fraction of the output's logical box. input.cpp divides local
      // coordinates by the layout size and the backend multiplies back.
      auto o = output_target(abs, float(local_x / abs.width) * abs.width, float(local_y / abs.height) * abs.height);
      ASSERT_TRUE(o) << c.name;
      EXPECT_GE(landed_x(*o, touch_max_x, c.x, c.logical_w), want_x - 0.01) << c.name;
      EXPECT_LT(landed_x(*o, touch_max_x, c.x, c.logical_w), want_x + c.logical_w / (touch_max_x + 1.0) + 0.01) << c.name;
      EXPECT_GE(landed_y(*o, touch_max_y, c.y, c.logical_h), want_y - 0.01) << c.name;
      EXPECT_LT(landed_y(*o, touch_max_y, c.y, c.logical_h), want_y + c.logical_h / (touch_max_y + 1.0) + 0.01) << c.name;
    }
  }
}

// Bugs caught: 0, False positives: 0
TEST(AbsInputTest, CompositorDeviceNames) {
  EXPECT_EQ(compositor_device_name("Mouse passthrough (absolute)"), "mouse-passthrough-(absolute)");
  EXPECT_EQ(compositor_device_name("Touch passthrough (client 3)"), "touch-passthrough-(client-3)");

  const std::string touch = "touch-passthrough-(client-3)";
  EXPECT_TRUE(is_instance_of(touch, touch));
  EXPECT_TRUE(is_instance_of(touch + "-1", touch));
  EXPECT_TRUE(is_instance_of(touch + "-12", touch));
  EXPECT_FALSE(is_instance_of("touch-passthrough-(client-31)", touch));
  EXPECT_FALSE(is_instance_of(touch + "-", touch));
  EXPECT_FALSE(is_instance_of(touch + "-1a", touch));
  EXPECT_FALSE(is_instance_of("mouse-passthrough-(absolute)", "mouse-passthrough"));
  EXPECT_TRUE(is_instance_of("mouse-passthrough-2", "mouse-passthrough"));
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
  auto t = mapper.map(headless, 960, 540);
  EXPECT_EQ(fake->refreshes, 1);
  EXPECT_DOUBLE_EQ(t.at.x, 4960.0 / 5920.0);
  EXPECT_DOUBLE_EQ(t.at.y, 1100.0 / 2560.0);

  // Another client's virtual display appears at 5920,0 and the layout grows, but this stream's port is not rebuilt.
  fake->monitors.push_back({"APOLLO-OTHER", 5920, 0, 1920, 1080});
  t = mapper.map(headless, 960, 540);
  EXPECT_EQ(fake->refreshes, 1);  // a cached read: no forced refresh for an unchanged port
  EXPECT_DOUBLE_EQ(t.at.x, 4960.0 / 7840.0);
  EXPECT_DOUBLE_EQ(t.min.x, 4000.0 / 7840.0);
  EXPECT_DOUBLE_EQ(t.max.x, 5920.0 / 7840.0);

  // The streamed output is rescaled (1920x1080 at scale 1.5): the old port's coordinates are rescaled with it.
  fake->monitors[2] = {"APOLLO-TEST-1", 4000, 0, 1280, 720};
  t = mapper.map(headless, 960, 540);
  EXPECT_DOUBLE_EQ(t.at.x, (4000.0 + 640.0) / 7840.0);
  EXPECT_DOUBLE_EQ(t.at.y, (560.0 + 360.0) / 2560.0);

  // The output disappears: fall back to the port.
  fake->monitors.pop_back();
  fake->monitors.pop_back();
  t = mapper.map(headless, 960, 540);
  EXPECT_DOUBLE_EQ(t.at.x, 4960.0 / 5920.0);

  // Unreachable compositor or none at all: the port.
  fake->reachable = false;
  layout_mapper_t offline {fake};
  t = offline.map(dp3, 0, 0);
  EXPECT_DOUBLE_EQ(t.at.y, 560.0 / 2560.0);
  layout_mapper_t none {nullptr};
  t = none.map(dp3, 0, 0);
  EXPECT_DOUBLE_EQ(t.at.y, 560.0 / 2560.0);
}

// Bugs caught: 0, False positives: 0
TEST(AbsInputTest, OutputMapperBindsToStreamedOutputAndUsesOutputCoordinates) {
  auto fake = fake_hyprland();
  output_mapper_t mapper {"Touch passthrough (client 1)", fake};

  auto t = mapper.map(headless, 960, 540);
  EXPECT_EQ(mapper.bound_output(), "APOLLO-TEST-1");
  ASSERT_EQ(fake->binds.size(), 1u);
  EXPECT_EQ(fake->binds[0], (std::pair<std::string, std::string> {"Touch passthrough (client 1)", "APOLLO-TEST-1"}));
  EXPECT_DOUBLE_EQ(t.at.x, 0.5);
  EXPECT_DOUBLE_EQ(t.at.y, 0.5);

  // Same port: no more compositor traffic.
  mapper.map(headless, 0, 0);
  mapper.map(headless, 1920, 1080);
  EXPECT_EQ(fake->binds.size(), 1u);
  EXPECT_EQ(fake->reads, 1);

  // The stream moves to another output: rebind.
  t = mapper.map(hdmi, 0, 2560);
  EXPECT_EQ(mapper.bound_output(), "HDMI-A-1");
  EXPECT_EQ(fake->binds.size(), 2u);
  EXPECT_DOUBLE_EQ(t.at.x, 0.0);
  EXPECT_DOUBLE_EQ(t.at.y, 1.0);

  // Settings may have been lost (config reload): bind again.
  fake->gen++;
  mapper.map(hdmi, 0, 0);
  EXPECT_EQ(fake->binds.size(), 3u);
  EXPECT_EQ(fake->binds.back().second, "HDMI-A-1");
}

// Bugs caught: 0, False positives: 0
TEST(AbsInputTest, OutputMapperFallsBackToLayout) {
  // No compositor (X11, or no Wayland support compiled in).
  output_mapper_t none {"Pen passthrough (client 1)", nullptr};
  auto t = none.map(headless, 960, 540);
  EXPECT_TRUE(none.bound_output().empty());
  EXPECT_DOUBLE_EQ(t.at.x, 4960.0 / 5920.0);
  EXPECT_DOUBLE_EQ(t.at.y, 1100.0 / 2560.0);

  // A compositor that cannot bind: layout, and no retry before retry_interval.
  auto fake = fake_hyprland();
  fake->bind_ok = false;
  output_mapper_t failing {"Pen passthrough (client 2)", fake};
  t = failing.map(headless, 960, 540);
  EXPECT_TRUE(failing.bound_output().empty());
  EXPECT_DOUBLE_EQ(t.at.x, 4960.0 / 5920.0);
  failing.map(headless, 0, 0);
  EXPECT_EQ(fake->binds.size(), 1u);

  // No output at the port's position (e.g. a stale port): layout, and an earlier binding is removed.
  fake->bind_ok = true;
  output_mapper_t mapper {"Pen passthrough (client 3)", fake};
  mapper.map(headless, 0, 0);
  EXPECT_EQ(mapper.bound_output(), "APOLLO-TEST-1");
  fake->binds.clear();
  auto stale = port(9000, 0, 1920, 1080, desk_headless);
  t = mapper.map(stale, 0, 0);
  EXPECT_TRUE(mapper.bound_output().empty());
  ASSERT_EQ(fake->binds.size(), 1u);
  EXPECT_EQ(fake->binds[0].second, "");
  EXPECT_DOUBLE_EQ(t.at.x, 1.0);

  // A port without the output size (backends that don't report it): layout.
  auto sizeless = headless;
  sizeless.output_width = sizeless.output_height = 0;
  output_mapper_t old {"Pen passthrough (client 4)", fake};
  t = old.map(sizeless, 960, 540);
  EXPECT_TRUE(old.bound_output().empty());
  EXPECT_DOUBLE_EQ(t.at.x, 4960.0 / 5920.0);
}

// Bugs caught: 0, False positives: 0
TEST(AbsInputTest, OutputMapperBoxSizeFollowsTheMapping) {
  // Bound: device axes span the streamed output.
  auto fake = fake_hyprland();
  output_mapper_t mapper {"Touch passthrough (client 5)", fake};
  mapper.map(headless, 0, 0);
  auto box = mapper.box_size(headless);
  EXPECT_DOUBLE_EQ(box.x, 1920.0);
  EXPECT_DOUBLE_EQ(box.y, 1080.0);

  // Unbound: they span the layout box.
  output_mapper_t none {"Touch passthrough (client 6)", nullptr};
  none.map(headless, 0, 0);
  box = none.box_size(headless);
  EXPECT_DOUBLE_EQ(box.x, 5920.0);
  EXPECT_DOUBLE_EQ(box.y, 2560.0);
}

// Bugs caught: 0, False positives: 0
TEST(AbsInputTest, FlatAccelOncePerGeneration) {
  auto fake = fake_hyprland();
  flat_accel_t accel {"Mouse passthrough", fake};
  accel.ensure();
  accel.ensure();
  ASSERT_EQ(fake->flat.size(), 1u);
  EXPECT_EQ(fake->flat[0], "Mouse passthrough");

  fake->gen++;
  accel.ensure();
  accel.ensure();
  EXPECT_EQ(fake->flat.size(), 2u);

  // Failure: retried only after retry_interval.
  fake->bind_ok = false;
  fake->gen++;
  accel.ensure();
  accel.ensure();
  EXPECT_EQ(fake->flat.size(), 3u);

  flat_accel_t none {"Mouse passthrough", nullptr};
  none.ensure();
}

#endif
