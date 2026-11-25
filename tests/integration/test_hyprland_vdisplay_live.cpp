/**
 * @file tests/integration/test_hyprland_vdisplay_live.cpp
 * @brief Test platf::vdisplay on a live Hyprland session, if one is running.
 *
 * Creates the virtual display for a random identity (APOLLO-<8 random hex digits>) at 2560x1600@120 through
 * platf::vdisplay::create(), checks Hyprland and Wayland capture see it, removes it and checks it is gone.
 * Skipped when no Hyprland instance answers, or when APOLLO_SKIP_LIVE_HYPRLAND is set.
 */
#include "../tests_common.h"

#ifdef SUNSHINE_BUILD_WAYLAND

  // standard includes
  #include <algorithm>
  #include <cstdlib>

  // local includes
  #include <src/platform/linux/hyprland/ipc.h>
  #include <src/platform/linux/hyprland/vdisplay.h>
  #include <src/platform/vdisplay.h>

namespace {
  /**
   * @brief Removes the output when the test ends, including on assertion failure.
   */
  struct vdisplay_guard_t {
    const hyprland::ipc_t &ipc;
    uuid_util::uuid_t identity;
    std::string name;

    ~vdisplay_guard_t() {
      platf::vdisplay::remove(identity);
      ipc.remove_output(name);  // in case the backend lost track of it; fails harmlessly if already gone
    }
  };

  std::optional<hyprland::monitor_info_t> find_monitor(const hyprland::ipc_t &ipc, const std::string &name) {
    for (auto &m : ipc.monitors()) {
      if (m.name == name) {
        return m;
      }
    }
    return std::nullopt;
  }

  struct HyprlandVdisplayLiveTest: PlatformTestSuite {};
}  // namespace

// Bugs caught: 1, False positives: 1
TEST_F(HyprlandVdisplayLiveTest, CreateStreamableOutputAtClientModeAndRemoveIt) {
  if (std::getenv("APOLLO_SKIP_LIVE_HYPRLAND")) {
    GTEST_SKIP() << "APOLLO_SKIP_LIVE_HYPRLAND is set";
  }
  auto ipc = hyprland::ipc_t::connect();
  if (!ipc) {
    GTEST_SKIP() << "no live Hyprland instance found";
  }
  ASSERT_EQ(platf::vdisplay::init(), platf::vdisplay::status_e::ok);

  const auto identity = uuid_util::uuid_t::generate();
  const auto name = hyprland::vdisplay_output_name(identity);
  ASSERT_FALSE(find_monitor(*ipc, name)) << name << " already exists";
  vdisplay_guard_t guard {*ipc, identity, name};

  ASSERT_EQ(platf::vdisplay::create(identity, "Live test", {2560, 1600, 120000}), name);
  EXPECT_EQ(platf::vdisplay::output_name_for(name), name);

  // create() only returns once the mode is applied, so no polling here.
  auto monitor = find_monitor(*ipc, name);
  ASSERT_TRUE(monitor) << name << " missing from monitors()";
  EXPECT_EQ(monitor->width, 2560);
  EXPECT_EQ(monitor->height, 1600);
  EXPECT_NEAR(monitor->refresh_hz, 120.0, 0.05);
  EXPECT_DOUBLE_EQ(monitor->scale, 1.0);
  EXPECT_FALSE(monitor->disabled);

  // Wayland capture can select it by name.
  const auto names = platf::display_names(platf::mem_type_e::system);
  EXPECT_NE(std::ranges::find(names, name), names.end()) << name << " not among the Wayland outputs";

  // The mode change process.cpp makes right after create() is a no-op, and a different mode applies.
  EXPECT_TRUE(platf::vdisplay::set_mode(name, {2560, 1600, 120000}, false));
  EXPECT_TRUE(platf::vdisplay::set_mode(name, {1920, 1080, 59940}, false));
  monitor = find_monitor(*ipc, name);
  ASSERT_TRUE(monitor);
  EXPECT_EQ(monitor->width, 1920);
  EXPECT_EQ(monitor->height, 1080);
  // Hyprland snaps 59.94 to the 60 Hz mode a headless output lists.
  EXPECT_NEAR(monitor->refresh_hz, 59.94, 1.0);

  EXPECT_TRUE(platf::vdisplay::remove(identity));
  EXPECT_FALSE(find_monitor(*ipc, name)) << name << " still exists after remove()";
}

#endif
