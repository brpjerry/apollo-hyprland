/**
 * @file tests/unit/platform/test_wayland_live.cpp
 * @brief Wayland output enumeration and selection against the running compositor.
 * @details Skipped unless WAYLAND_DISPLAY points at a compositor with wlr-screencopy (e.g. Hyprland, Sway).
 *          Set APOLLO_TEST_EXPECT_OUTPUTS to a comma separated list of output names that must be present.
 */
#include "../../tests_common.h"

#if defined(__linux__) && defined(SUNSHINE_BUILD_WAYLAND)

  #include <src/platform/common.h>
  #include <src/video.h>

namespace {
  struct WaylandLiveTest: PlatformTestSuite {
    void SetUp() override {
      if (!std::getenv("WAYLAND_DISPLAY")) {
        GTEST_SKIP() << "WAYLAND_DISPLAY is not set";
      }
      names = platf::display_names(platf::mem_type_e::system);
      if (names.empty()) {
        GTEST_SKIP() << "No outputs available through wlr-screencopy";
      }
    }

    static video::config_t config() {
      video::config_t config {};
      config.width = 1920;
      config.height = 1080;
      config.framerate = 60;
      return config;
    }

    std::vector<std::string> names;
  };

  std::vector<std::string> split(std::string_view list) {
    std::vector<std::string> result;
    while (!list.empty()) {
      auto comma = list.find(',');
      auto item = list.substr(0, comma);
      if (!item.empty()) {
        result.emplace_back(item);
      }
      if (comma == std::string_view::npos) {
        break;
      }
      list.remove_prefix(comma + 1);
    }
    return result;
  }
}  // namespace

// Bugs caught: 0, False positives: 0
TEST_F(WaylandLiveTest, EnumeratesOutputsByConnectorName) {
  for (const auto &name : names) {
    std::cout << "Wayland output: " << name << std::endl;
    EXPECT_FALSE(name.empty());
  }

  if (const char *expected = std::getenv("APOLLO_TEST_EXPECT_OUTPUTS")) {
    for (const auto &name : split(expected)) {
      EXPECT_NE(std::find(names.begin(), names.end(), name), names.end()) << name << " is missing";
    }
  }
}

// Bugs caught: 0, False positives: 0
TEST_F(WaylandLiveTest, SelectsOutputsByNameAndLegacyIndex) {
  for (std::size_t x = 0; x < names.size(); ++x) {
    auto by_name = platf::display(platf::mem_type_e::system, names[x], config());
    ASSERT_TRUE(by_name) << names[x];
    auto by_index = platf::display(platf::mem_type_e::system, std::to_string(x), config());
    ASSERT_TRUE(by_index) << x;

    std::cout << names[x] << ": frame " << by_name->width << 'x' << by_name->height
              << ", logical " << by_name->logical_width << 'x' << by_name->logical_height
              << " at " << by_name->offset_x << ',' << by_name->offset_y
              << ", layout " << by_name->env_width << 'x' << by_name->env_height
              << " at " << by_name->env_x << ',' << by_name->env_y << std::endl;

    EXPECT_GT(by_name->logical_width, 0);
    EXPECT_GT(by_name->logical_height, 0);
    EXPECT_LE(by_name->env_x, by_name->offset_x);
    EXPECT_LE(by_name->env_y, by_name->offset_y);
    EXPECT_GE(by_name->env_x + by_name->env_width, by_name->offset_x + by_name->logical_width);
    EXPECT_GE(by_name->env_y + by_name->env_height, by_name->offset_y + by_name->logical_height);

    EXPECT_EQ(by_index->offset_x, by_name->offset_x);
    EXPECT_EQ(by_index->offset_y, by_name->offset_y);
    EXPECT_EQ(by_index->width, by_name->width);
    EXPECT_EQ(by_index->height, by_name->height);
  }
}

// Bugs caught: 0, False positives: 0
TEST_F(WaylandLiveTest, MissingOutputIsAnError) {
  EXPECT_FALSE(platf::display(platf::mem_type_e::system, "APOLLO-TEST-does-not-exist", config()));
  EXPECT_FALSE(platf::display(platf::mem_type_e::system, std::to_string(names.size()), config()));
}

// Bugs caught: 1, False positives: 0
TEST_F(WaylandLiveTest, CapturedFrameMatchesReportedSize) {
  for (const auto &name : names) {
    auto disp = platf::display(platf::mem_type_e::system, name, config());
    ASSERT_TRUE(disp) << name;

    bool got_frame = false;
    bool cursor = false;
    auto status = disp->capture(
      [&](std::shared_ptr<platf::img_t> &&, bool frame_captured) {
        got_frame = frame_captured;
        return false;
      },
      [&](std::shared_ptr<platf::img_t> &img_out) {
        img_out = disp->alloc_img();
        return true;
      },
      &cursor
    );

    std::cout << name << ": capture status " << (int) status << ", frame " << (got_frame ? "captured" : "timed out") << std::endl;
    EXPECT_EQ(status, platf::capture_e::ok) << name << ": screencopy frame size differs from " << disp->width << 'x' << disp->height;
  }
}

#endif
