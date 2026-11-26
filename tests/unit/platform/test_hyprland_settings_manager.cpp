/**
 * @file tests/unit/platform/test_hyprland_settings_manager.cpp
 * @brief Test the Hyprland display-device settings manager (src/platform/linux/hyprland/settings_manager.*)
 *        against a fake compositor.
 */
#include "../../tests_common.h"

#ifdef SUNSHINE_BUILD_WAYLAND

  // standard includes
  #include <algorithm>
  #include <format>
  #include <functional>
  #include <map>
  #include <set>

  // local includes
  #include <src/platform/linux/hyprland/settings_manager.h>
  #include <src/platform/linux/hyprland/vdisplay.h>

namespace {
  using namespace hyprland;
  using namespace hyprland::dd;
  using display_device::HdrState;
  using display_device::Rational;
  using display_device::Resolution;
  using display_device::SingleDisplayConfiguration;
  using ApplyResult = display_device::SettingsManagerInterface::ApplyResult;
  using RevertResult = display_device::SettingsManagerInterface::RevertResult;
  using Prep = SingleDisplayConfiguration::DevicePreparation;

  /**
   * @brief In-memory persistence.
   */
  struct memory_persistence_t: display_device::SettingsPersistenceInterface {
    std::optional<std::vector<std::uint8_t>> data;

    bool store(const std::vector<std::uint8_t> &bytes) override {
      data = bytes;
      return true;
    }

    std::optional<std::vector<std::uint8_t>> load() const override {
      return data ? data : std::make_optional(std::vector<std::uint8_t> {});
    }

    bool clear() override {
      data.reset();
      return true;
    }

    std::optional<state_t> state() const {
      if (!data) {
        return std::nullopt;
      }
      return state_from_json(nlohmann::json::parse(data->begin(), data->end()));
    }
  };

  /**
   * @brief A compositor that applies rules to an in-memory layout, roughly like Hyprland does.
   */
  struct fake_compositor_t: compositor_t {
    layout_t current;
    bool reachable = true;
    bool merges = true;
    std::vector<monitor_rule_t> rules;
    std::vector<std::string> calls;  ///< "focus X", "show M W", "vhdr X 1"
    std::map<std::string, bool> vhdr;
    std::set<std::string> ignore_mode;  ///< Outputs that keep their mode whatever the rule says.
    int delay_polls = 0;  ///< layout() calls before queued rules take effect (Hyprland applies some asynchronously).
    bool steal_active = false;  ///< A monitor receiving workspaces of a disabled one shows the moved workspace.
    std::function<void(const monitor_rule_t &)> on_rule;
    std::set<std::string> refuse;  ///< Outputs whose rules are rejected.
    bool refuse_dispatch = false;  ///< Reject focus and workspace dispatches.

    std::vector<monitor_rule_t> queued;
    int polls = 0;

    output_t *find(const std::string &name) {
      auto it = std::ranges::find(current.outputs, name, &output_t::name);
      return it == current.outputs.end() ? nullptr : &*it;
    }

    void apply(const monitor_rule_t &rule) {
      auto *o = find(rule.output);
      if (!o) {
        return;
      }
      if (rule.disabled && *rule.disabled) {
        o->enabled = false;
        const bool had_focus = o->focused;
        o->focused = false;
        o->active_workspace.clear();
        output_t *receiver = nullptr;
        for (auto &other : current.outputs) {
          if (other.enabled && (!receiver || other.focused)) {
            receiver = &other;
          }
        }
        if (receiver) {
          for (auto &w : current.workspaces) {
            if (w.monitor == o->name) {
              w.monitor = receiver->name;
              if (steal_active) {
                receiver->active_workspace = w.name;
              }
            }
          }
          if (had_focus || steal_active) {
            for (auto &other : current.outputs) {
              other.focused = &other == receiver;
            }
          }
        }
        return;
      }
      if (rule.disabled && !*rule.disabled) {
        o->enabled = true;
      }
      if (rule.mode && !ignore_mode.contains(o->name)) {
        o->mode = *parse_mode_string(*rule.mode);
      }
      if (rule.position) {
        const auto x = rule.position->find('x');
        o->x = std::stoi(rule.position->substr(0, x));
        o->y = std::stoi(rule.position->substr(x + 1));
      }
      if (rule.cm && o->hdr_capable) {
        o->cm = *rule.cm;
      }
      if (rule.bitdepth) {
        o->bitdepth = rule.bitdepth;
      }
    }

    std::optional<layout_t> layout() override {
      if (!reachable) {
        return std::nullopt;
      }
      if (!queued.empty() && ++polls > delay_polls) {
        for (const auto &rule : queued) {
          apply(rule);
        }
        queued.clear();
        polls = 0;
      }
      return current;
    }

    bool merges_rules() const override {
      return merges;
    }

    bool set_monitor(const monitor_rule_t &rule) override {
      if (on_rule) {
        on_rule(rule);
      }
      if (refuse.contains(rule.output)) {
        return false;
      }
      rules.push_back(rule);
      if (delay_polls > 0) {
        queued.push_back(rule);
      } else {
        apply(rule);
      }
      return true;
    }

    bool focus_monitor(const std::string &name) override {
      calls.push_back("focus " + name);
      if (refuse_dispatch) {
        return false;
      }
      for (auto &o : current.outputs) {
        o.focused = o.name == name;
      }
      return true;
    }

