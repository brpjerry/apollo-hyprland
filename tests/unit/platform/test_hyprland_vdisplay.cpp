/**
 * @file tests/unit/platform/test_hyprland_vdisplay.cpp
 * @brief Test the Hyprland virtual display backend (src/platform/linux/hyprland/vdisplay.*) against a fake Hyprland.
 */
#include "../../tests_common.h"

#ifdef SUNSHINE_BUILD_WAYLAND

  // standard includes
  #include <algorithm>
  #include <atomic>
  #include <format>
  #include <fstream>
  #include <map>
  #include <mutex>
  #include <regex>

  // platform includes
  #include <sys/wait.h>
  #include <unistd.h>

  // local includes
  #include "hyprland_fakes.h"
  #include <src/platform/linux/hyprland/vdisplay.h>

namespace {
  namespace fs = std::filesystem;
  using namespace std::chrono_literals;
  using namespace hyprland_fakes;
  using platf::vdisplay::mode_t;
  using platf::vdisplay::status_e;

  constexpr auto client_uuid = "1A2B3C4D-0000-4000-8000-00000000BEEF";
  constexpr auto client_output = "APOLLO-1a2b3c4d";

  struct fake_monitor_t {
    std::string name;
    int width;
    int height;
    double refresh;
    int x = 0;
    int y = 0;
    double scale = 1.0;
    int transform = 0;
    bool disabled = false;
    bool dpms = true;
    std::string cm = "srgb";  ///< colorManagementPreset
    bool supports_hdr = false;  ///< A rule forced HDR support on (headless outputs have no EDID).
    std::string format = "XRGB8888";  ///< currentFormat, follows `bitdepth` like on a DRM-backed session.
  };

  /**
   * @brief A fake Hyprland instance: request and event sockets plus a list of monitors that
   * `output create/remove` and `hl.monitor()` change, emitting the events Hyprland 0.57 emits.
   */
  class fake_hyprland_t {
  public:
    static constexpr auto signature = "sig_1_1";

    explicit fake_hyprland_t(std::vector<fake_monitor_t> monitors = {{"DP-1", 2560, 1440, 60.0}}):
        dir_ {make_dir(tmp_.path())},
        monitors_ {std::move(monitors)},
        events_ {dir_ / ".socket2.sock"},
        requests_ {dir_ / ".socket.sock", [this](const std::string &req) {
                     return handle(req);
                   }} {
    }

    /// Options pointing the backend at this fake, with short timeouts.
    hyprland::vdisplay_t::options_t options() {
      hyprland::vdisplay_t::options_t o;
      o.connect = [this]() {
        return hyprland::ipc_t::connect(tmp_.path(), signature);
      };
      o.instance_exists = []() {
        return true;
      };
      o.output_visible = [](const std::string &) -> std::optional<bool> {
        return true;
      };
      o.state_dir = tmp_.path() / "state";
      o.settings_file = settings_file();
      o.timeout = 500ms;
      o.poll_interval = 10ms;
      return o;
    }

    fs::path state_dir() const {
      return tmp_.path() / "state";
    }

    fs::path settings_file() const {
      return tmp_.path() / "vdisplay_settings.json";
    }

    std::vector<std::string> requests() {
      return requests_.requests();
    }

    std::size_t count_requests(std::string_view prefix) {
      auto reqs = requests();
      return std::ranges::count_if(reqs, [&](const std::string &r) {
        return r.starts_with(prefix);
      });
    }

    std::optional<fake_monitor_t> monitor(const std::string &name) {
      std::lock_guard lock {mutex_};
      for (auto &m : monitors_) {
        if (m.name == name) {
          return m;
        }
      }
      return std::nullopt;
    }

    /// Change a monitor in place, like `dpms off` or disabling it in the config would.
    void update_monitor(const std::string &name, const std::function<void(fake_monitor_t &)> &change) {
      std::lock_guard lock {mutex_};
      for (auto &m : monitors_) {
        if (m.name == name) {
          change(m);
        }
      }
    }

    /// Like `hyprctl reload`: runtime rules are dropped and the output falls back to its default mode.
    void reload_config() {
      {
        std::lock_guard lock {mutex_};
        for (auto &m : monitors_) {
          if (m.name.starts_with("APOLLO-")) {
            m.width = 1920;
            m.height = 1080;
            m.refresh = 60.0;
          }
        }
      }
      events_.send("configreloaded>>\n");
    }

    void wait_for_listener() {
      ASSERT_TRUE(events_.wait_for_connections(1));
    }

    std::atomic<bool> emit_events {true};  ///< Send monitoraddedv2/monitorremovedv2.
    std::atomic<bool> apply_rules {true};  ///< Apply hl.monitor() rules to the monitor list.

  private:
    static fs::path make_dir(const fs::path &base) {
      auto dir = base / signature;
      fs::create_directories(dir);
      return dir;
    }

    int right_edge_locked(const std::string &except) {
      int right = 0;
      for (auto &m : monitors_) {
        if (m.name != except) {
          right = std::max(right, m.x + static_cast<int>(m.width / m.scale));
        }
      }
      return right;
    }

