/**
 * @file tests/unit/platform/test_hyprland_session.cpp
 * @brief Test src/platform/linux/hyprland/session.* against a fake Hyprland socket.
 */
#include "../../tests_common.h"

#ifdef SUNSHINE_BUILD_WAYLAND

  // standard includes
  #include <filesystem>
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

#endif
