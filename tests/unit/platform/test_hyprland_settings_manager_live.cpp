/**
 * @file tests/unit/platform/test_hyprland_settings_manager_live.cpp
 * @brief Test the Hyprland display-device settings manager against the private headless Hyprland of
 *        scripts/hyprland/test-live.sh, whose headless outputs stand in for physical monitors.
 *
 * These tests disable outputs and change their modes, so they must never reach a desktop session. They only run when
 * run-headless-hyprland.sh set up the environment (APOLLO_HEADLESS_HYPRLAND_DIR is the runtime dir), talk to that
 * one instance by signature, and the compositor refuses to change any output that is not headless.
 */
#include "../../tests_common.h"

#ifdef SUNSHINE_BUILD_WAYLAND

  // standard includes
  #include <cstdlib>
  #include <filesystem>
  #include <thread>

  // lib includes
  #include <display_device/file_settings_persistence.h>

  // local includes
  #include <src/platform/linux/hyprland/ipc.h>
  #include <src/platform/linux/hyprland/settings_manager.h>

namespace {
  namespace fs = std::filesystem;
  using namespace hyprland;
  using namespace hyprland::dd;
  using display_device::Rational;
  using display_device::Resolution;
  using display_device::SingleDisplayConfiguration;
  using ApplyResult = display_device::SettingsManagerInterface::ApplyResult;
  using RevertResult = display_device::SettingsManagerInterface::RevertResult;
  using Prep = SingleDisplayConfiguration::DevicePreparation;

  SingleDisplayConfiguration cfg(const std::string &id, Prep prep, std::optional<Resolution> res = {}, std::optional<int> fps = {}) {
    SingleDisplayConfiguration c;
    c.m_device_id = id;
    c.m_device_prep = prep;
    c.m_resolution = res;
    if (fps) {
      c.m_refresh_rate = Rational {static_cast<unsigned int>(*fps * 1000), 1000};
    }
    return c;
  }

  struct HyprlandSettingsManagerLiveTest: testing::Test {
    fs::path hypr_dir;
    std::string signature;
    fs::path state_file;
    std::vector<output_t> initial;
    std::shared_ptr<hyprland_compositor_t> compositor;

    void SetUp() override {
      const char *private_dir = std::getenv("APOLLO_HEADLESS_HYPRLAND_DIR");
      const char *runtime = std::getenv("XDG_RUNTIME_DIR");
      const char *sig = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
      // Only inside the private instance: run-headless-hyprland.sh gives it its own XDG_RUNTIME_DIR.
      if (!private_dir || !runtime || !sig || fs::path {private_dir} != fs::path {runtime}) {
        GTEST_SKIP() << "Needs the private headless Hyprland: run scripts/hyprland/test-live.sh";
      }
      hypr_dir = fs::path {runtime} / "hypr";
      signature = sig;
      compositor = make_compositor([](const output_t &o) {
        return o.any_mode;  // headless outputs only
      });
      auto layout = compositor->layout();
      ASSERT_TRUE(layout) << "private Hyprland " << signature << " does not answer";
      ASSERT_EQ(compositor->signature(), signature);
      for (const auto &o : layout->outputs) {
        // Safety net on top of the compositor's own check: this is not a session with real monitors.
        ASSERT_TRUE(o.any_mode) << o.name << " is not a headless output; refusing to run";
        if (!o.is_virtual && o.enabled) {
          initial.push_back(o);
        }
      }
      if (initial.size() < 2) {
        GTEST_SKIP() << "Needs two enabled headless outputs";
      }
      state_file = fs::path {runtime} / std::format("dd-live-{}.state", ::testing::UnitTest::GetInstance()->current_test_info()->name());
      fs::remove(state_file);
    }

    void TearDown() override {
      if (!compositor) {
        return;
      }
      // Put the instance back for the next test, whatever happened.
      for (const auto &o : initial) {
        monitor_rule_t rule;
        rule.output = o.name;
        rule.disabled = false;
        rule.mode = mode_string(o.mode);
        rule.position = std::format("{}x{}", o.x, o.y);
        compositor->set_monitor(rule);
      }
      std::error_code ec;
      fs::remove(state_file, ec);
    }

    std::shared_ptr<hyprland_compositor_t> make_compositor(std::function<bool(const output_t &)> may_change) {
      hyprland_compositor_t::options_t options;
      options.hypr_dir = hypr_dir;
      options.signature = signature;
      options.may_change = std::move(may_change);
      options.get_virtual_hdr = [](const std::string &) {
        return std::nullopt;
      };
      options.set_virtual_hdr = [](const std::string &, bool) {
        return false;
      };
      return std::make_shared<hyprland_compositor_t>(std::move(options));
    }

    std::unique_ptr<settings_manager_t> manager() {
      return std::make_unique<settings_manager_t>(compositor, std::make_shared<display_device::FileSettingsPersistence>(state_file));
    }

    output_t now(const std::string &name) {
      auto layout = compositor->layout();
      EXPECT_TRUE(layout);
      const auto *o = layout ? layout->find(name) : nullptr;
      EXPECT_TRUE(o) << name;
      return o ? *o : output_t {};
    }