    bool show_workspace(const std::string &monitor, const std::string &workspace) override {
      calls.push_back("show " + monitor + " " + workspace);
      if (refuse_dispatch) {
        return false;
      }
      auto it =std::ranges::find(current.workspaces, workspace, &workspace_t::name);
      if (it == current.workspaces.end()) {
        current.workspaces.push_back({workspace, monitor});
      } else {
        it->monitor = monitor;
      }
      if (auto *o = find(monitor)) {
        o->active_workspace = workspace;
      }
      return true;
    }

    std::optional<bool> virtual_hdr(const std::string &name) override {
      auto it = vhdr.find(name);
      return it == vhdr.end() ? std::nullopt : std::make_optional(it->second);
    }

    bool set_virtual_hdr(const std::string &name, bool enable) override {
      calls.push_back(std::format("vhdr {} {}", name, enable ? 1 : 0));
      vhdr[name] = enable;
      return true;
    }
  };

  output_t output(const std::string &name, output_mode_t mode, int x, int y, const std::string &workspace) {
    output_t o;
    o.name = name;
    o.description = "Monitor " + name;
    o.mode = mode;
    o.x = x;
    o.y = y;
    o.cm = "srgb";
    o.bitdepth = 8;
    o.any_mode = true;
    o.active_workspace = workspace;
    o.is_virtual = is_vdisplay_output_name(name);
    return o;
  }

  /// Two monitors like the user's desk: DP-3 at the origin with focus, HDMI-A-1 to its right.
  std::shared_ptr<fake_compositor_t> desk() {
    auto c = std::make_shared<fake_compositor_t>();
    c->current.outputs.push_back(output("DP-3", {3840, 2160, 240.0}, 0, 0, "1"));
    c->current.outputs.push_back(output("HDMI-A-1", {3840, 2160, 239.991}, 2560, -560, "2"));
    c->current.outputs[0].focused = true;
    c->current.workspaces = {{"1", "DP-3"}, {"2", "HDMI-A-1"}, {"3", "DP-3"}};
    return c;
  }

  SingleDisplayConfiguration cfg(const std::string &id, Prep prep, std::optional<Resolution> res = {}, std::optional<double> hz = {}, std::optional<HdrState> hdr = {}) {
    SingleDisplayConfiguration c;
    c.m_device_id = id;
    c.m_device_prep = prep;
    c.m_resolution = res;
    if (hz) {
      // As display_device.cpp passes the client's FPS: millihertz over 1000.
      c.m_refresh_rate = Rational {static_cast<unsigned int>(*hz * 1000), 1000};
    }
    c.m_hdr_state = hdr;
    return c;
  }

  settings_manager_t::options_t fast() {
    settings_manager_t::options_t options;
    options.settle_timeout = std::chrono::milliseconds {100};
    options.poll_interval = std::chrono::milliseconds {1};
    return options;
  }

  const output_t &out(const fake_compositor_t &c, const std::string &name) {
    return *c.current.find(name);
  }

  const workspace_t &ws(const fake_compositor_t &c, const std::string &name) {
    return *c.current.find_workspace(name);
  }
}  // namespace

