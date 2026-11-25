/**
 * @file tests/unit/platform/test_hyprland_session_live.cpp
 * @brief Waking the streamed output (hyprland::wake_output), against the private headless Hyprland.
 *
 * Run with scripts/hyprland/test-live.sh. Refuses to run outside the private instance started by
 * run-headless-hyprland.sh, since it switches an output off.
 */
#include "../../tests_common.h"

#ifdef SUNSHINE_BUILD_WAYLAND

  // standard includes
  #include <chrono>
  #include <cstdlib>
  #include <memory>
  #include <optional>
  #include <string>
  #include <thread>

  // local includes
  #include <src/platform/linux/hyprland/ipc.h>
  #include <src/platform/linux/hyprland/session.h>

namespace {
  using namespace std::chrono_literals;

  class HyprlandSessionLiveTest: public ::testing::Test {
  protected:
    void SetUp() override {
      const char *private_dir = std::getenv("APOLLO_HEADLESS_HYPRLAND_DIR");
      const char *runtime_dir = std::getenv("XDG_RUNTIME_DIR");
      if (!private_dir || !runtime_dir || std::string {private_dir} != runtime_dir || !std::getenv("HYPRLAND_INSTANCE_SIGNATURE")) {
        GTEST_SKIP() << "needs the private headless Hyprland (scripts/hyprland/test-live.sh)";
      }
      ipc = hyprland::ipc_t::connect();
      if (!ipc) {
        GTEST_SKIP() << "Hyprland does not answer";
      }
      for (const auto &m : ipc->monitors(true)) {
        if (m.name.starts_with("APOLLO-TEST-")) {
          (target.empty() ? target : other) = m.name;
        }
      }
      if (target.empty() || other.empty()) {
        GTEST_SKIP() << "needs two APOLLO-TEST-* outputs";
      }
    }

    void TearDown() override {
      if (ipc && !target.empty()) {
        ipc->set_dpms(target, true);
      }
    }

    std::optional<bool> dpms_on(const std::string &name) {
      for (const auto &m : ipc->monitors(true)) {
        if (m.name == name) {
          return m.dpms_on;
        }
      }
      return std::nullopt;
    }

    bool wait_for_dpms(const std::string &name, bool on) {
      auto deadline = std::chrono::steady_clock::now() + 5s;
      while (dpms_on(name) != on) {
        if (std::chrono::steady_clock::now() > deadline) {
          return false;
        }
        std::this_thread::sleep_for(20ms);
      }
      return true;
    }

    std::unique_ptr<hyprland::ipc_t> ipc;
    std::string target;  ///< The "streamed" output.
    std::string other;
  };
}  // namespace

// Bugs caught: 0, False positives: 0
TEST_F(HyprlandSessionLiveTest, WakesOnlyTheStreamedOutput) {
  ASSERT_TRUE(ipc->set_dpms(target, false));
  ASSERT_TRUE(wait_for_dpms(target, false));
  EXPECT_EQ(dpms_on(other), true);

  EXPECT_EQ(hyprland::wake_output(*ipc, target), hyprland::wake_result_e::woken);
  EXPECT_TRUE(wait_for_dpms(target, true));
  EXPECT_EQ(hyprland::wake_output(*ipc, target), hyprland::wake_result_e::already_on);
  EXPECT_EQ(hyprland::wake_output(*ipc, "APOLLO-TEST-missing"), hyprland::wake_result_e::not_found);
  EXPECT_EQ(dpms_on(other), true);
}

#endif
