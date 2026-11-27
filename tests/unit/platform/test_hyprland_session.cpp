/**
 * @file tests/unit/platform/test_hyprland_session.cpp
 * @brief Test src/platform/linux/hyprland/session.* against a fake Hyprland socket.
 */
#include "../../tests_common.h"

#ifdef SUNSHINE_BUILD_WAYLAND

  // standard includes
  #include <filesystem>
  #include <map>
  #include <string>
  #include <vector>

  // local includes
  #include "hyprland_fakes.h"
  #include <src/platform/linux/hyprland/ipc.h>
  #include <src/platform/linux/hyprland/session.h>

namespace {
  namespace fs = std::filesystem;
  using namespace hyprland_fakes;

  constexpr auto lua_status = R"({"configProvider": "lua", "backend": "drm"})";
  constexpr auto monitors_json = R"([
    {"name": "DP-3", "dpmsStatus": false},
    {"name": "APOLLO-1a2b3c4d", "dpmsStatus": false},
    {"name": "APOLLO-99999999", "dpmsStatus": true}
  ])";

  /**
   * @brief A fake instance answering status, monitors and dispatches; dispatches for `BROKEN` fail.
   */
  fake_request_socket_t::handler_t fake_hyprland(const char *status) {
    return [status](const std::string &req) -> std::string {
      if (req == "j/status") {
        return status;
      }
      if (req == "j/monitors all") {
        return monitors_json;
      }
      if (req.find("BROKEN") != std::string::npos) {
        return "error: no such monitor";
      }
      return "ok";
    };
  }

  std::vector<std::string> dispatches(fake_request_socket_t &server) {
    std::vector<std::string> result;
    for (auto &req : server.requests()) {
      if (req.starts_with("/dispatch")) {
        result.push_back(req);
      }
    }
    return result;
  }
}  // namespace

// Bugs caught: 0, False positives: 0
TEST(HyprlandSessionTest, DpmsCallNamesOneOutput) {
  EXPECT_EQ(hyprland::lua_dpms_call("APOLLO-1a2b3c4d", true), R"(hl.dsp.dpms({ action = "on", monitor = "APOLLO-1a2b3c4d" }))");
  EXPECT_EQ(hyprland::lua_dpms_call("we\"ird", false), R"(hl.dsp.dpms({ action = "off", monitor = "we\"ird" }))");
  // Without a monitor the dispatcher would switch every output.
  EXPECT_EQ(hyprland::lua_dpms_call("", true), std::nullopt);
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandSessionTest, WakeOutputOnlyTouchesTheStreamedOutput) {
  temp_dir_t tmp;
  fs::create_directories(tmp.path() / "sig_1_1");
  fake_request_socket_t server {tmp.path() / "sig_1_1" / ".socket.sock", fake_hyprland(lua_status)};
  auto ipc = hyprland::ipc_t::connect(tmp.path(), "sig_1_1");
  ASSERT_NE(ipc, nullptr);

  EXPECT_EQ(hyprland::wake_output(*ipc, "APOLLO-99999999"), hyprland::wake_result_e::already_on);
  EXPECT_EQ(hyprland::wake_output(*ipc, "APOLLO-1a2b3c4d"), hyprland::wake_result_e::woken);
  EXPECT_EQ(hyprland::wake_output(*ipc, "0"), hyprland::wake_result_e::not_found);
  EXPECT_EQ(hyprland::wake_output(*ipc, ""), hyprland::wake_result_e::not_found);
  EXPECT_EQ(hyprland::wake_output(*ipc, "APOLLO-1a2b3c4"), hyprland::wake_result_e::not_found);

  // DP-3 is off too, but only the streamed output is switched on.
  std::vector<std::string> expected {R"(/dispatch hl.dsp.dpms({ action = "on", monitor = "APOLLO-1a2b3c4d" }))"};
  EXPECT_EQ(dispatches(server), expected);
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandSessionTest, WakeOutputResolvesTheLegacyIndexLikeCapture) {
  temp_dir_t tmp;
  fs::create_directories(tmp.path() / "sig_1_1");
  fake_request_socket_t server {tmp.path() / "sig_1_1" / ".socket.sock", fake_hyprland(lua_status)};
  auto ipc = hyprland::ipc_t::connect(tmp.path(), "sig_1_1");
  ASSERT_NE(ipc, nullptr);

  // Capture indexes the Wayland outputs, in Wayland's order.
  int enumerated = 0;
  const auto wayland_names = [&]() {
    ++enumerated;
    return std::vector<std::string> {"APOLLO-99999999", "APOLLO-1a2b3c4d", "DP-3"};
  };
  EXPECT_EQ(hyprland::wake_output(*ipc, "0", wayland_names), hyprland::wake_result_e::already_on);
  EXPECT_EQ(hyprland::wake_output(*ipc, "1", wayland_names), hyprland::wake_result_e::woken);
  EXPECT_EQ(hyprland::wake_output(*ipc, "3", wayland_names), hyprland::wake_result_e::not_found);
  EXPECT_EQ(enumerated, 3);
  // Names are not looked up in Wayland.
  EXPECT_EQ(hyprland::wake_output(*ipc, "APOLLO-99999999", wayland_names), hyprland::wake_result_e::already_on);
  EXPECT_EQ(hyprland::wake_output(*ipc, "DP-", wayland_names), hyprland::wake_result_e::not_found);
  EXPECT_EQ(enumerated, 3);

  std::vector<std::string> expected {R"(/dispatch hl.dsp.dpms({ action = "on", monitor = "APOLLO-1a2b3c4d" }))"};
  EXPECT_EQ(dispatches(server), expected);
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandSessionTest, WakeOutputOnHyprlang) {
  temp_dir_t tmp;
  fs::create_directories(tmp.path() / "sig_1_1");
  fake_request_socket_t server {tmp.path() / "sig_1_1" / ".socket.sock", fake_hyprland(R"({"configProvider": "hyprlang"})")};
  auto ipc = hyprland::ipc_t::connect(tmp.path(), "sig_1_1");
  ASSERT_NE(ipc, nullptr);

  EXPECT_EQ(hyprland::wake_output(*ipc, "APOLLO-1a2b3c4d"), hyprland::wake_result_e::woken);
  EXPECT_FALSE(ipc->set_dpms("", true));
  EXPECT_FALSE(ipc->set_dpms("has space", true));
  EXPECT_FALSE(ipc->set_dpms("BROKEN", true));
  std::vector<std::string> expected {"/dispatch dpms on APOLLO-1a2b3c4d", "/dispatch dpms on BROKEN"};
  EXPECT_EQ(dispatches(server), expected);
}

