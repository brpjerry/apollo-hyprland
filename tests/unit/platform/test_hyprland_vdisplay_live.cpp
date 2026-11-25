/**
 * @file tests/unit/platform/test_hyprland_vdisplay_live.cpp
 * @brief Live tests of virtual display focus, workspaces, window placement and remembered settings
 *        (HYP-11, HYP-12) against a real Hyprland.
 *
 * These tests move focus and workspaces around, so they only run in the private headless instance that
 * scripts/hyprland/test-live.sh starts (APOLLO_HEADLESS_HYPRLAND_DIR set and equal to XDG_RUNTIME_DIR), never in a
 * desktop session. Windows are opened with `wev` (a minimal wl_shm client); without it those tests skip.
 */
#include "../../tests_common.h"

#ifdef SUNSHINE_BUILD_WAYLAND

  // standard includes
  #include <cstdlib>
  #include <format>
  #include <thread>

  // platform includes
  #include <csignal>
  #include <sys/wait.h>
  #include <unistd.h>

  // local includes
  #include "hyprland_fakes.h"
  #include <src/platform/linux/hyprland/vdisplay.h>

namespace {
  namespace fs = std::filesystem;
  using namespace std::chrono_literals;

  constexpr auto identity_str = "5EC0DE11-0000-4000-8000-0000000011AB";
  constexpr auto output = "APOLLO-5ec0de11";
  constexpr auto workspace = "apollo-5ec0de11";

  template<class F>
  bool eventually(F &&cond, std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (cond()) {
        return true;
      }
      std::this_thread::sleep_for(20ms);
    }
    return cond();
  }

  std::optional<std::string> find_in_path(const std::string &name) {
    const char *path = std::getenv("PATH");
    if (!path) {
      return std::nullopt;
    }
    std::string_view rest {path};
    while (!rest.empty()) {
      auto dir = rest.substr(0, rest.find(':'));
      rest.remove_prefix(std::min(rest.size(), dir.size() + 1));
      auto candidate = fs::path {dir} / name;
      if (::access(candidate.c_str(), X_OK) == 0) {
        return candidate.string();
      }
    }
    return std::nullopt;
  }

  class HyprlandWorkspaceLiveTest: public ::testing::Test {
  protected:
    void SetUp() override {
      const char *private_dir = std::getenv("APOLLO_HEADLESS_HYPRLAND_DIR");
      const char *runtime_dir = std::getenv("XDG_RUNTIME_DIR");
      if (!private_dir || !runtime_dir || std::string_view {private_dir} != runtime_dir) {
        GTEST_SKIP() << "Only runs in the private headless Hyprland (scripts/hyprland/test-live.sh)";
      }
      ipc_ = hyprland::ipc_t::connect();
      if (!ipc_) {
        GTEST_SKIP() << "No Hyprland instance answers";
      }
      if (ipc_->dialect() != hyprland::config_dialect_e::lua) {
        GTEST_SKIP() << "Written for Hyprland's Lua config (0.55+)";
      }
      auto options = hyprland::vdisplay_t::default_options();
      options.state_dir = tmp_.path() / "state";
      options.settings_file = tmp_.path() / "vdisplay_settings.json";
      vd_ = std::make_unique<hyprland::vdisplay_t>(options);
      ASSERT_EQ(vd_->init(), platf::vdisplay::status_e::ok);
    }

    void TearDown() override {
      for (pid_t pgid : groups_) {
        ::kill(-pgid, SIGKILL);
      }
      for (pid_t pid : children_) {
        ::waitpid(pid, nullptr, 0);
      }
      if (vd_) {
        vd_->remove(identity());
      }
    }

    static uuid_util::uuid_t identity() {
      return uuid_util::uuid_t::parse(identity_str);
    }

    std::vector<hyprland::monitor_info_t> monitors() {
      return ipc_->monitors();
    }

    std::optional<hyprland::monitor_info_t> monitor(const std::string &name) {
      for (auto &m : monitors()) {
        if (m.name == name) {
          return m;
        }
      }
      return std::nullopt;
    }

    std::string focused_monitor() {
      for (auto &m : monitors()) {
        if (m.focused) {
          return m.name;
        }
      }
      return {};
    }

    std::optional<hyprland::client_info_t> client_of(pid_t pid) {
      auto json = ipc_->request_json("clients");
      if (!json) {
        return std::nullopt;
      }
      for (auto &c : hyprland::parse_clients(*json)) {
        if (c.pid == pid) {
          return c;
        }
      }
      return std::nullopt;
    }

    /// Start wev in process group `pgid` (0: a new group led by wev). Returns its pid.
    pid_t spawn_wev(pid_t pgid) {
      pid_t pid = ::fork();
      if (pid == 0) {
        ::setpgid(0, pgid);
        ::execl(wev_->c_str(), "wev", static_cast<char *>(nullptr));
        ::_exit(127);
      }
      ::setpgid(pid, pgid);  // also from the parent, so the group exists before the test relies on it
      children_.push_back(pid);
      groups_.push_back(pgid == 0 ? pid : pgid);
      return pid;
    }

    bool need_wev() {
      wev_ = find_in_path("wev");
      return wev_.has_value();
    }

    bool focus(const std::string &monitor_name) {
      return ipc_->dispatch_lua(hyprland::lua_focus_monitor(monitor_name));
    }

    hyprland_fakes::temp_dir_t tmp_;
    std::unique_ptr<hyprland::ipc_t> ipc_;
    std::unique_ptr<hyprland::vdisplay_t> vd_;
    std::optional<std::string> wev_;
    std::vector<pid_t> children_;
    std::vector<pid_t> groups_;
  };
}  // namespace

