/**
 * @file tests/unit/platform/test_wayland_geometry.cpp
 * @brief Test src/platform/linux/wayland_geometry.h.
 */
#include "../../tests_common.h"

#include <src/platform/linux/wayland_geometry.h>

namespace {
  // The layout of the development machine: two 4K panels at scale 1.5, the second rotated
  // (transform 1 = 90 degrees) and placed above the top edge of the first.
  wl::output_geometry_t dp3() {
    wl::output_geometry_t g;
    g.logical_x = 0;
    g.logical_y = 0;
    g.logical_width = 2560;
    g.logical_height = 1440;
    g.mode_width = 3840;
    g.mode_height = 2160;
    g.transform = 0;
    g.integer_scale = 2;
    return g;
  }

  wl::output_geometry_t hdmi_a_1() {
    wl::output_geometry_t g;
    g.logical_x = 2560;
    g.logical_y = -560;
    g.logical_width = 1440;
    g.logical_height = 2560;
    g.mode_width = 3840;
    g.mode_height = 2160;
    g.transform = 1;
    g.integer_scale = 2;
    return g;
  }
}  // namespace

// Bugs caught: 0, False positives: 0
TEST(WaylandGeometryTest, TransformedModeSizeSwapsForQuarterTurns) {
  EXPECT_EQ(wl::transformed_mode_size(dp3()), (wl::extent_t {3840, 2160}));
  EXPECT_EQ(wl::transformed_mode_size(hdmi_a_1()), (wl::extent_t {2160, 3840}));

  auto g = dp3();
  for (int transform : {0, 2, 4, 6}) {
    g.transform = transform;
    EXPECT_FALSE(wl::transform_swaps_axes(transform)) << transform;
    EXPECT_EQ(wl::transformed_mode_size(g), (wl::extent_t {3840, 2160})) << transform;
  }
  for (int transform : {1, 3, 5, 7}) {
    g.transform = transform;
    EXPECT_TRUE(wl::transform_swaps_axes(transform)) << transform;
    EXPECT_EQ(wl::transformed_mode_size(g), (wl::extent_t {2160, 3840})) << transform;
  }
}

// Bugs caught: 0, False positives: 0
TEST(WaylandGeometryTest, LogicalSizeComesFromXdgOutput) {
  EXPECT_EQ(wl::logical_size(dp3()), (wl::extent_t {2560, 1440}));
  EXPECT_EQ(wl::logical_size(hdmi_a_1()), (wl::extent_t {1440, 2560}));
  EXPECT_DOUBLE_EQ(wl::effective_scale(dp3()), 1.5);
  EXPECT_DOUBLE_EQ(wl::effective_scale(hdmi_a_1()), 1.5);
}

// Bugs caught: 0, False positives: 0
TEST(WaylandGeometryTest, LogicalSizeFallsBackToModeAndIntegerScale) {
  wl::output_geometry_t g;
  g.mode_width = 3840;
  g.mode_height = 2160;
  g.transform = 3;
  g.integer_scale = 2;
  EXPECT_EQ(wl::logical_size(g), (wl::extent_t {1080, 1920}));
  EXPECT_DOUBLE_EQ(wl::effective_scale(g), 2.0);

  g.integer_scale = 0;  // never sent: treat as 1
  EXPECT_EQ(wl::logical_size(g), (wl::extent_t {2160, 3840}));

  EXPECT_DOUBLE_EQ(wl::effective_scale(wl::output_geometry_t {}), 1.0);
}

// Bugs caught: 0, False positives: 0
TEST(WaylandGeometryTest, LayoutBoundsOfRealLayoutIncludeNegativeOrigin) {
  const auto bounds = wl::layout_bounds({dp3(), hdmi_a_1()});
  // x: 0 .. 2560 + 1440 = 4000; y: -560 .. max(1440, -560 + 2560 = 2000) = 2000.
  EXPECT_EQ(bounds, (wl::layout_bounds_t {0, -560, 4000, 2560}));

  // Order must not matter.
  EXPECT_EQ(wl::layout_bounds({hdmi_a_1(), dp3()}), bounds);
}

// Bugs caught: 0, False positives: 0
TEST(WaylandGeometryTest, LayoutBoundsWithHeadlessOutput) {
  // A headless output that Hyprland auto-places to the right of everything else.
  wl::output_geometry_t headless;
  headless.logical_x = 4000;
  headless.logical_y = 0;
  headless.logical_width = 1920;
  headless.logical_height = 1080;
  headless.mode_width = 1920;
  headless.mode_height = 1080;

  const auto bounds = wl::layout_bounds({dp3(), hdmi_a_1(), headless});
  EXPECT_EQ(bounds, (wl::layout_bounds_t {0, -560, 5920, 2560}));

  // Normalised top-left of each output inside the bounding box, as absolute input needs it.
  auto normalised = [&](const wl::output_geometry_t &g) {
    return std::pair {
      double(g.logical_x - bounds.min_x) / bounds.width,
      double(g.logical_y - bounds.min_y) / bounds.height
    };
  };
  EXPECT_DOUBLE_EQ(normalised(dp3()).first, 0.0);
  EXPECT_DOUBLE_EQ(normalised(dp3()).second, 560.0 / 2560.0);
  EXPECT_DOUBLE_EQ(normalised(hdmi_a_1()).first, 2560.0 / 5920.0);
  EXPECT_DOUBLE_EQ(normalised(hdmi_a_1()).second, 0.0);
  EXPECT_DOUBLE_EQ(normalised(headless).first, 4000.0 / 5920.0);
}

// Bugs caught: 0, False positives: 0
TEST(WaylandGeometryTest, LayoutBoundsSkipsOutputsWithoutGeometry) {
  wl::output_geometry_t unknown;  // no mode and no logical size yet
  unknown.logical_x = -10000;
  EXPECT_EQ(wl::layout_bounds({unknown, dp3()}), (wl::layout_bounds_t {0, 0, 2560, 1440}));
  EXPECT_EQ(wl::layout_bounds({}), (wl::layout_bounds_t {0, 0, 0, 0}));
}

// Bugs caught: 0, False positives: 0
TEST(WaylandGeometryTest, LayoutBoundsAllNegative) {
  auto left = dp3();
  left.logical_x = -2560;
  left.logical_y = -1440;
  EXPECT_EQ(wl::layout_bounds({left}), (wl::layout_bounds_t {-2560, -1440, 2560, 1440}));
}