namespace {
  /**
   * @brief A fake /proc: pid -> {ppid, pgid, start_time}.
   */
  auto fake_procs(std::map<int, hyprland::proc_stat_t> procs) {
    return [procs = std::move(procs)](int pid) -> std::optional<hyprland::proc_stat_t> {
      auto it = procs.find(pid);
      if (it == procs.end()) {
        return std::nullopt;
      }
      return it->second;
    };
  }
}  // namespace

// Bugs caught: 1, False positives: 0
TEST(HyprlandSessionTest, ParseProcStatStartTime) {
  // A real line from /proc/<pid>/stat (comm with a space and a parenthesis); starttime is field 22.
  auto st = hyprland::parse_proc_stat("4242 (Game (x64)) S 4000 4100 4100 0 -1 4194560 2361 0 0 0 7 3 0 0 20 0 3 0 987654 18145280 1320 18446744073709551615 1 1 0 0 0 0 0 4096 0 0 0 0 17 5 0 0 0 0 0");
  ASSERT_TRUE(st);
  EXPECT_EQ(st->ppid, 4000);
  EXPECT_EQ(st->pgid, 4100);
  EXPECT_EQ(st->start_time, 987654u);

  // Truncated: still parsed, start time unknown.
  st = hyprland::parse_proc_stat("1234 (my app) S 1000 1200 1000 0 -1 4194560 ...");
  ASSERT_TRUE(st);
  EXPECT_EQ(st->pgid, 1200);
  EXPECT_EQ(st->start_time, 0u);
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandSessionTest, AppWindowsIncludeGroupAndLaterWindowsOnTheStreamOnly) {
  // The app (group 500, started at t=1000) launched a game through Steam, which was already running (pid 300).
  auto procs = fake_procs({
    {500, {.ppid = 1, .pgid = 500, .start_time = 1000}},  // launcher script, group leader
    {510, {.ppid = 500, .pgid = 500, .start_time = 1010}},  // app window process in the group
    {520, {.ppid = 510, .pgid = 520, .start_time = 1020}},  // child that called setsid()
    {300, {.ppid = 1, .pgid = 300, .start_time = 100}},  // Steam, started before the app
    {310, {.ppid = 300, .pgid = 300, .start_time = 1500}},  // the game, a child of Steam, started after the app
    {400, {.ppid = 1, .pgid = 400, .start_time = 2000}},  // a terminal the user opened on the desktop meanwhile
  });
  std::vector<hyprland::client_info_t> clients {
    {.address = "0xa510", .pid = 510, .workspace = "apollo-1", .monitor = 7},
    {.address = "0xa520", .pid = 520, .workspace = "3", .monitor = 0},
    {.address = "0xa300", .pid = 300, .workspace = "apollo-1", .monitor = 7},
    {.address = "0xa310", .pid = 310, .workspace = "apollo-1", .monitor = 7},
    {.address = "0xa400", .pid = 400, .workspace = "2", .monitor = 0},
    {.address = "", .pid = 510, .monitor = 7},
    {.address = "0xdead", .pid = 999, .monitor = 7},  // process already gone
  };

  std::vector<std::string> expected {"0xa510", "0xa520", "0xa310"};
  EXPECT_EQ(hyprland::app_window_addresses(clients, {.pgid = 500, .stream_monitor = 7}, procs), expected);

  // Without the app's own display only the group's windows are closed.
  std::vector<std::string> group_only {"0xa510", "0xa520"};
  EXPECT_EQ(hyprland::app_window_addresses(clients, {.pgid = 500}, procs), group_only);

  // Leader gone (start time unknown): the stream monitor rule cannot be applied safely.
  auto no_leader = fake_procs({{510, {.ppid = 1, .pgid = 500, .start_time = 1010}}, {310, {.ppid = 300, .pgid = 300, .start_time = 1500}}});
  std::vector<std::string> leader_gone {"0xa510"};
  EXPECT_EQ(hyprland::app_window_addresses(clients, {.pgid = 500, .stream_monitor = 7}, no_leader), leader_gone);

  EXPECT_TRUE(hyprland::app_window_addresses(clients, {.pgid = 0, .stream_monitor = 7}, procs).empty());
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandSessionTest, CloseAppWindowsDispatchesClose) {
  EXPECT_EQ(hyprland::lua_close_window("0x55aa"), R"(hl.dsp.window.close({ window = "address:0x55aa" }))");

  temp_dir_t tmp;
  fs::create_directories(tmp.path() / "sig_1_1");
  fake_request_socket_t server {tmp.path() / "sig_1_1" / ".socket.sock", [](const std::string &req) -> std::string {
                                  if (req == "j/status") {
                                    return lua_status;
                                  }
                                  if (req == "j/clients") {
                                    return R"([
                                      {"address": "0xa510", "pid": 510, "monitor": 1, "workspace": {"name": "apollo-1"}},
                                      {"address": "0xa310", "pid": 310, "monitor": 1, "workspace": {"name": "apollo-1"}},
                                      {"address": "0xa400", "pid": 400, "monitor": 0, "workspace": {"name": "2"}}
                                    ])";
                                  }
                                  if (req == "j/monitors all") {
                                    return R"([{"name": "DP-3", "id": 0}, {"name": "APOLLO-1a2b3c4d", "id": 1}])";
                                  }
                                  return "ok";
                                }};
  auto ipc = hyprland::ipc_t::connect(tmp.path(), "sig_1_1");
  ASSERT_NE(ipc, nullptr);
  auto procs = fake_procs({
    {500, {.ppid = 1, .pgid = 500, .start_time = 1000}},
    {510, {.ppid = 500, .pgid = 500, .start_time = 1010}},
    {310, {.ppid = 300, .pgid = 300, .start_time = 1500}},
    {400, {.ppid = 1, .pgid = 400, .start_time = 2000}},
  });

  EXPECT_EQ(hyprland::close_app_windows(*ipc, 500, "APOLLO-1a2b3c4d", procs), 2);
  std::vector<std::string> expected {
    R"(/dispatch hl.dsp.window.close({ window = "address:0xa510" }))",
    R"(/dispatch hl.dsp.window.close({ window = "address:0xa310" }))",
  };
  EXPECT_EQ(dispatches(server), expected);

  // A group without windows: nothing is closed, the caller falls back to SIGTERM.
  EXPECT_EQ(hyprland::close_app_windows(*ipc, 777, "", procs), 0);
  EXPECT_EQ(dispatches(server).size(), 2);
}

#endif