    std::string handle(const std::string &req) {
      static const std::regex create_re {"^/output create headless (\\S+)$"};
      static const std::regex remove_re {"^/output remove (\\S+)$"};
      static const std::regex output_re {"output = \"([^\"]+)\""};
      static const std::regex mode_re {"mode = \"(\\d+)x(\\d+)@([0-9.]+)\""};
      static const std::regex position_re {"position = \"([^\"]+)\""};
      static const std::regex cm_re {"cm = \"([^\"]+)\""};
      static const std::regex supports_hdr_re {"supports_hdr = (-?\\d+)"};
      static const std::regex bitdepth_re {"bitdepth = (\\d+)"};
      static const std::regex scale_re {"scale = ([0-9.]+)"};
      static const std::regex transform_re {"transform = ([0-7])"};
      std::smatch match;

      if (req == "j/status") {
        return R"({"configProvider": "lua", "backend": "drm"})";
      }
      if (req == "j/version") {
        return R"({"tag": "v0.57.2", "version": "0.57.2"})";
      }
      if (req == "j/monitors all") {
        std::lock_guard lock {mutex_};
        nlohmann::json arr = nlohmann::json::array();
        int id = 0;
        for (auto &m : monitors_) {
          arr.push_back({{"id", id++}, {"name", m.name}, {"description", ""}, {"width", m.width}, {"height", m.height}, {"refreshRate", m.refresh}, {"x", m.x}, {"y", m.y}, {"scale", m.scale}, {"transform", m.transform}, {"disabled", m.disabled}, {"dpmsStatus", m.dpms}, {"colorManagementPreset", m.cm}, {"currentFormat", m.format}});
        }
        return arr.dump();
      }
      if (std::regex_match(req, match, create_re)) {
        const std::string name = match[1];
        {
          std::lock_guard lock {mutex_};
          if (std::ranges::any_of(monitors_, [&](auto &m) {
                return m.name == name;
              })) {
            return "Name already taken";
          }
          monitors_.push_back({name, 1920, 1080, 60.0, right_edge_locked(name), 0});
        }
        if (emit_events) {
          events_.send(std::format("monitoradded>>{}\nmonitoraddedv2>>7,{},\n", name, name));
        }
        return "ok";
      }
      if (std::regex_match(req, match, remove_re)) {
        const std::string name = match[1];
        {
          std::lock_guard lock {mutex_};
          auto it = std::ranges::find_if(monitors_, [&](auto &m) {
            return m.name == name;
          });
          if (it == monitors_.end()) {
            return "output not found";
          }
          monitors_.erase(it);
        }
        if (emit_events) {
          events_.send(std::format("monitorremoved>>{}\nmonitorremovedv2>>7,{},\n", name, name));
        }
        return "ok";
      }
      if (req.starts_with("/eval hl.monitor(")) {
        if (!std::regex_search(req, match, output_re)) {
          return "error: no output";
        }
        const std::string name = match[1];
        if (!apply_rules) {
          return "ok";
        }
        std::lock_guard lock {mutex_};
        auto it = std::ranges::find_if(monitors_, [&](auto &m) {
          return m.name == name;
        });
        if (it == monitors_.end()) {
          return "ok";  // a rule for an output that does not exist is kept for later
        }
        if (std::regex_search(req, match, mode_re)) {
          it->width = std::stoi(match[1]);
          it->height = std::stoi(match[2]);
          it->refresh = std::stod(match[3]);
        }
        if (std::regex_search(req, match, scale_re)) {
          it->scale = std::stod(match[1]);
        }
        if (std::regex_search(req, match, transform_re)) {
          it->transform = std::stoi(match[1]);
        }
        if (std::regex_search(req, match, position_re)) {
          const std::string pos = match[1];
          int x;
          int y;
          if (pos == "auto-right") {
            it->x = right_edge_locked(name);
            it->y = 0;
          } else if (std::sscanf(pos.c_str(), "%dx%d", &x, &y) == 2) {
            it->x = x;
            it->y = y;
          }
        }
        if (std::regex_search(req, match, bitdepth_re)) {
          it->format = match[1] == "10" ? "XBGR2101010" : "XRGB8888";
        }
        if (std::regex_search(req, match, supports_hdr_re)) {
          it->supports_hdr = std::stoi(match[1]) == 1;
        }
        if (std::regex_search(req, match, cm_re)) {
          // Like Hyprland 0.57: an output that does not support HDR stays SDR.
          it->cm = match[1] == "hdr" && !it->supports_hdr ? "srgb" : std::string {match[1]};
        }
        return "ok";
      }
      return "unknown request";
    }

    temp_dir_t tmp_;
    fs::path dir_;
    std::mutex mutex_;
    std::vector<fake_monitor_t> monitors_;
    fake_event_socket_t events_;
    fake_request_socket_t requests_;
  };

  uuid_util::uuid_t client_identity() {
    return uuid_util::uuid_t::parse(client_uuid);
  }

  /// A pid that belonged to a process that has exited.
  int dead_pid() {
    pid_t pid = ::fork();
    if (pid == 0) {
      ::_exit(0);
    }
    ::waitpid(pid, nullptr, 0);
    return pid;
  }

  void write_owner(const fs::path &dir, const std::string &name, int pid) {
    fs::create_directories(dir);
    std::ofstream {dir / name} << pid << '\n';
  }

