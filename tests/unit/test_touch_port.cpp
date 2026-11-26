/**
 * @file tests/unit/test_touch_port.cpp
 * @brief Test the touch port that video::make_port() hands to input.
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
      return platf::capture_e::error;
    }

    std::shared_ptr<platf::img_t> alloc_img() override {
      return nullptr;
    }

    int dummy_img(platf::img_t *) override {
      return 0;
    }
  };

  video::config_t stream(int width, int height) {
    video::config_t config {};
    config.width = width;
    config.height = height;
    return config;
  }

  // What input::client_to_touchport() does with a point already in stream pixels.
  std::pair<float, float> to_local(const input::touch_port_t &port, float x, float y) {
    return {(x - port.client_offsetX) * port.scalar_inv, (y - port.client_offsetY) * port.scalar_inv};
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

  const auto port = video::make_port(&display, stream(1280, 720));
  EXPECT_EQ(port.offset_x, 1920);
  EXPECT_EQ(port.offset_y, 0);
  EXPECT_EQ(port.width, 1280);
  EXPECT_EQ(port.height, 720);
  EXPECT_EQ(port.env_width, 3840);
  EXPECT_EQ(port.env_height, 1080);
  EXPECT_EQ(port.env_x, 0);
  EXPECT_EQ(port.env_y, 0);
  EXPECT_FLOAT_EQ(port.scalar_inv, 1.5f);
  EXPECT_EQ(port.logical_width, 1920);
  EXPECT_EQ(port.logical_height, 1080);

  const auto [x, y] = to_local(port, 1280, 720);
  EXPECT_FLOAT_EQ(x, 1920);
  EXPECT_FLOAT_EQ(y, 1080);
}

// Bugs caught: 0, False positives: 0
TEST(TouchPortTest, WaylandScaledOutputMapsToLogicalPixels) {
  // DP-3 on the development machine: 3840x2160 at scale 1.5, next to HDMI-A-1 at 2560,-560 (logical 1440x2560).
  fake_display_t display;
  display.width = 3840;
  display.height = 2160;
  display.logical_width = 2560;
  display.logical_height = 1440;
  display.offset_x = 0;
  display.offset_y = 0;
  display.env_x = 0;
  display.env_y = -560;
  display.env_width = 4000;
  display.env_height = 2560;

  // 16:10 stream of a 16:9 display: letterboxed by 60 px top and bottom.
  const auto port = video::make_port(&display, stream(1920, 1200));
  EXPECT_EQ(port.env_x, 0);
  EXPECT_EQ(port.env_y, -560);
  EXPECT_EQ(port.env_width, 4000);
  EXPECT_EQ(port.env_height, 2560);
  EXPECT_EQ(port.logical_width, 2560);
  EXPECT_EQ(port.logical_height, 1440);
  EXPECT_FLOAT_EQ(port.client_offsetX, 0);
  EXPECT_FLOAT_EQ(port.client_offsetY, 60);

  auto [x0, y0] = to_local(port, 0, 60);
  EXPECT_FLOAT_EQ(x0, 0);
  EXPECT_FLOAT_EQ(y0, 0);

  auto [x1, y1] = to_local(port, 1920, 1140);
  EXPECT_FLOAT_EQ(x1, 2560);
  EXPECT_FLOAT_EQ(y1, 1440);

  // Normalised against the whole layout, DP-3's top-left sits 560 px below the top of the bounding box.
  EXPECT_FLOAT_EQ(float(port.offset_y + y0 - port.env_y) / port.env_height, 560.0f / 2560.0f);
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
  display.env_x = -1920;
  display.env_y = -200;
  display.env_width = 4480;
  display.env_height = 1640;

  const auto port = video::make_port(&display, stream(1920, 1080));
  EXPECT_EQ(port.offset_x, -1920);
  EXPECT_EQ(port.offset_y, -200);
  EXPECT_FLOAT_EQ(port.scalar_inv, 1.0f);

  const auto [x, y] = to_local(port, 0, 0);
  EXPECT_FLOAT_EQ(float(port.offset_x + x - port.env_x) / port.env_width, 0.0f);
  EXPECT_FLOAT_EQ(float(port.offset_y + y - port.env_y) / port.env_height, 0.0f);
}