// Bugs caught: 0, False positives: 0
TEST_F(HyprlandWorkspaceLiveTest, ActivateFocusesTheDisplayOnItsWorkspaceAndRemoveRestoresFocus) {
  auto before = monitors();
  ASSERT_GE(before.size(), 2u);
  // Start from the second physical output, so "Hyprland's own choice after removal" (the first) differs.
  const auto previous = before[1];
  ASSERT_TRUE(focus(previous.name));
  ASSERT_TRUE(eventually([&]() {
    return focused_monitor() == previous.name;
  }));

  ASSERT_EQ(vd_->create(identity(), "Live test", {1280, 800, 60000}), output);
  ASSERT_TRUE(vd_->set_mode(output, {1280, 800, 60000}, true));
  ASSERT_TRUE(vd_->activate(output));

  auto m = monitor(output);
  ASSERT_TRUE(m);
  EXPECT_TRUE(m->focused);
  EXPECT_EQ(m->active_workspace, workspace);
  // The other monitors keep showing what they showed.
  for (auto &b : before) {
    auto now = monitor(b.name);
    ASSERT_TRUE(now);
    EXPECT_EQ(now->active_workspace, b.active_workspace) << b.name;
  }
  // Focusing the isolated display warps the pointer onto it.
  auto cursor = ipc_->request_json("cursorpos");
  ASSERT_TRUE(cursor);
  const int cx = cursor->value("x", -1);
  const int cy = cursor->value("y", -1);
  EXPECT_GE(cx, m->x);
  EXPECT_LT(cx, m->x + m->width);
  EXPECT_GE(cy, m->y);
  EXPECT_LT(cy, m->y + m->height);

  // A window opened now lands on the virtual display.
  if (need_wev()) {
    pid_t pid = spawn_wev(0);
    ASSERT_TRUE(eventually([&]() {
      return client_of(pid).has_value();
    })) << "wev never mapped a window";
    EXPECT_EQ(client_of(pid)->workspace, workspace);
    EXPECT_EQ(client_of(pid)->monitor, m->id);
  }

  EXPECT_TRUE(vd_->remove(identity()));
  EXPECT_TRUE(eventually([&]() {
    return focused_monitor() == previous.name;
  })) << "focus is on " << focused_monitor();
  auto restored = monitor(previous.name);
  ASSERT_TRUE(restored);
  EXPECT_EQ(restored->active_workspace, previous.active_workspace);
}

