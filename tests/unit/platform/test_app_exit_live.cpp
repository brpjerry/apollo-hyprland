/**
 * @file tests/unit/platform/test_app_exit_live.cpp
 * @brief Closing an app through Hyprland (request_process_group_exit), against the private headless instance.
 *
 * Run with scripts/hyprland/test-live.sh. The tests start their own Wayland client (wev) in their own process group
 * and only close that client's window; they refuse to run outside the private instance started by
 * run-headless-hyprland.sh.
 */
#include "../../tests_common.h"

#ifdef SUNSHINE_BUILD_WAYLAND

  // standard includes
  #include <chrono>
  #include <cstdlib>
  #include <fstream>
  #include <functional>
  #include <memory>
  #include <optional>
  #include <string>
  #include <thread>
  #include <vector>

  // platform includes
  #include <signal.h>
  #include <sys/wait.h>
  #include <unistd.h>

  // local includes
  #include <src/platform/linux/hyprland/display_settings.h>
  #include <src/platform/linux/hyprland/ipc.h>
  #include <src/platform/linux/hyprland/session.h>

namespace {
  using namespace std::chrono_literals;

  class HyprlandAppExitLiveTest: public ::testing::Test {
  protected:
    void SetUp() override {
      // Only the private instance: run-headless-hyprland.sh gives it its own XDG_RUNTIME_DIR.
      const char *private_dir = std::getenv("APOLLO_HEADLESS_HYPRLAND_DIR");
      const char *runtime_dir = std::getenv("XDG_RUNTIME_DIR");
      if (!private_dir || !runtime_dir || std::string {private_dir} != runtime_dir || !std::getenv("HYPRLAND_INSTANCE_SIGNATURE")) {
        GTEST_SKIP() << "needs the private headless Hyprland (scripts/hyprland/test-live.sh)";
      }
      if (std::system("command -v wev >/dev/null 2>&1") != 0) {
        GTEST_SKIP() << "wev (a Wayland test client) is not installed";
      }
      ipc = hyprland::ipc_t::connect();
      if (!ipc) {
        GTEST_SKIP() << "Hyprland does not answer";
      }
    }

    void TearDown() override {
      for (auto pgid : groups) {
        ::kill(-pgid, SIGKILL);
        ::waitpid(pgid, nullptr, 0);
      }
    }

    /**
     * @brief Run `sh -c script` as the leader of a new process group, like proc_t does with bp::group.
     */
    pid_t spawn_group(const char *script) {
      pid_t pid = ::fork();
      if (pid == 0) {
        ::setpgid(0, 0);
        ::execl("/bin/sh", "sh", "-c", script, nullptr);
        ::_exit(127);
      }
      ::setpgid(pid, pid);
      groups.push_back(pid);
      return pid;
    }

    /**
     * @brief The pid of a window whose process is in the group, once one is mapped.
     */
    std::optional<int> wait_for_window(pid_t pgid) {
      auto deadline = std::chrono::steady_clock::now() + 10s;
      while (std::chrono::steady_clock::now() < deadline) {
        if (auto json = ipc->request_json("clients")) {
          for (const auto &client : hyprland::parse_clients(*json)) {
            if (hyprland::process_belongs_to_group(client.pid, pgid, hyprland::read_proc_stat)) {
              return client.pid;
            }
          }
        }
        std::this_thread::sleep_for(50ms);
      }
      return std::nullopt;
    }

    /**
     * @brief Whether a process has exited (gone, or a zombie nobody reaps yet).
     */
    static bool exited(int pid) {
      std::ifstream in {"/proc/" + std::to_string(pid) + "/stat"};
      std::string line;
      if (!std::getline(in, line)) {
        return true;
      }
      auto close = line.rfind(')');
      return close != std::string::npos && line.size() > close + 2 && line[close + 2] == 'Z';
    }

    /**
     * @brief Wait until a group leader exits and reap it.
     * @return Its wait status, or nullopt on timeout.
     */
    std::optional<int> wait_for_exit(pid_t pid) {
      auto deadline = std::chrono::steady_clock::now() + 5s;
      while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        if (::waitpid(pid, &status, WNOHANG) == pid) {
          std::erase(groups, pid);
          return status;
        }
        std::this_thread::sleep_for(20ms);
      }
      return std::nullopt;
    }