// Bugs caught: 0, False positives: 0
TEST(HyprlandSettingsManagerTest, ParsesMonitorsWorkspacesAndModes) {
  const auto monitors = nlohmann::json::parse(R"([
    {"id": 1, "name": "DP-3", "description": "Samsung Odyssey G80SH", "width": 3840, "height": 2160,
     "refreshRate": 239.991, "x": 0, "y": 0, "scale": 1.5, "transform": 0, "disabled": false, "dpmsStatus": true,
     "focused": true, "currentFormat": "XBGR2101010", "colorManagementPreset": "hdr",
     "activeWorkspace": {"id": 1, "name": "1"},
     "availableModes": ["3840x2160@240.00Hz", "2560x1440@119.88Hz", "garbage"],
     "hardwareDetails": {"backend": "drm", "hdr": true}},
    {"id": 2, "name": "FALLBACK", "width": 1920, "height": 1080, "refreshRate": 60, "disabled": false},
    {"id": 3, "name": "APOLLO-1a2b3c4d", "width": 1280, "height": 800, "refreshRate": 60, "disabled": true,
     "currentFormat": "XRGB8888", "hardwareDetails": {"backend": "headless", "hdr": false}}
  ])");
  const auto outputs = parse_outputs(monitors);
  ASSERT_EQ(outputs.size(), 2u);
  const auto &dp = outputs[0];
  EXPECT_EQ(dp.name, "DP-3");
  EXPECT_EQ(dp.description, "Samsung Odyssey G80SH");
  EXPECT_EQ(dp.mode, (output_mode_t {3840, 2160, 239.991}));
  EXPECT_EQ(dp.bitdepth, 10);
  EXPECT_EQ(dp.cm, "hdr");
  EXPECT_TRUE(dp.hdr_capable);
  EXPECT_FALSE(dp.any_mode);
  EXPECT_FALSE(dp.is_virtual);
  EXPECT_TRUE(dp.focused);
  EXPECT_EQ(dp.active_workspace, "1");
  ASSERT_EQ(dp.modes.size(), 2u);
  EXPECT_EQ(dp.modes[1], (output_mode_t {2560, 1440, 119.88}));
  const auto &vd = outputs[1];
  EXPECT_TRUE(vd.is_virtual);
  EXPECT_TRUE(vd.any_mode);
  EXPECT_FALSE(vd.enabled);
  EXPECT_EQ(vd.bitdepth, 8);

  const auto workspaces = parse_workspaces(nlohmann::json::parse(R"([
    {"id": 1, "name": "1", "monitor": "DP-3"}, {"id": -98, "name": "special:magic", "monitor": "DP-3"},
    {"id": 7, "name": "apollo-1a2b3c4d", "monitor": "APOLLO-1a2b3c4d"}])"));
  EXPECT_EQ(workspaces, (std::vector<workspace_t> {{"1", "DP-3"}, {"apollo-1a2b3c4d", "APOLLO-1a2b3c4d"}}));

  EXPECT_EQ(parse_mode_string("1920x1080@60"), (output_mode_t {1920, 1080, 60.0}));
  EXPECT_FALSE(parse_mode_string("1920x1080"));
  EXPECT_FALSE(parse_mode_string("0x1080@60"));
  EXPECT_EQ(mode_string({1920, 1080, 59.94}), "1920x1080@59.940");
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandSettingsManagerTest, ChoosesAnAvailableModeOnRealMonitors) {
  output_t drm = output("DP-3", {3840, 2160, 240.0}, 0, 0, "1");
  drm.any_mode = false;
  drm.modes = {{3840, 2160, 60.0}, {3840, 2160, 240.0}, {3840, 2160, 119.88}, {3840, 2160, 59.94}, {2560, 1440, 120.0}};
  EXPECT_EQ(choose_mode(drm, 3840, 2160, 120.0), (output_mode_t {3840, 2160, 119.88}));  // within 0.5 Hz
  EXPECT_EQ(choose_mode(drm, 3840, 2160, 60.0), (output_mode_t {3840, 2160, 60.0}));  // closest of 59.94 and 60
  EXPECT_EQ(choose_mode(drm, 3840, 2160, 90.0), (output_mode_t {3840, 2160, 119.88}));  // lowest above
  EXPECT_EQ(choose_mode(drm, 3840, 2160, 360.0), (output_mode_t {3840, 2160, 240.0}));  // highest
  EXPECT_FALSE(choose_mode(drm, 1920, 1080, 60.0));
  // Headless and virtual outputs take what is asked.
  EXPECT_EQ(choose_mode(output("H", {1, 1, 1}, 0, 0, ""), 1234, 567, 89.0), (output_mode_t {1234, 567, 89.0}));
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandSettingsManagerTest, EnumeratesOutputsAndMapsNames) {
  auto c = desk();
  c->current.outputs[1].enabled = false;
  c->current.outputs.push_back(output("APOLLO-1a2b3c4d", {1280, 800, 60.0}, 9000, 9000, "apollo-1a2b3c4d"));
  c->vhdr["APOLLO-1a2b3c4d"] = true;
  auto p = std::make_shared<memory_persistence_t>();
  settings_manager_t m {c, p, fast()};

  const auto devices = m.enumAvailableDevices();
  ASSERT_EQ(devices.size(), 3u);
  EXPECT_EQ(devices[0].m_device_id, "DP-3");
  EXPECT_EQ(devices[0].m_display_name, "DP-3");
  EXPECT_EQ(devices[0].m_friendly_name, "Monitor DP-3");
  ASSERT_TRUE(devices[0].m_info);
  EXPECT_TRUE(devices[0].m_info->m_primary);
  EXPECT_EQ(devices[0].m_info->m_resolution, (Resolution {3840, 2160}));
  EXPECT_FALSE(devices[0].m_info->m_hdr_state);  // not HDR capable
  // Disabled: no display name and no info, as for an inactive device on Windows.
  EXPECT_EQ(devices[1].m_display_name, "");
  EXPECT_FALSE(devices[1].m_info);
  ASSERT_TRUE(devices[2].m_info);
  EXPECT_EQ(devices[2].m_info->m_hdr_state, HdrState::Enabled);
  EXPECT_FALSE(devices[2].m_info->m_primary);

  EXPECT_EQ(m.getDisplayName(""), "DP-3");  // the primary display, as applySettings() resolves it
  EXPECT_EQ(m.getDisplayName("DP-3"), "DP-3");
  EXPECT_EQ(m.getDisplayName("Monitor DP-3"), "DP-3");
  EXPECT_EQ(m.getDisplayName("desc:Monitor D"), "DP-3");
  EXPECT_EQ(m.getDisplayName("HDMI-A-1"), "");  // disabled
  EXPECT_EQ(m.getDisplayName("0"), "0");  // unknown names pass through (capture's legacy index)
  c->reachable = false;
  EXPECT_EQ(m.getDisplayName("DP-3"), "DP-3");
  EXPECT_TRUE(m.enumAvailableDevices().empty());
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandSettingsManagerTest, DefaultDisplayIsTheOneApplySettingsConfigures) {
  // Wayland lists HDMI-A-1 first; capture on its own would take it. The primary display is DP-3, at the origin.
  auto c = desk();
  std::ranges::reverse(c->current.outputs);
  auto p = std::make_shared<memory_persistence_t>();
  settings_manager_t m {c, p, fast()};
  EXPECT_EQ(m.getDisplayName(""), "DP-3");

  ASSERT_EQ(m.applySettings(cfg("", Prep::EnsureActive, Resolution {1920, 1080})), ApplyResult::Ok);
  EXPECT_EQ(out(*c, "DP-3").mode, (output_mode_t {1920, 1080, 240.0}));
  // The layout changes during the stream and HDMI-A-1 ends up at the origin: the stream stays on the display that
  // was configured for it.
  c->find("DP-3")->x = 3840;
  c->find("HDMI-A-1")->x = 0;
  c->find("HDMI-A-1")->y = 0;
  EXPECT_EQ(m.getDisplayName(""), "DP-3");

  ASSERT_EQ(m.revertSettings(), RevertResult::Ok);
  EXPECT_EQ(m.getDisplayName(""), "DP-3");

  // Nothing at the origin: the focused monitor, as applySettings() would choose.
  c->find("DP-3")->x = 100;
  c->focus_monitor("HDMI-A-1");
  EXPECT_EQ(m.getDisplayName(""), "HDMI-A-1");
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandSettingsManagerTest, OnlyDisplayWithClientModeThenRevert) {
  auto c = desk();
  auto p = std::make_shared<memory_persistence_t>();
  settings_manager_t m {c, p, fast()};
  // The original layout is saved before the first change.
  bool saved_first = false;
  c->on_rule = [&](const monitor_rule_t &) {
    if (c->rules.empty()) {
      saved_first = p->state().has_value();
    }
  };

  ASSERT_EQ(m.applySettings(cfg("HDMI-A-1", Prep::EnsureOnlyDisplay, Resolution {2560, 1440}, 120.0)), ApplyResult::Ok);
  EXPECT_TRUE(saved_first);
  EXPECT_EQ(out(*c, "HDMI-A-1").mode, (output_mode_t {2560, 1440, 120.0}));
  EXPECT_TRUE(out(*c, "HDMI-A-1").enabled);
  EXPECT_FALSE(out(*c, "DP-3").enabled);
  EXPECT_TRUE(out(*c, "HDMI-A-1").focused);
  // The target keeps showing its own workspace.
  EXPECT_EQ(out(*c, "HDMI-A-1").active_workspace, "2");

  const auto state = p->state();
  ASSERT_TRUE(state);
  EXPECT_EQ(state->touched.at("DP-3"), (touched_t {.enabled = true}));
  EXPECT_EQ(state->touched.at("HDMI-A-1"), (touched_t {.mode = true}));
  EXPECT_EQ(state->focused, "DP-3");
  EXPECT_EQ(state->find("HDMI-A-1")->mode, (output_mode_t {3840, 2160, 239.991}));

  ASSERT_EQ(m.revertSettings(), RevertResult::Ok);
  EXPECT_TRUE(out(*c, "DP-3").enabled);
  EXPECT_EQ(out(*c, "HDMI-A-1").mode, (output_mode_t {3840, 2160, 239.991}));
  EXPECT_EQ(out(*c, "HDMI-A-1").x, 2560);
  EXPECT_EQ(out(*c, "HDMI-A-1").y, -560);
  // Workspaces moved off DP-3 go back, it shows the one it showed, and it has focus again.
  EXPECT_EQ(ws(*c, "1").monitor, "DP-3");
  EXPECT_EQ(ws(*c, "3").monitor, "DP-3");
  EXPECT_EQ(out(*c, "DP-3").active_workspace, "1");
  EXPECT_TRUE(out(*c, "DP-3").focused);
  EXPECT_FALSE(p->data);
  EXPECT_FALSE(m.state());

  // Nothing left to do.
  c->rules.clear();
  EXPECT_EQ(m.revertSettings(), RevertResult::Ok);
  EXPECT_TRUE(c->rules.empty());
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandSettingsManagerTest, RevertKeepsTheStateUntilThePositionsAreBack) {
  auto c = desk();
  auto p = std::make_shared<memory_persistence_t>();
  settings_manager_t m {c, p, fast()};
  ASSERT_EQ(m.applySettings(cfg("HDMI-A-1", Prep::EnsureOnlyDisplay, Resolution {1920, 1080}, 60.0)), ApplyResult::Ok);

  // Re-enabling DP-3 pushes HDMI-A-1 aside (as for an `auto` position), and Hyprland rejects moving it back.
  c->on_rule = [&](const monitor_rule_t &rule) {
    if (rule.output == "DP-3" && rule.disabled == false) {
      c->find("HDMI-A-1")->x = 3840;
    }
    if (rule.output == "HDMI-A-1" && rule.position && !rule.mode) {
      c->refuse.insert("HDMI-A-1");
    }
  };
  EXPECT_NE(m.revertSettings(), RevertResult::Ok);
  EXPECT_EQ(out(*c, "HDMI-A-1").x, 3840);
  EXPECT_TRUE(p->data) << "the original layout was forgotten before it was back";
  EXPECT_TRUE(m.state());

  c->on_rule = nullptr;
  c->refuse.clear();
  ASSERT_EQ(m.revertSettings(), RevertResult::Ok);
  EXPECT_EQ(std::pair(out(*c, "HDMI-A-1").x, out(*c, "HDMI-A-1").y), std::pair(2560, -560));
  EXPECT_FALSE(p->data);
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandSettingsManagerTest, RevertShowsTheActiveWorkspaceAfterMovingOthersBack) {
  auto c = desk();
  auto p = std::make_shared<memory_persistence_t>();
  settings_manager_t m {c, p, fast()};
  ASSERT_EQ(m.applySettings(cfg("HDMI-A-1", Prep::EnsureOnlyDisplay)), ApplyResult::Ok);
  ASSERT_EQ(ws(*c, "3").monitor, "HDMI-A-1");

  // Hyprland gives the re-enabled DP-3 back the workspace it showed, but not workspace 3.
  c->on_rule = [&](const monitor_rule_t &rule) {
    if (rule.output == "DP-3" && rule.disabled == false) {
      std::ranges::find(c->current.workspaces, "1", &workspace_t::name)->monitor = "DP-3";
      c->find("DP-3")->active_workspace = "1";
    }
  };
  ASSERT_EQ(m.revertSettings(), RevertResult::Ok);
  EXPECT_EQ(ws(*c, "3").monitor, "DP-3");
  // Moving workspace 3 over showed it on DP-3; DP-3 must end up showing workspace 1 again.
  EXPECT_EQ(out(*c, "DP-3").active_workspace, "1");
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandSettingsManagerTest, RevertKeepsTheStateWhenWorkspacesCannotBeRestored) {
  auto c = desk();
  auto p = std::make_shared<memory_persistence_t>();
  settings_manager_t m {c, p, fast()};
  ASSERT_EQ(m.applySettings(cfg("HDMI-A-1", Prep::EnsureOnlyDisplay)), ApplyResult::Ok);

  c->refuse_dispatch = true;
  EXPECT_EQ(m.revertSettings(), RevertResult::ApiTemporarilyUnavailable);
  EXPECT_TRUE(out(*c, "DP-3").enabled);
  EXPECT_TRUE(p->data);

  // The retry finishes the job.
  c->refuse_dispatch = false;
  ASSERT_EQ(m.revertSettings(), RevertResult::Ok);
  EXPECT_EQ(ws(*c, "1").monitor, "DP-3");
  EXPECT_EQ(ws(*c, "3").monitor, "DP-3");
  EXPECT_EQ(out(*c, "DP-3").active_workspace, "1");
  EXPECT_TRUE(out(*c, "DP-3").focused);
  EXPECT_FALSE(p->data);
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandSettingsManagerTest, EnsurePrimaryShiftsTheLayout) {
  auto c = desk();
  auto p = std::make_shared<memory_persistence_t>();
  settings_manager_t m {c, p, fast()};

  ASSERT_EQ(m.applySettings(cfg("HDMI-A-1", Prep::EnsurePrimary)), ApplyResult::Ok);
  EXPECT_EQ(std::pair(out(*c, "HDMI-A-1").x, out(*c, "HDMI-A-1").y), std::pair(0, 0));
  EXPECT_EQ(std::pair(out(*c, "DP-3").x, out(*c, "DP-3").y), std::pair(-2560, 560));
  EXPECT_TRUE(out(*c, "HDMI-A-1").focused);
  // The primary display is now HDMI-A-1.
  EXPECT_TRUE(m.enumAvailableDevices()[1].m_info->m_primary);

  ASSERT_EQ(m.revertSettings(), RevertResult::Ok);
  EXPECT_EQ(std::pair(out(*c, "HDMI-A-1").x, out(*c, "HDMI-A-1").y), std::pair(2560, -560));
  EXPECT_EQ(std::pair(out(*c, "DP-3").x, out(*c, "DP-3").y), std::pair(0, 0));
  EXPECT_TRUE(out(*c, "DP-3").focused);
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandSettingsManagerTest, EnsureActiveEnablesAndRevertDisablesAgain) {
  auto c = desk();
  c->current.outputs[1].enabled = false;
  auto p = std::make_shared<memory_persistence_t>();
  settings_manager_t m {c, p, fast()};

  // An empty device id means the primary display (the one at the origin).
  ASSERT_EQ(m.applySettings(cfg("", Prep::EnsureActive, Resolution {1920, 1080})), ApplyResult::Ok);
  EXPECT_EQ(out(*c, "DP-3").mode, (output_mode_t {1920, 1080, 240.0}));  // the refresh rate is kept
  EXPECT_FALSE(out(*c, "HDMI-A-1").enabled);

  ASSERT_EQ(m.applySettings(cfg("HDMI-A-1", Prep::EnsureActive)), ApplyResult::Ok);
  EXPECT_TRUE(out(*c, "HDMI-A-1").enabled);
  // A new configuration first undoes the previous one.
  EXPECT_EQ(out(*c, "DP-3").mode, (output_mode_t {3840, 2160, 240.0}));

  ASSERT_EQ(m.revertSettings(), RevertResult::Ok);
  EXPECT_FALSE(out(*c, "HDMI-A-1").enabled);
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandSettingsManagerTest, FailuresChangeNothing) {
  auto c = desk();
  c->current.outputs[1].enabled = false;
  c->current.outputs[0].any_mode = false;
  c->current.outputs[0].modes = {{3840, 2160, 240.0}};
  auto p = std::make_shared<memory_persistence_t>();
  settings_manager_t m {c, p, fast()};

  EXPECT_EQ(m.applySettings(cfg("HDMI-A-1", Prep::VerifyOnly)), ApplyResult::DevicePrepFailed);
  EXPECT_EQ(m.applySettings(cfg("DP-9", Prep::EnsureActive)), ApplyResult::DevicePrepFailed);
  EXPECT_EQ(m.applySettings(cfg("DP-3", Prep::EnsureOnlyDisplay, Resolution {1920, 1080})), ApplyResult::DisplayModePrepFailed);
  EXPECT_TRUE(c->rules.empty());
  EXPECT_FALSE(p->data);
  EXPECT_FALSE(m.state());

  c->reachable = false;
  EXPECT_EQ(m.applySettings(cfg("DP-3", Prep::EnsureActive)), ApplyResult::ApiTemporarilyUnavailable);
  EXPECT_FALSE(p->data);
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandSettingsManagerTest, ModeHyprlandDoesNotTakeIsReportedAndReverted) {
  auto c = desk();
  c->ignore_mode.insert("HDMI-A-1");
  auto p = std::make_shared<memory_persistence_t>();
  settings_manager_t m {c, p, fast()};

  EXPECT_EQ(m.applySettings(cfg("HDMI-A-1", Prep::EnsureOnlyDisplay, Resolution {1920, 1080}, 60.0)), ApplyResult::DisplayModePrepFailed);
  EXPECT_TRUE(out(*c, "DP-3").enabled);
  EXPECT_FALSE(p->data);
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandSettingsManagerTest, RejectedRuleRevertsWhatWasAlreadySent) {
  auto c = desk();
  c->delay_polls = 2;
  c->refuse.insert("DP-3");
  auto p = std::make_shared<memory_persistence_t>();
  settings_manager_t m {c, p, fast()};

  // HDMI-A-1's mode is sent (and lands late), then disabling DP-3 is refused: the mode must still be undone.
  EXPECT_EQ(m.applySettings(cfg("HDMI-A-1", Prep::EnsureOnlyDisplay, Resolution {1920, 1080}, 60.0)), ApplyResult::DevicePrepFailed);
  for (int i = 0; i < 5; ++i) {
    c->layout();  // let anything still queued land
  }
  EXPECT_EQ(out(*c, "HDMI-A-1").mode, (output_mode_t {3840, 2160, 239.991}));
  EXPECT_TRUE(out(*c, "DP-3").enabled);
  EXPECT_FALSE(p->data);
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandSettingsManagerTest, WaitsForAsynchronousChanges) {
  auto c = desk();
  c->delay_polls = 3;
  auto p = std::make_shared<memory_persistence_t>();
  auto options = fast();
  int sleeps = 0;
  options.sleep = [&](auto) {
    ++sleeps;
  };
  settings_manager_t m {c, p, options};

  ASSERT_EQ(m.applySettings(cfg("DP-3", Prep::EnsureOnlyDisplay)), ApplyResult::Ok);
  EXPECT_FALSE(out(*c, "HDMI-A-1").enabled);
  EXPECT_GE(sleeps, 2);
  ASSERT_EQ(m.revertSettings(), RevertResult::Ok);
  EXPECT_TRUE(out(*c, "HDMI-A-1").enabled);
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandSettingsManagerTest, RestoresAfterACrashFromTheSavedState) {
  auto c = desk();
  auto p = std::make_shared<memory_persistence_t>();
  {
    settings_manager_t m {c, p, fast()};
    ASSERT_EQ(m.applySettings(cfg("HDMI-A-1", Prep::EnsureOnlyDisplay, Resolution {1920, 1080}, 60.0)), ApplyResult::Ok);
    // Apollo dies here: no revert.
  }
  ASSERT_FALSE(out(*c, "DP-3").enabled);

  // Hyprland is not up yet when Apollo restarts: revert is retried later.
  settings_manager_t restarted {c, p, fast()};
  ASSERT_TRUE(restarted.state());
  c->reachable = false;
  EXPECT_EQ(restarted.revertSettings(), RevertResult::ApiTemporarilyUnavailable);
  EXPECT_TRUE(p->data);

  c->reachable = true;
  ASSERT_EQ(restarted.revertSettings(), RevertResult::Ok);
  EXPECT_TRUE(out(*c, "DP-3").enabled);
  EXPECT_EQ(out(*c, "HDMI-A-1").mode, (output_mode_t {3840, 2160, 239.991}));
  EXPECT_FALSE(p->data);
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandSettingsManagerTest, UnpluggedMonitorIsRestoredWhenItReturns) {
  auto c = desk();
  auto p = std::make_shared<memory_persistence_t>();
  settings_manager_t m {c, p, fast()};
  ASSERT_EQ(m.applySettings(cfg("HDMI-A-1", Prep::EnsureOnlyDisplay, Resolution {1920, 1080}, 60.0)), ApplyResult::Ok);

  auto dp = out(*c, "DP-3");
  std::erase_if(c->current.outputs, [](const output_t &o) {
    return o.name == "DP-3";
  });
  EXPECT_EQ(m.revertSettings(), RevertResult::TopologyIsInvalid);
  EXPECT_EQ(out(*c, "HDMI-A-1").mode, (output_mode_t {3840, 2160, 239.991}));
  ASSERT_TRUE(p->state());
  EXPECT_EQ(p->state()->touched.size(), 1u);
  EXPECT_TRUE(p->state()->touched.contains("DP-3"));

  c->current.outputs.push_back(dp);
  ASSERT_EQ(m.revertSettings(), RevertResult::Ok);
  EXPECT_TRUE(out(*c, "DP-3").enabled);
  EXPECT_FALSE(p->data);
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandSettingsManagerTest, VirtualDisplayTargetKeepsItsWorkspaceAndHdrGoesThroughVdisplay) {
  auto c = desk();
  c->steal_active = true;
  // Another client's virtual display, and this session's (activated: focused, showing its workspace).
  c->current.outputs.push_back(output("APOLLO-99999999", {1920, 1080, 60.0}, 9000, 0, "apollo-99999999"));
  c->current.outputs.push_back(output("APOLLO-1a2b3c4d", {2560, 1600, 120.0}, 5000, 5000, "apollo-1a2b3c4d"));
  c->current.outputs[0].focused = false;
  c->current.outputs.back().focused = true;
  c->current.workspaces.push_back({"apollo-1a2b3c4d", "APOLLO-1a2b3c4d"});
  c->vhdr["APOLLO-1a2b3c4d"] = false;
  auto p = std::make_shared<memory_persistence_t>();
  settings_manager_t m {c, p, fast()};

  ASSERT_EQ(m.applySettings(cfg("APOLLO-1a2b3c4d", Prep::EnsureOnlyDisplay, Resolution {2560, 1600}, 120.0, HdrState::Enabled)), ApplyResult::Ok);
  EXPECT_FALSE(out(*c, "DP-3").enabled);
  EXPECT_FALSE(out(*c, "HDMI-A-1").enabled);
  EXPECT_TRUE(out(*c, "APOLLO-99999999").enabled);
  EXPECT_TRUE(c->vhdr["APOLLO-1a2b3c4d"]);
  EXPECT_EQ(out(*c, "APOLLO-1a2b3c4d").active_workspace, "apollo-1a2b3c4d");
  EXPECT_TRUE(out(*c, "APOLLO-1a2b3c4d").focused);
  EXPECT_EQ(std::ranges::count_if(c->rules, [](const monitor_rule_t &r) {
              return r.output == "APOLLO-1a2b3c4d";
            }),
            0);

  // The session ends: the virtual display may already be gone when the revert runs.
  std::erase_if(c->current.outputs, [](const output_t &o) {
    return o.name == "APOLLO-1a2b3c4d";
  });
  c->calls.clear();
  ASSERT_EQ(m.revertSettings(), RevertResult::Ok);
  EXPECT_TRUE(out(*c, "DP-3").enabled);
  EXPECT_TRUE(out(*c, "HDMI-A-1").enabled);
  EXPECT_EQ(ws(*c, "1").monitor, "DP-3");
  EXPECT_EQ(ws(*c, "2").monitor, "HDMI-A-1");
  EXPECT_EQ(std::ranges::count(c->calls, "vhdr APOLLO-1a2b3c4d 0"), 0);
  EXPECT_FALSE(p->data);
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandSettingsManagerTest, PhysicalHdrUsesColourManagement) {
  auto c = desk();
  c->current.outputs[0].hdr_capable = true;
  auto p = std::make_shared<memory_persistence_t>();
  settings_manager_t m {c, p, fast()};

  ASSERT_EQ(m.applySettings(cfg("DP-3", Prep::VerifyOnly, {}, {}, HdrState::Enabled)), ApplyResult::Ok);
  EXPECT_EQ(out(*c, "DP-3").cm, "hdr");
  EXPECT_EQ(out(*c, "DP-3").bitdepth, 10);
  EXPECT_EQ(m.enumAvailableDevices()[0].m_info->m_hdr_state, HdrState::Enabled);
  ASSERT_EQ(m.revertSettings(), RevertResult::Ok);
  EXPECT_EQ(out(*c, "DP-3").cm, "srgb");
  EXPECT_EQ(out(*c, "DP-3").bitdepth, 8);

  // HDMI-A-1 cannot do HDR: left alone, and nothing to restore.
  c->rules.clear();
  ASSERT_EQ(m.applySettings(cfg("HDMI-A-1", Prep::VerifyOnly, {}, {}, HdrState::Enabled)), ApplyResult::Ok);
  EXPECT_TRUE(c->rules.empty());
  EXPECT_FALSE(p->data);
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandSettingsManagerTest, PhysicalMonitorAlreadyInHdrKeepsItsPreset) {
  // The user runs DP-3 with the EDID-based HDR preset (and a bit depth Apollo would not pick itself).
  auto c = desk();
  c->current.outputs[0].hdr_capable = true;
  c->current.outputs[0].cm = "hdredid";
  c->current.outputs[0].bitdepth = 8;
  auto p = std::make_shared<memory_persistence_t>();
  settings_manager_t m {c, p, fast()};

  // HDR is already on: nothing to change and nothing to restore.
  ASSERT_EQ(m.applySettings(cfg("DP-3", Prep::VerifyOnly, {}, {}, HdrState::Enabled)), ApplyResult::Ok);
  EXPECT_TRUE(c->rules.empty());
  EXPECT_EQ(out(*c, "DP-3").cm, "hdredid");
  EXPECT_EQ(out(*c, "DP-3").bitdepth, 8);
  EXPECT_FALSE(p->data);

  // Switching it off for an SDR client is restored afterwards.
  ASSERT_EQ(m.applySettings(cfg("DP-3", Prep::VerifyOnly, {}, {}, HdrState::Disabled)), ApplyResult::Ok);
  EXPECT_EQ(out(*c, "DP-3").cm, "srgb");
  ASSERT_EQ(m.revertSettings(), RevertResult::Ok);
  EXPECT_EQ(out(*c, "DP-3").cm, "hdredid");
  EXPECT_EQ(out(*c, "DP-3").bitdepth, 8);
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandSettingsManagerTest, ResetForgetsTheSavedState) {
  auto c = desk();
  auto p = std::make_shared<memory_persistence_t>();
  settings_manager_t m {c, p, fast()};
  ASSERT_EQ(m.applySettings(cfg("DP-3", Prep::EnsureOnlyDisplay)), ApplyResult::Ok);
  ASSERT_TRUE(p->data);
  EXPECT_TRUE(m.resetPersistence());
  EXPECT_FALSE(p->data);
  EXPECT_EQ(m.revertSettings(), RevertResult::Ok);
  EXPECT_FALSE(out(*c, "HDMI-A-1").enabled);  // the user accepted the layout as it is
  EXPECT_TRUE(m.resetPersistence());
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandSettingsManagerTest, StateRoundTripsThroughJson) {
  state_t s;
  saved_output_t a;
  a.name = "DP-3";
  a.description = "Samsung";
  a.mode = {3840, 2160, 239.991};
  a.x = -10;
  a.y = 20;
  a.cm = "hdr";
  a.bitdepth = 10;
  a.active_workspace = "1";
  saved_output_t b;
  b.name = "APOLLO-1a2b3c4d";
  b.mode = {1280, 800, 60.0};
  b.enabled = false;
  b.is_virtual = true;
  b.virtual_hdr = true;
  s.initial = {a, b};
  s.touched["DP-3"] = {.enabled = true, .hdr = true};
  s.workspaces = {{"1", "DP-3"}};
  s.focused = "DP-3";
  s.focus_changed = true;
  EXPECT_EQ(state_from_json(state_to_json(s)), s);
  EXPECT_FALSE(state_from_json(nlohmann::json::parse(R"({"version": 99, "initial": []})")));
  EXPECT_FALSE(state_from_json(nlohmann::json::parse(R"({"version": 1, "initial": [{"name": "X"}]})")));

  // A corrupt file is ignored rather than blocking the stream.
  auto p = std::make_shared<memory_persistence_t>();
  p->data = std::vector<std::uint8_t> {'{', 'x'};
  settings_manager_t m {desk(), p, fast()};
  EXPECT_FALSE(m.state());
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandSettingsManagerTest, HyprlangRulesCarryEveryField) {
  layout_t current;
  current.outputs.push_back(output("DP-3", {3840, 2160, 240.0}, 0, 0, "1"));
  current.outputs[0].scale = 1.5;
  current.outputs[0].transform = 1;
  plan_t plan;
  plan["DP-3"].position = std::pair {100, 0};

  const auto lua = rules_for(plan, current, true);
  ASSERT_EQ(lua.size(), 1u);
  EXPECT_FALSE(lua[0].mode);
  EXPECT_FALSE(lua[0].scale);
  EXPECT_EQ(lua[0].position, "100x0");

  const auto legacy = rules_for(plan, current, false);
  ASSERT_EQ(legacy.size(), 1u);
  EXPECT_EQ(legacy[0].mode, "3840x2160@240.000");
  EXPECT_EQ(legacy[0].scale, 1.5);
  EXPECT_EQ(legacy[0].transform, 1);
  EXPECT_EQ(legacy[0].bitdepth, 8);
  // Without it Hyprland would reset the preset, and a position-only change would not restore it.
  EXPECT_EQ(legacy[0].cm, "srgb");
  current.outputs[0].cm = "hdredid";
  EXPECT_EQ(rules_for(plan, current, false).at(0).cm, "hdredid");
  EXPECT_FALSE(rules_for(plan, current, true).at(0).cm);
  // A planned preset wins over the current one.
  plan["DP-3"].cm = "hdr";
  plan["DP-3"].bitdepth = 10;
  EXPECT_EQ(rules_for(plan, current, false).at(0).cm, "hdr");
  EXPECT_EQ(rules_for(plan, current, false).at(0).bitdepth, 10);
  plan["DP-3"].cm.reset();
  plan["DP-3"].bitdepth.reset();

  // Nothing to change: no rule.
  plan["DP-3"].position = std::pair {0, 0};
  EXPECT_TRUE(rules_for(plan, current, true).empty());
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandSettingsManagerTest, RejectedHyprlangRuleIsReportedByWhatItChanges) {
  // Hyprlang rules carry the output's mode and preset as well as the change.
  auto c = desk();
  c->merges = false;
  c->refuse.insert("DP-3");
  auto p = std::make_shared<memory_persistence_t>();
  settings_manager_t m {c, p, fast()};
  EXPECT_EQ(m.applySettings(cfg("DP-3", Prep::VerifyOnly, Resolution {1920, 1080})), ApplyResult::DisplayModePrepFailed);

  c->refuse = {"HDMI-A-1"};
  EXPECT_EQ(m.applySettings(cfg("HDMI-A-1", Prep::EnsurePrimary)), ApplyResult::PrimaryDevicePrepFailed);
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandSettingsManagerTest, MonitorRulesCarryColourManagement) {
  monitor_rule_t rule;
  rule.output = "DP-3";
  rule.cm = "hdr";
  rule.bitdepth = 10;
  EXPECT_EQ(lua_monitor_call(rule), R"(hl.monitor({ output = "DP-3", bitdepth = 10, cm = "hdr" }))");
  EXPECT_EQ(hyprlang_monitor_value(rule), "DP-3,preferred,auto,auto,bitdepth,10,cm,hdr");
}

#endif