  template<class F>
  bool eventually(F &&cond, std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (cond()) {
        return true;
      }
      std::this_thread::sleep_for(10ms);
    }
    return cond();
  }
}  // namespace

// Bugs caught: 1, False positives: 0
TEST(HyprlandVdisplayNamingTest, SameIdentitySameLowercaseName) {
  EXPECT_EQ(hyprland::vdisplay_output_name(client_identity()), client_output);
  EXPECT_EQ(hyprland::vdisplay_output_name(uuid_util::uuid_t::parse("1a2b3c4d-ffff-4fff-8fff-ffffffffffff")), client_output);
  EXPECT_EQ(hyprland::vdisplay_output_name(uuid_util::uuid_t::parse("00000000-0000-0000-0000-000000000001")), "APOLLO-00000000");

  EXPECT_TRUE(hyprland::is_vdisplay_output_name("APOLLO-1a2b3c4d"));
  EXPECT_TRUE(hyprland::is_vdisplay_output_name("APOLLO-00000000"));
  EXPECT_FALSE(hyprland::is_vdisplay_output_name("APOLLO-TEST-1a2b3c4d"));
  EXPECT_FALSE(hyprland::is_vdisplay_output_name("APOLLO-TEST-h06a"));
  EXPECT_FALSE(hyprland::is_vdisplay_output_name("APOLLO-1A2B3C4D"));
  EXPECT_FALSE(hyprland::is_vdisplay_output_name("APOLLO-1a2b3c4"));
  EXPECT_FALSE(hyprland::is_vdisplay_output_name("APOLLO-1a2b3c4d5"));
  EXPECT_FALSE(hyprland::is_vdisplay_output_name("HEADLESS-2"));
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandVdisplayNamingTest, ModeStringHasThreeDecimals) {
  EXPECT_EQ(hyprland::vdisplay_mode_string({2560, 1600, 120000}), "2560x1600@120.000");
  EXPECT_EQ(hyprland::vdisplay_mode_string({2560, 1600, 119880}), "2560x1600@119.880");
  EXPECT_EQ(hyprland::vdisplay_mode_string({1920, 1080, 59940}), "1920x1080@59.940");
  EXPECT_EQ(hyprland::vdisplay_mode_string({800, 600, 0}), "800x600@60.000");

  hyprland::monitor_info_t m;
  m.width = 2560;
  m.height = 1600;
  m.refresh_hz = 119.88;
  EXPECT_TRUE(hyprland::monitor_has_mode(m, {2560, 1600, 119880}));
  // Hyprland may snap to a listed mode within 1 Hz.
  EXPECT_TRUE(hyprland::monitor_has_mode(m, {2560, 1600, 120000}));
  EXPECT_FALSE(hyprland::monitor_has_mode(m, {2560, 1600, 90000}));
  EXPECT_FALSE(hyprland::monitor_has_mode(m, {2560, 1440, 119880}));
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandVdisplayNamingTest, IsolatedPositionClearsTheLogicalLayout) {
  // The live layout: DP-3 4K at scale 1.5, HDMI-A-1 4K at scale 1.5 rotated, to the right and 560 px higher.
  std::vector<hyprland::monitor_info_t> monitors(4);
  monitors[0] = {.name = "DP-3", .width = 3840, .height = 2160, .x = 0, .y = 0, .scale = 1.5};
  monitors[1] = {.name = "HDMI-A-1", .width = 3840, .height = 2160, .x = 2560, .y = -560, .scale = 1.5, .transform = 1};
  monitors[2] = {.name = "APOLLO-1a2b3c4d", .width = 2560, .height = 1600, .x = 4000, .y = 0};
  monitors[3] = {.name = "DP-9", .width = 9999, .height = 9999, .x = 0, .y = 0, .disabled = true};
  // Right edge 2560 + 2160/1.5 = 4000, bottom edge -560 + 3840/1.5 = 2000.
  EXPECT_EQ(hyprland::isolated_position(monitors, "APOLLO-1a2b3c4d", 1000), "5000x3000");
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandVdisplayNamingTest, ActiveMonitorIsEnabledAndSwitchedOn) {
  auto monitor = [](std::string name, bool disabled, bool dpms_on) {
    hyprland::monitor_info_t m;
    m.name = std::move(name);
    m.width = 3840;
    m.height = 2160;
    m.disabled = disabled;
    m.dpms_on = dpms_on;
    return m;
  };

  EXPECT_TRUE(hyprland::monitor_is_active(monitor("DP-3", false, true)));
  EXPECT_FALSE(hyprland::monitor_is_active(monitor("DP-3", true, true)));
  // DPMS-off outputs capture as solid black on Hyprland.
  EXPECT_FALSE(hyprland::monitor_is_active(monitor("DP-3", false, false)));
  EXPECT_FALSE(hyprland::monitor_is_active(monitor("FALLBACK", false, true)));
  // Headless outputs show the desktop like physical ones; Apollo's probe display must count, or probing
  // on it would be refused too.
  EXPECT_TRUE(hyprland::monitor_is_active(monitor("HEADLESS-2", false, true)));
  EXPECT_TRUE(hyprland::monitor_is_active(monitor(client_output, false, true)));

  EXPECT_FALSE(hyprland::any_monitor_active({}));
  EXPECT_FALSE(hyprland::any_monitor_active({monitor("DP-3", true, true), monitor("HDMI-A-1", false, false), monitor("FALLBACK", false, true)}));
  EXPECT_TRUE(hyprland::any_monitor_active({monitor("DP-3", true, true), monitor("HDMI-A-1", false, true)}));
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandVdisplayTest, AnyMonitorActiveFollowsHyprland) {
  fake_hyprland_t hypr {{{"DP-1", 2560, 1440, 60.0}, {"HDMI-A-1", 1920, 1080, 60.0, 2560, 0}}};
  hyprland::vdisplay_t vd {hypr.options()};
  ASSERT_EQ(vd.init(), status_e::ok);
  hypr.wait_for_listener();

  EXPECT_EQ(vd.any_monitor_active(), std::optional<bool> {true});

  hypr.update_monitor("DP-1", [](auto &m) {
    m.disabled = true;
  });
  EXPECT_EQ(vd.any_monitor_active(), std::optional<bool> {true});

  hypr.update_monitor("HDMI-A-1", [](auto &m) {
    m.dpms = false;
  });
  EXPECT_EQ(vd.any_monitor_active(), std::optional<bool> {false});

  // The temporary probe display (or a session's virtual display) makes a display active again.
  ASSERT_EQ(vd.create(client_identity(), "Probe", {800, 600, 60000}), client_output);
  EXPECT_EQ(vd.any_monitor_active(), std::optional<bool> {true});
  EXPECT_TRUE(vd.remove(client_identity()));
  EXPECT_EQ(vd.any_monitor_active(), std::optional<bool> {false});
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandVdisplayTest, AnyMonitorActiveUnknownWithoutHyprland) {
  auto options = hyprland::vdisplay_t::default_options();
  options.connect = []() {
    return std::unique_ptr<hyprland::ipc_t> {};
  };
  options.instance_exists = []() {
    return false;
  };
  options.state_dir.clear();
  hyprland::vdisplay_t vd {options};

  EXPECT_EQ(vd.init(), status_e::unavailable);
  EXPECT_EQ(vd.any_monitor_active(), std::nullopt);
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandVdisplayTest, CreateAppliesModeAndRemoveCleansUp) {
  fake_hyprland_t hypr;
  std::vector<std::string> visible_checks;
  auto options = hypr.options();
  options.output_visible = [&](const std::string &name) -> std::optional<bool> {
    visible_checks.push_back(name);
    return true;
  };
  hyprland::vdisplay_t vd {options};

  EXPECT_EQ(vd.status(), status_e::unknown);
  ASSERT_EQ(vd.init(), status_e::ok);
  EXPECT_EQ(vd.status(), status_e::ok);
  hypr.wait_for_listener();

  ASSERT_EQ(vd.create(client_identity(), "Living Room", {2560, 1600, 120000}), client_output);

  EXPECT_EQ(hypr.count_requests("/output create headless APOLLO-1a2b3c4d"), 1u);
  EXPECT_EQ(hypr.count_requests(R"(/eval hl.monitor({ output = "APOLLO-1a2b3c4d", mode = "2560x1600@120.000", position = "auto-right", scale = 1 }))"), 1u);
  auto m = hypr.monitor(client_output);
  ASSERT_TRUE(m);
  EXPECT_EQ(m->width, 2560);
  EXPECT_EQ(m->height, 1600);
  EXPECT_DOUBLE_EQ(m->refresh, 120.0);
  EXPECT_FALSE(visible_checks.empty());
  EXPECT_EQ(visible_checks.front(), client_output);
  EXPECT_EQ(vd.owned_outputs(), std::vector<std::string> {client_output});

  // The owner file records this process, for crash cleanup.
  std::ifstream owner {hypr.state_dir() / client_output};
  int pid = 0;
  owner >> pid;
  EXPECT_EQ(pid, ::getpid());

  // process.cpp applies the same mode right after create(); that must not re-send the rule.
  EXPECT_TRUE(vd.set_mode(client_output, {2560, 1600, 120000}, false));
  EXPECT_EQ(hypr.count_requests("/eval hl.monitor("), 1u);

  EXPECT_TRUE(vd.remove(client_identity()));
  EXPECT_FALSE(hypr.monitor(client_output));
  EXPECT_EQ(hypr.count_requests("/output remove APOLLO-1a2b3c4d"), 1u);
  EXPECT_TRUE(vd.owned_outputs().empty());
  EXPECT_FALSE(fs::exists(hypr.state_dir() / client_output));

  // Nothing left to remove.
  EXPECT_FALSE(vd.remove(client_identity()));
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandVdisplayTest, CreateWaitsForTheWaylandOutput) {
  fake_hyprland_t hypr;
  std::atomic<int> checks {0};
  auto options = hypr.options();
  options.output_visible = [&](const std::string &) -> std::optional<bool> {
    return ++checks >= 5;
  };
  hyprland::vdisplay_t vd {options};
  ASSERT_EQ(vd.init(), status_e::ok);
  hypr.wait_for_listener();

  ASSERT_EQ(vd.create(client_identity(), "Living Room", {1280, 720, 60000}), client_output);
  EXPECT_GE(checks.load(), 5);
  EXPECT_TRUE(vd.remove(client_identity()));
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandVdisplayTest, CreateFailsAndCleansUpWhenWaylandNeverSeesTheOutput) {
  fake_hyprland_t hypr;
  auto options = hypr.options();
  options.output_visible = [](const std::string &) -> std::optional<bool> {
    return false;
  };
  hyprland::vdisplay_t vd {options};
  ASSERT_EQ(vd.init(), status_e::ok);
  hypr.wait_for_listener();

  EXPECT_EQ(vd.create(client_identity(), "Living Room", {1280, 720, 60000}), "");
  EXPECT_FALSE(hypr.monitor(client_output));
  EXPECT_EQ(hypr.count_requests("/output remove APOLLO-1a2b3c4d"), 1u);
  EXPECT_TRUE(vd.owned_outputs().empty());
  EXPECT_FALSE(fs::exists(hypr.state_dir() / client_output));
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandVdisplayTest, CreateFailsAndCleansUpWhenTheModeIsNeverApplied) {
  fake_hyprland_t hypr;
  hypr.apply_rules = false;
  hyprland::vdisplay_t vd {hypr.options()};
  ASSERT_EQ(vd.init(), status_e::ok);
  hypr.wait_for_listener();

  EXPECT_EQ(vd.create(client_identity(), "Living Room", {2560, 1600, 120000}), "");
  EXPECT_FALSE(hypr.monitor(client_output));
  EXPECT_EQ(hypr.count_requests("/output remove APOLLO-1a2b3c4d"), 1u);
  EXPECT_TRUE(vd.owned_outputs().empty());
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandVdisplayTest, CreateFailsWithoutMonitorAddedEvent) {
  fake_hyprland_t hypr;
  hypr.emit_events = false;
  hyprland::vdisplay_t vd {hypr.options()};
  ASSERT_EQ(vd.init(), status_e::ok);
  hypr.wait_for_listener();

  EXPECT_EQ(vd.create(client_identity(), "Living Room", {2560, 1600, 120000}), "");
  // No monitorremovedv2 either, but the output is gone from j/monitors, which counts as removed.
  EXPECT_FALSE(hypr.monitor(client_output));
  EXPECT_TRUE(vd.owned_outputs().empty());
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandVdisplayTest, ReusesItsOwnOutputButReplacesLeftovers) {
  fake_hyprland_t hypr {{{"DP-1", 2560, 1440, 60.0}, {client_output, 1920, 1080, 60.0, 2560, 0}}};
  hyprland::vdisplay_t vd {hypr.options()};
  // The leftover has an owner file naming a running process (pid 1), so init()'s stale cleanup leaves it for create().
  write_owner(hypr.state_dir(), client_output, ::getpid() == 1 ? 2 : 1);
  ASSERT_EQ(vd.init(), status_e::ok);
  hypr.wait_for_listener();
  ASSERT_TRUE(hypr.monitor(client_output)) << "pid 1 is alive, so the output must not count as stale";

  // Not created by this backend: removed and created afresh.
  ASSERT_EQ(vd.create(client_identity(), "Living Room", {1280, 720, 60000}), client_output);
  auto reqs = hypr.requests();
  auto remove_it = std::ranges::find(reqs, "/output remove APOLLO-1a2b3c4d");
  auto create_it = std::ranges::find(reqs, "/output create headless APOLLO-1a2b3c4d");
  ASSERT_NE(remove_it, reqs.end());
  ASSERT_NE(create_it, reqs.end());
  EXPECT_LT(remove_it, create_it);

  // Created by this backend: reused with the new mode.
  ASSERT_EQ(vd.create(client_identity(), "Living Room", {1920, 1200, 90000}), client_output);
  EXPECT_EQ(hypr.count_requests("/output create headless"), 1u);
  auto m = hypr.monitor(client_output);
  ASSERT_TRUE(m);
  EXPECT_EQ(m->width, 1920);
  EXPECT_EQ(m->height, 1200);
  EXPECT_DOUBLE_EQ(m->refresh, 90.0);

  EXPECT_TRUE(vd.remove(client_identity()));
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandVdisplayTest, ConfigReloadReappliesTheMode) {
  fake_hyprland_t hypr;
  hyprland::vdisplay_t vd {hypr.options()};
  ASSERT_EQ(vd.init(), status_e::ok);
  hypr.wait_for_listener();
  ASSERT_EQ(vd.create(client_identity(), "Living Room", {2560, 1600, 120000}), client_output);
  ASSERT_TRUE(vd.set_mode(client_output, {2560, 1600, 120000}, true));
  const auto isolated = hypr.monitor(client_output);
  ASSERT_TRUE(isolated);
  EXPECT_EQ(isolated->x, 2560 + 1000);  // DP-1's right edge plus the gap
  EXPECT_EQ(isolated->y, 1440 + 1000);

  hypr.reload_config();

  EXPECT_TRUE(eventually([&]() {
    auto m = hypr.monitor(client_output);
    return m && m->width == 2560 && m->height == 1600 && m->refresh == 120.0;
  })) << "the rule was not re-applied after configreloaded";
  // The isolated position is part of the re-applied rule.
  auto m = hypr.monitor(client_output);
  ASSERT_TRUE(m);
  EXPECT_EQ(m->x, isolated->x);
  EXPECT_EQ(m->y, isolated->y);

  EXPECT_TRUE(vd.remove(client_identity()));

  // After removal a reload re-applies nothing.
  const auto evals = hypr.count_requests("/eval hl.monitor(");
  hypr.reload_config();
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(hypr.count_requests("/eval hl.monitor("), evals);
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandVdisplayTest, CleanupStaleRemovesOnlyOrphanedApolloOutputs) {
  fake_hyprland_t hypr {{
    {"DP-1", 2560, 1440, 60.0},
    {"HEADLESS-2", 1920, 1080, 60.0, 2560},
    {"APOLLO-TEST-12345678", 1920, 1080, 60.0, 4480},  // a test's output: not ours
    {"APOLLO-deadbeef", 1920, 1080, 60.0, 6400},  // no owner file: stale
    {"APOLLO-0badf00d", 1920, 1080, 60.0, 8320},  // owner exited: stale
    {"APOLLO-cafebabe", 1920, 1080, 60.0, 10240},  // owner still running: kept
  }};
  write_owner(hypr.state_dir(), "APOLLO-0badf00d", dead_pid());
  write_owner(hypr.state_dir(), "APOLLO-cafebabe", ::getppid());

  hyprland::vdisplay_t vd {hypr.options()};
  ASSERT_EQ(vd.init(), status_e::ok);  // the first successful init() cleans up

  EXPECT_FALSE(hypr.monitor("APOLLO-deadbeef"));
  EXPECT_FALSE(hypr.monitor("APOLLO-0badf00d"));
  EXPECT_FALSE(fs::exists(hypr.state_dir() / "APOLLO-0badf00d"));
  EXPECT_TRUE(hypr.monitor("APOLLO-cafebabe"));
  EXPECT_TRUE(hypr.monitor("APOLLO-TEST-12345678"));
  EXPECT_TRUE(hypr.monitor("HEADLESS-2"));
  EXPECT_TRUE(hypr.monitor("DP-1"));
  EXPECT_EQ(hypr.count_requests("/output remove"), 2u);

  // Outputs of the running session survive later cleanups (proc::refresh() calls it on every app list reload).
  hypr.wait_for_listener();
  ASSERT_EQ(vd.create(client_identity(), "Living Room", {1280, 720, 60000}), client_output);
  vd.cleanup_stale();
  EXPECT_TRUE(hypr.monitor(client_output));
  EXPECT_TRUE(hypr.monitor("APOLLO-cafebabe"));
  EXPECT_TRUE(vd.remove(client_identity()));
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandVdisplayTest, StatusWithoutHyprland) {
  auto options = hyprland::vdisplay_t::default_options();
  options.connect = []() {
    return std::unique_ptr<hyprland::ipc_t> {};
  };
  bool exists = false;
  options.instance_exists = [&]() {
    return exists;
  };
  options.state_dir.clear();
  hyprland::vdisplay_t vd {options};

  EXPECT_EQ(vd.init(), status_e::unavailable);
  EXPECT_EQ(vd.status(), status_e::unavailable);
  EXPECT_EQ(vd.create(client_identity(), "Living Room", {1280, 720, 60000}), "");
  EXPECT_FALSE(vd.remove(client_identity()));

  exists = true;
  EXPECT_EQ(vd.init(), status_e::failed);
  EXPECT_EQ(vd.status(), status_e::failed);
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandVdisplayTest, SetModeOnlyForOwnOutputs) {
  fake_hyprland_t hypr;
  hyprland::vdisplay_t vd {hypr.options()};
  ASSERT_EQ(vd.init(), status_e::ok);
  EXPECT_FALSE(vd.set_mode("DP-1", {1280, 720, 60000}, false));
  EXPECT_FALSE(vd.set_mode("APOLLO-deadbeef", {1280, 720, 60000}, false));
  EXPECT_EQ(hypr.count_requests("/eval"), 0u);
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandVdisplayTest, RemembersTheScaleTheUserSetAcrossSessions) {
  // HYP-12 acceptance: set scale 2 on a client's virtual display, disconnect, reconnect: scale is 2 again.
  fake_hyprland_t hypr;
  {
    hyprland::vdisplay_t vd {hypr.options()};
    ASSERT_EQ(vd.init(), status_e::ok);
    hypr.wait_for_listener();
    ASSERT_EQ(vd.create(client_identity(), "Living Room", {2560, 1600, 120000}), client_output);
    ASSERT_TRUE(vd.set_mode(client_output, {2560, 1600, 120000}, false));
    EXPECT_FALSE(fs::exists(hypr.settings_file())) << "nothing to remember yet";

    // The user changes the scale during the stream (hyprctl, nwg-displays, ...).
    hypr.update_monitor(client_output, [](fake_monitor_t &m) {
      m.scale = 2.0;
    });
    EXPECT_TRUE(vd.remove(client_identity()));
  }
  ASSERT_TRUE(fs::exists(hypr.settings_file()));
  {
    std::ifstream in {hypr.settings_file()};
    auto json = nlohmann::json::parse(in);
    EXPECT_EQ(json["displays"][client_output]["scale"], 2.0);
    EXPECT_EQ(json["displays"][client_output]["client"], "Living Room");
    EXPECT_FALSE(json["displays"][client_output].contains("position")) << "the position did not change";
  }

  // A new Apollo process (restart), same client.
  hyprland::vdisplay_t vd {hypr.options()};
  ASSERT_EQ(vd.init(), status_e::ok);
  ASSERT_EQ(vd.create(client_identity(), "Living Room", {1920, 1080, 60000}), client_output);
  EXPECT_EQ(hypr.count_requests(R"(/eval hl.monitor({ output = "APOLLO-1a2b3c4d", mode = "1920x1080@60.000", position = "auto-right", scale = 2 }))"), 1u);
  auto m = hypr.monitor(client_output);
  ASSERT_TRUE(m);
  EXPECT_DOUBLE_EQ(m->scale, 2.0);

  // Unchanged this time: the file stays as it was.
  const auto before = fs::last_write_time(hypr.settings_file());
  EXPECT_TRUE(vd.remove(client_identity()));
  EXPECT_EQ(fs::last_write_time(hypr.settings_file()), before);
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandVdisplayTest, ScaleFactorBecomesTheOutputScale) {
  fake_hyprland_t hypr;
  hyprland::vdisplay_t vd {hypr.options()};
  ASSERT_EQ(vd.init(), status_e::ok);
  hypr.wait_for_listener();

  // 1920x1080 at a 200% scale factor renders 3840x2160; at scale 2 the desktop keeps the client's logical size.
  platf::vdisplay::mode_t mode {3840, 2160, 60000, 200};
  ASSERT_EQ(vd.create(client_identity(), "Living Room", mode), client_output);
  ASSERT_TRUE(vd.set_mode(client_output, mode, false));
  auto m = hypr.monitor(client_output);
  ASSERT_TRUE(m);
  EXPECT_DOUBLE_EQ(m->scale, 2.0);
  EXPECT_TRUE(vd.remove(client_identity()));
  EXPECT_FALSE(fs::exists(hypr.settings_file())) << "Apollo's own scale is not a user change";

  // The next session without a scale factor must not inherit scale 2 from the rule Hyprland keeps.
  ASSERT_EQ(vd.create(client_identity(), "Living Room", {1920, 1080, 60000}), client_output);
  m = hypr.monitor(client_output);
  ASSERT_TRUE(m);
  EXPECT_DOUBLE_EQ(m->scale, 1.0);
  EXPECT_TRUE(vd.remove(client_identity()));
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandVdisplayTest, RemembersPositionOnlyWhenNotIsolated) {
  fake_hyprland_t hypr;
  hyprland::vdisplay_t vd {hypr.options()};
  ASSERT_EQ(vd.init(), status_e::ok);
  hypr.wait_for_listener();

  // Isolated: Apollo owns the position; moving it is not remembered.
  ASSERT_EQ(vd.create(client_identity(), "Living Room", {1280, 720, 60000}), client_output);
  ASSERT_TRUE(vd.set_mode(client_output, {1280, 720, 60000}, true));
  hypr.update_monitor(client_output, [](fake_monitor_t &m) {
    m.x = 0;
    m.y = -720;
  });
  EXPECT_TRUE(vd.remove(client_identity()));
  EXPECT_FALSE(fs::exists(hypr.settings_file()));

  // Not isolated: the user put it above DP-1, and it goes there next time.
  ASSERT_EQ(vd.create(client_identity(), "Living Room", {1280, 720, 60000}), client_output);
  hypr.update_monitor(client_output, [](fake_monitor_t &m) {
    m.x = 0;
    m.y = -720;
  });
  EXPECT_TRUE(vd.remove(client_identity()));
  ASSERT_EQ(vd.create(client_identity(), "Living Room", {1280, 720, 60000}), client_output);
  auto m = hypr.monitor(client_output);
  ASSERT_TRUE(m);
  EXPECT_EQ(m->x, 0);
  EXPECT_EQ(m->y, -720);
  EXPECT_TRUE(vd.remove(client_identity()));
}