    std::unique_ptr<hyprland::ipc_t> ipc;
    std::vector<pid_t> groups;  ///< Spawned groups not reaped yet; killed in TearDown().
  };

  bool wait_until(const std::function<bool()> &condition, std::chrono::milliseconds timeout = 5s) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
      if (std::chrono::steady_clock::now() > deadline) {
        return false;
      }
      std::this_thread::sleep_for(20ms);
    }
    return true;
  }
}  // namespace

// Bugs caught: 0, False positives: 0
TEST_F(HyprlandAppExitLiveTest, ClosesTheWindowInsteadOfSignalling) {
  auto pgid = spawn_group("exec wev >/dev/null 2>&1");
  ASSERT_TRUE(wait_for_window(pgid)) << "wev did not map a window";

  ASSERT_TRUE(platf::request_process_group_exit(pgid));

  // wev exits by itself when its toplevel gets the close event: a normal exit, not a signal.
  auto status = wait_for_exit(pgid);
  ASSERT_TRUE(status) << "the app did not exit after its window was closed";
  EXPECT_TRUE(WIFEXITED(*status)) << "killed by signal " << (WIFSIGNALED(*status) ? WTERMSIG(*status) : 0);

  // Nothing left to escalate.
  EXPECT_FALSE(platf::escalate_process_group_exit(pgid));
}

// Bugs caught: 0, False positives: 0
TEST_F(HyprlandAppExitLiveTest, EscalatesToSigtermWhenTheAppKeepsRunning) {
  // The window closes, but a windowless process of the app keeps the group alive.
  auto pgid = spawn_group("wev >/dev/null 2>&1 & exec sleep 60");
  auto window_pid = wait_for_window(pgid);
  ASSERT_TRUE(window_pid) << "wev did not map a window";

  ASSERT_TRUE(platf::request_process_group_exit(pgid));
  ASSERT_TRUE(wait_until([&]() {
    return exited(*window_pid);
  })) << "the window's process did not exit after the close request";
  EXPECT_TRUE(platf::process_group_running(pgid));

  // After the exit timeout, process.cpp escalates: SIGTERM ends the rest.
  ASSERT_TRUE(platf::escalate_process_group_exit(pgid));
  auto status = wait_for_exit(pgid);
  ASSERT_TRUE(status);
  ASSERT_TRUE(WIFSIGNALED(*status));
  EXPECT_EQ(WTERMSIG(*status), SIGTERM);
  EXPECT_FALSE(platf::escalate_process_group_exit(pgid));
}

// Bugs caught: 0, False positives: 0
TEST_F(HyprlandAppExitLiveTest, ClosesLaterWindowsOfOtherProcessesOnTheAppsDisplay) {
  // The "app" is a launcher without windows; the window comes from a process outside its group started after it,
  // as when Steam starts a game.
  auto app = spawn_group("exec sleep 60");
  std::this_thread::sleep_for(50ms);  // /proc start times have 10 ms resolution
  auto game = spawn_group("exec wev >/dev/null 2>&1");
  auto window_pid = wait_for_window(game);
  ASSERT_TRUE(window_pid);

  std::string output;
  auto json = ipc->request_json("clients");
  ASSERT_TRUE(json);
  for (const auto &client : hyprland::parse_clients(*json)) {
    if (client.pid == *window_pid) {
      for (const auto &monitor : ipc->monitors(true)) {
        if (monitor.id == client.monitor) {
          output = monitor.name;
        }
      }
    }
  }
  ASSERT_FALSE(output.empty());

  // Not the app's window by process, so without the app's display nothing is closed.
  EXPECT_EQ(hyprland::close_app_windows(*ipc, app, ""), 0);
  EXPECT_EQ(hyprland::close_app_windows(*ipc, app, output), 1);
  auto status = wait_for_exit(game);
  ASSERT_TRUE(status);
  EXPECT_TRUE(WIFEXITED(*status));
}

// Bugs caught: 0, False positives: 0
TEST_F(HyprlandAppExitLiveTest, WindowlessAppGetsSigtermRightAway) {
  auto pgid = spawn_group("exec sleep 60");
  ASSERT_TRUE(platf::request_process_group_exit(pgid));
  auto status = wait_for_exit(pgid);
  ASSERT_TRUE(status);
  ASSERT_TRUE(WIFSIGNALED(*status));
  EXPECT_EQ(WTERMSIG(*status), SIGTERM);
  EXPECT_FALSE(platf::escalate_process_group_exit(pgid));
}

#endif
