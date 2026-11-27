/**
 * @file tests/unit/test_touch_port.cpp
 * @brief Test the touch port that video::make_port() hands to input, with Hyprland-style geometry.
 */
#include "../tests_common.h"

#include <src/input.h>
#include <src/platform/common.h>
#include <src/video.h>

namespace video {
  input::touch_port_t make_port(platf::display_t *display, const config_t &config);
}

namespace {
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

  video::config_t stream_config(int width, int height) {
    video::config_t config {};
    config.width = width;
    config.height = height;
    return config;
  }

  // What input::client_to_touchport() does with a point already in stream pixels: the layout position.
  std::pair<float, float> to_layout(const input::touch_port_t &port, float x, float y) {
    const float local_x = (x - port.client_offsetX) * port.scalar_inv;
    const float local_y = (y - port.client_offsetY) * port.scalar_inv;
    return {(local_x + port.offset_x * port.scalar_tpcoords) / port.scalar_tpcoords, (local_y + port.offset_y * port.scalar_tpcoords) / port.scalar_tpcoords};
  }
}  // namespace

// Bugs caught: 0, False positives: 0
TEST(TouchPortTest, BackendWithoutLogicalSizeIsUnchanged) {
  fake_display_t display;
  display.width = 1920;
  display.height = 1080;
  display.offset_x = 1920;
  display.offset_y = 0;
  display.env_width = 3840;
  display.env_height = 1080;

  const auto port = video::make_port(&display, stream_config(1280, 720));
  EXPECT_EQ(port.offset_x, 1920);
  EXPECT_EQ(port.offset_y, 0);
  EXPECT_EQ(port.width, 1280);
  EXPECT_EQ(port.height, 720);
  EXPECT_EQ(port.env_width, 3840);
  EXPECT_EQ(port.env_height, 1080);
  EXPECT_EQ(port.env_offset_x, 0);
  EXPECT_EQ(port.env_offset_y, 0);
  EXPECT_FLOAT_EQ(port.scalar_inv, 1.5f);
  EXPECT_FLOAT_EQ(port.scalar_tpcoords, 1.0f);
  // Without logical sizes there is nothing to scale by.
  EXPECT_EQ(port.logical_width, 0);
  EXPECT_EQ(port.env_logical_width, 0);

  const auto [x, y] = to_layout(port, 1280, 720);
  EXPECT_FLOAT_EQ(x, 1920 + 1920);
  EXPECT_FLOAT_EQ(y, 1080);
}

// Bugs caught: 0, False positives: 0
TEST(TouchPortTest, WaylandScaledOutputMapsToLogicalPixels) {
  // DP-3 on the development machine: 3840x2160 at scale 1.5, next to HDMI-A-1 at 2560,-560 (logical 1440x2560).
  // The ext capture backend reports the whole layout in logical pixels (wl::apply_output_geometry()).
  fake_display_t display;
  display.width = 3840;
  display.height = 2160;
  display.logical_width = 2560;
  display.logical_height = 1440;
  display.offset_x = 0;
  display.offset_y = 0;
  display.env_offset_x = 0;
  display.env_offset_y = -560;
  display.env_width = 4000;
  display.env_height = 2560;
  display.env_logical_width = 4000;
  display.env_logical_height = 2560;
  display.output_name = "DP-3";

  // 16:10 stream of a 16:9 display: letterboxed by 60 px top and bottom.
  const auto port = video::make_port(&display, stream_config(1920, 1200));
  EXPECT_EQ(port.env_offset_x, 0);
  EXPECT_EQ(port.env_offset_y, -560);
  EXPECT_EQ(port.env_logical_width, 4000);
  EXPECT_EQ(port.env_logical_height, 2560);
  EXPECT_EQ(port.logical_width, 2560);
  EXPECT_EQ(port.logical_height, 1440);
  EXPECT_EQ(port.output_name, "DP-3");
  EXPECT_EQ(port.transform, 0);
  EXPECT_FLOAT_EQ(port.scalar_tpcoords, 1.5f);
  EXPECT_FLOAT_EQ(port.client_offsetX, 0);
  EXPECT_FLOAT_EQ(port.client_offsetY, 60);

  auto [x0, y0] = to_layout(port, 0, 60);
  EXPECT_FLOAT_EQ(x0, 0);
  EXPECT_FLOAT_EQ(y0, 0);

  auto [x1, y1] = to_layout(port, 1920, 1140);
  EXPECT_FLOAT_EQ(x1, 2560);
  EXPECT_FLOAT_EQ(y1, 1440);

  // Normalised against the whole layout, DP-3's top-left sits 560 px below the top of the bounding box.
  EXPECT_FLOAT_EQ(float(y0 - port.env_offset_y) / port.env_logical_height, 560.0f / 2560.0f);
}

// Bugs caught: 0, False positives: 0
TEST(TouchPortTest, WaylandRotatedOutputIsDeliveredUpright) {
  // HDMI-A-1: 3840x2160 at scale 1.5 with transform 1. Hyprland delivers the frame upright (2160x3840), so
  // wl::apply_output_geometry() reports no rotation left to apply.
  fake_display_t display;
  display.width = 2160;
  display.height = 3840;
  display.logical_width = 1440;
  display.logical_height = 2560;
  display.offset_x = 2560;
  display.offset_y = -560;
  display.env_offset_x = 0;
  display.env_offset_y = -560;
  display.env_width = 4000;
  display.env_height = 2560;
  display.env_logical_width = 4000;
  display.env_logical_height = 2560;
  display.output_name = "HDMI-A-1";

  const auto port = video::make_port(&display, stream_config(1080, 1920));
  EXPECT_EQ(port.transform, 0);
  EXPECT_FLOAT_EQ(port.scalar_tpcoords, 1.5f);
  const auto [x, y] = to_layout(port, 1080, 1920);
  EXPECT_FLOAT_EQ(x, 2560 + 1440);
  EXPECT_FLOAT_EQ(y, -560 + 2560);
}

// Bugs caught: 0, False positives: 0
TEST(TouchPortTest, WaylandOutputAtNegativeOffset) {
  // A 1920x1080 scale-1 output placed at -1920,-200.
  fake_display_t display;
  display.width = 1920;
  display.height = 1080;
  display.logical_width = 1920;
  display.logical_height = 1080;
  display.offset_x = -1920;
  display.offset_y = -200;
  display.env_offset_x = -1920;
  display.env_offset_y = -200;
  display.env_width = 4480;
  display.env_height = 1640;
  display.env_logical_width = 4480;
  display.env_logical_height = 1640;

  const auto port = video::make_port(&display, stream_config(1920, 1080));
  EXPECT_EQ(port.offset_x, -1920);
  EXPECT_EQ(port.offset_y, -200);
  EXPECT_FLOAT_EQ(port.scalar_inv, 1.0f);
  EXPECT_FLOAT_EQ(port.scalar_tpcoords, 1.0f);

  const auto [x, y] = to_layout(port, 0, 0);
  EXPECT_FLOAT_EQ((x - port.env_offset_x) / port.env_logical_width, 0.0f);
  EXPECT_FLOAT_EQ((y - port.env_offset_y) / port.env_logical_height, 0.0f);
}