#endif

// Bugs caught: 0, False positives: 0
TEST(HyprlandVdisplayTest, SetHdrSwitchesTheColorPresetAndKeepsItAcrossModeChanges) {
  fake_hyprland_t hypr;
  hyprland::vdisplay_t vd {hypr.options()};
  ASSERT_EQ(vd.init(), status_e::ok);
  hypr.wait_for_listener();
  ASSERT_EQ(vd.create(client_identity(), "Living Room", {2560, 1600, 120000}), client_output);
  EXPECT_EQ(vd.get_hdr(client_output), false);

  ASSERT_TRUE(vd.set_hdr(client_output, true));
  EXPECT_EQ(hypr.count_requests(R"(/eval hl.monitor({ output = "APOLLO-1a2b3c4d", mode = "2560x1600@120.000", position = "auto-right", scale = 1, bitdepth = 10, cm = "hdr", supports_hdr = 1, supports_wide_color = 1, min_luminance = 0.005, max_luminance = 1000, max_avg_luminance = 400 }))"), 1u);
  EXPECT_EQ(hypr.monitor(client_output)->cm, "hdr");
  EXPECT_EQ(vd.get_hdr(client_output), true);

  // A new mode keeps HDR, and so does a config reload.
  ASSERT_TRUE(vd.set_mode(client_output, {1920, 1080, 60000}, false));
  EXPECT_EQ(hypr.count_requests(R"(/eval hl.monitor({ output = "APOLLO-1a2b3c4d", mode = "1920x1080@60.000", position = "auto-right", scale = 1, bitdepth = 10, cm = "hdr")"), 1u);
  hypr.update_monitor(client_output, [](fake_monitor_t &m) {
    m.cm = "srgb";
  });
  hypr.reload_config();
  EXPECT_TRUE(eventually([&]() {
    return hypr.monitor(client_output)->cm == "hdr";
  })) << "HDR was not re-applied after configreloaded";

  ASSERT_TRUE(vd.set_hdr(client_output, false));
  EXPECT_EQ(hypr.monitor(client_output)->cm, "srgb");
  EXPECT_EQ(vd.get_hdr(client_output), false);
  EXPECT_EQ(hypr.count_requests(R"(/eval hl.monitor({ output = "APOLLO-1a2b3c4d", mode = "1920x1080@60.000", position = "auto-right", scale = 1, bitdepth = 8, cm = "srgb")"), 1u);

  EXPECT_TRUE(vd.remove(client_identity()));
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandVdisplayTest, HdrOnlyForOwnOutputs) {
  fake_hyprland_t hypr;
  hyprland::vdisplay_t vd {hypr.options()};
  ASSERT_EQ(vd.init(), status_e::ok);

  // Physical monitors are the display device code's business.
  EXPECT_EQ(vd.get_hdr("DP-1"), std::nullopt);
  EXPECT_FALSE(vd.set_hdr("DP-1", true));
  EXPECT_EQ(hypr.count_requests("/eval hl.monitor("), 0u);
  EXPECT_EQ(hypr.monitor("DP-1")->cm, "srgb");
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandVdisplayTest, SetHdrFailsWhenHyprlandKeepsSdr) {
  fake_hyprland_t hypr;
  hyprland::vdisplay_t vd {hypr.options()};
  ASSERT_EQ(vd.init(), status_e::ok);
  hypr.wait_for_listener();
  ASSERT_EQ(vd.create(client_identity(), "Living Room", {1920, 1080, 60000}), client_output);

  hypr.apply_rules = false;
  EXPECT_FALSE(vd.set_hdr(client_output, true));
  EXPECT_EQ(vd.get_hdr(client_output), false);
  hypr.apply_rules = true;

  EXPECT_TRUE(vd.remove(client_identity()));
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandVdisplayTest, HdrIsNotRememberedAsAUserSetting) {
  fake_hyprland_t hypr;
  hyprland::vdisplay_t vd {hypr.options()};
  ASSERT_EQ(vd.init(), status_e::ok);
  hypr.wait_for_listener();
  ASSERT_EQ(vd.create(client_identity(), "Living Room", {1920, 1080, 60000}), client_output);

  // An HDR client: the output goes to 10 bits for the session.
  ASSERT_TRUE(vd.set_hdr(client_output, true));
  EXPECT_EQ(hypr.monitor(client_output)->format, "XBGR2101010");
  EXPECT_TRUE(vd.remove(client_identity()));
  EXPECT_FALSE(fs::exists(hypr.settings_file())) << "HDR's bit depth is Apollo's, not the user's";

  // The same with process.cpp's restore to SDR before removal.
  ASSERT_EQ(vd.create(client_identity(), "Living Room", {1920, 1080, 60000}), client_output);
  ASSERT_TRUE(vd.set_hdr(client_output, true));
  ASSERT_TRUE(vd.set_hdr(client_output, false));
  EXPECT_EQ(hypr.monitor(client_output)->format, "XRGB8888");
  EXPECT_TRUE(vd.remove(client_identity()));
  EXPECT_FALSE(fs::exists(hypr.settings_file()));
}