// Bugs caught: 0, False positives: 0
TEST_F(HyprlandWorkspaceLiveTest, WindowsOfTheLaunchedProcessGroupMoveToTheDisplay) {
  if (!need_wev()) {
    GTEST_SKIP() << "wev is not installed";
  }
  const auto physical = monitors().front();

  ASSERT_EQ(vd_->create(identity(), "Live test", {1280, 800, 60000}), output);
  ASSERT_TRUE(vd_->activate(output));
  auto vd_monitor = monitor(output);
  ASSERT_TRUE(vd_monitor);

  // The user (or a window rule) puts focus on a physical monitor, so new windows would open there.
  ASSERT_TRUE(focus(physical.name));
  ASSERT_TRUE(eventually([&]() {
    return focused_monitor() == physical.name;
  }));

  // The "app": a process group, as boost::process creates it for proc_t.
  pid_t leader = spawn_wev(0);
  ASSERT_TRUE(eventually([&]() {
    return client_of(leader).has_value();
  }));
  vd_->track_process_group(output, leader);
  // Already open when tracking started: moved right away.
  EXPECT_TRUE(eventually([&]() {
    auto c = client_of(leader);
    return c && c->workspace == workspace;
  })) << "on " << client_of(leader).value_or(hyprland::client_info_t {}).workspace;

  // Opened later by the app: moved when Hyprland reports it.
  pid_t second = spawn_wev(leader);
  EXPECT_TRUE(eventually([&]() {
    auto c = client_of(second);
    return c && c->workspace == workspace && c->monitor == vd_monitor->id;
  }));

  // Some other program's window stays where the user is.
  pid_t stranger = spawn_wev(0);
  ASSERT_TRUE(eventually([&]() {
    return client_of(stranger).has_value();
  }));
  std::this_thread::sleep_for(300ms);
  EXPECT_EQ(client_of(stranger)->workspace, physical.active_workspace);
  // Moving was silent: focus stayed on the physical monitor.
  EXPECT_EQ(focused_monitor(), physical.name);

  EXPECT_TRUE(vd_->remove(identity()));
  // Focus was not on the display at the end, so remove() leaves it alone.
  EXPECT_EQ(focused_monitor(), physical.name);
}

// Bugs caught: 1, False positives: 0
TEST_F(HyprlandWorkspaceLiveTest, RemembersScaleAndWorkspaceAcrossSessions) {
  ASSERT_EQ(vd_->create(identity(), "Live test", {2560, 1600, 60000}), output);
  ASSERT_TRUE(vd_->activate(output));

  // The user changes the scale during the stream.
  ASSERT_TRUE(ipc_->set_monitor({.output = output, .scale = 2.0}));
  ASSERT_TRUE(eventually([&]() {
    auto m = monitor(output);
    return m && m->scale == 2.0;
  }));
  EXPECT_TRUE(vd_->remove(identity()));

  // Drop Hyprland's runtime rule for the output, as a restart of Hyprland would.
  ASSERT_TRUE(ipc_->set_monitor({.output = output, .scale = 1.0}));

  ASSERT_EQ(vd_->create(identity(), "Live test", {2560, 1600, 60000}), output);
  auto m = monitor(output);
  ASSERT_TRUE(m);
  EXPECT_DOUBLE_EQ(m->scale, 2.0);

  // Leaving another workspace on the display is remembered too.
  ASSERT_TRUE(vd_->activate(output));
  ASSERT_EQ(ipc_->eval(hyprland::lua_show_workspace(output, "apollo-live-other")), "ok");
  EXPECT_TRUE(vd_->remove(identity()));
  ASSERT_EQ(vd_->create(identity(), "Live test", {2560, 1600, 60000}), output);
  ASSERT_TRUE(vd_->activate(output));
  m = monitor(output);
  ASSERT_TRUE(m);
  EXPECT_EQ(m->active_workspace, "apollo-live-other");
}

// Bugs caught: 1, False positives: 0
TEST_F(HyprlandWorkspaceLiveTest, ScaleFactorSetsTheOutputScale) {
  // 1280x800 at a 200% scale factor renders 2560x1600; scale 2 keeps the 1280x800 logical size.
  ASSERT_EQ(vd_->create(identity(), "Live test", {2560, 1600, 60000, 200}), output);
  auto m = monitor(output);
  ASSERT_TRUE(m);
  EXPECT_DOUBLE_EQ(m->scale, 2.0);
  EXPECT_TRUE(vd_->remove(identity()));

  ASSERT_EQ(vd_->create(identity(), "Live test", {2560, 1600, 60000}), output);
  m = monitor(output);
  ASSERT_TRUE(m);
  EXPECT_DOUBLE_EQ(m->scale, 1.0) << "the scale of the previous session's rule stuck";
}

#endif