    void expect_initial_layout() {
      for (const auto &o : initial) {
        const auto n = now(o.name);
        EXPECT_TRUE(n.enabled) << o.name;
        EXPECT_EQ(n.mode.width, o.mode.width) << o.name;
        EXPECT_EQ(n.mode.height, o.mode.height) << o.name;
        EXPECT_NEAR(n.mode.refresh_hz, o.mode.refresh_hz, 0.5) << o.name;
        EXPECT_EQ(n.x, o.x) << o.name;
        EXPECT_EQ(n.y, o.y) << o.name;
        EXPECT_EQ(n.active_workspace, o.active_workspace) << o.name;
      }
    }
  };
}  // namespace

// Bugs caught: 0, False positives: 0
TEST_F(HyprlandSettingsManagerLiveTest, OnlyDisplayWithClientModeThenRevert) {
  const auto &target = initial[1].name;
  const auto &other = initial[0].name;
  auto m = manager();

  ASSERT_EQ(m->applySettings(cfg(target, Prep::EnsureOnlyDisplay, Resolution {2560, 1440}, 120)), ApplyResult::Ok);
  auto t = now(target);
  EXPECT_EQ(t.mode.width, 2560);
  EXPECT_EQ(t.mode.height, 1440);
  EXPECT_NEAR(t.mode.refresh_hz, 120.0, 0.5);
  EXPECT_TRUE(t.focused);
  EXPECT_FALSE(now(other).enabled);
  EXPECT_TRUE(fs::exists(state_file));
  // Capture would now find only the target.
  EXPECT_EQ(m->getDisplayName(other), "");
  EXPECT_EQ(m->getDisplayName(target), target);

  ASSERT_EQ(m->revertSettings(), RevertResult::Ok);
  expect_initial_layout();
  EXPECT_TRUE(now(initial[0].focused ? other : target).focused);
  EXPECT_FALSE(fs::exists(state_file));
}

// Bugs caught: 0, False positives: 0
TEST_F(HyprlandSettingsManagerLiveTest, RestoresAfterACrash) {
  const auto &target = initial[0].name;
  const auto &other = initial[1].name;
  {
    auto m = manager();
    ASSERT_EQ(m->applySettings(cfg(target, Prep::EnsureOnlyDisplay, Resolution {1600, 900}, 60)), ApplyResult::Ok);
    ASSERT_FALSE(now(other).enabled);
    // Apollo is killed: the manager goes away without reverting.
  }
  ASSERT_TRUE(fs::exists(state_file));

  // Apollo starts again; display_device::init() creates a new manager and reverts what the file describes.
  auto restarted = manager();
  ASSERT_TRUE(restarted->state());
  ASSERT_EQ(restarted->revertSettings(), RevertResult::Ok);
  expect_initial_layout();
  EXPECT_FALSE(fs::exists(state_file));
}

// Bugs caught: 0, False positives: 0
TEST_F(HyprlandSettingsManagerLiveTest, EnsurePrimaryMovesTheTargetToTheOrigin) {
  const auto &target = initial[1].name;
  auto m = manager();
  ASSERT_EQ(m->applySettings(cfg(target, Prep::EnsurePrimary)), ApplyResult::Ok);
  const auto t = now(target);
  EXPECT_EQ(t.x, 0);
  EXPECT_EQ(t.y, 0);
  EXPECT_TRUE(t.focused);
  ASSERT_EQ(m->revertSettings(), RevertResult::Ok);
  expect_initial_layout();
}

// Bugs caught: 1, False positives: 0
TEST_F(HyprlandSettingsManagerLiveTest, SafetyCheckRefusesOutputsItDoesNotAllow) {
  const auto &protected_output = initial[0].name;
  compositor = make_compositor([&](const output_t &o) {
    return o.name != protected_output;
  });
  auto m = manager();
  // Disabling the protected output is refused, so the apply fails and the rest is reverted.
  EXPECT_NE(m->applySettings(cfg(initial[1].name, Prep::EnsureOnlyDisplay, Resolution {1024, 768}, 60)), ApplyResult::Ok);
  EXPECT_TRUE(now(protected_output).enabled);
  // Hyprland applies mode changes asynchronously: the mode set before the refusal must not land after the revert.
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds {500};
  while (std::chrono::steady_clock::now() < until) {
    ASSERT_EQ(now(initial[1].name).mode.width, initial[1].mode.width);
    std::this_thread::sleep_for(std::chrono::milliseconds {20});
  }
  expect_initial_layout();
  EXPECT_FALSE(fs::exists(state_file));

  // A compositor pinned to an instance that does not exist reaches nothing at all.
  hyprland_compositor_t::options_t options;
  options.hypr_dir = hypr_dir;
  options.signature = signature + "_missing";
  hyprland_compositor_t elsewhere {options};
  EXPECT_FALSE(elsewhere.layout());
  monitor_rule_t rule;
  rule.output = protected_output;
  rule.disabled = true;
  EXPECT_FALSE(elsewhere.set_monitor(rule));
}

#endif