// Bugs caught: 1, False positives: 0
// Found with a real client (2026-11-25): terminate() restores the initial HDR state before remove(). set_hdr re-applied
// the rule built from the stored settings, which put back scale 1 over the scale the user had set during the stream,
// so remove() saw nothing to remember.
TEST(HyprlandVdisplayTest, RestoringHdrStateKeepsTheUsersChanges) {
  fake_hyprland_t hypr;
  {
    hyprland::vdisplay_t vd {hypr.options()};
    ASSERT_EQ(vd.init(), status_e::ok);
    hypr.wait_for_listener();
    ASSERT_EQ(vd.create(client_identity(), "Living Room", {2560, 1600, 120000}), client_output);

    hypr.update_monitor(client_output, [](fake_monitor_t &m) {
      m.scale = 2.0;
    });
    // process.cpp's terminate: restore the initial (SDR) state, then remove the display.
    ASSERT_TRUE(vd.set_hdr(client_output, false));
    auto m = hypr.monitor(client_output);
    ASSERT_TRUE(m);
    EXPECT_DOUBLE_EQ(m->scale, 2.0) << "set_hdr must not undo the user's scale";
    EXPECT_TRUE(vd.remove(client_identity()));
  }
  ASSERT_TRUE(fs::exists(hypr.settings_file()));
  std::ifstream in {hypr.settings_file()};
  auto json = nlohmann::json::parse(in);
  EXPECT_EQ(json["displays"][client_output]["scale"], 2.0);
}
