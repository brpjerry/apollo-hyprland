/**
 * @file tests/unit/platform/test_hyprland_ipc.cpp
 * @brief Test src/platform/linux/hyprland/ipc.* against fake sockets and recorded fixtures.
 */
#include "../../tests_common.h"

#ifdef SUNSHINE_BUILD_WAYLAND

  // standard includes
  #include <atomic>
  #include <condition_variable>
  #include <cstdlib>
  #include <fstream>
  #include <functional>
  #include <limits>
  #include <mutex>
  #include <thread>

  // platform includes
  #include <poll.h>
  #include <sys/socket.h>
  #include <sys/un.h>
  #include <unistd.h>

  // local includes
  #include "hyprland_fakes.h"
  #include <src/platform/linux/hyprland/ipc.h>

namespace {
  namespace fs = std::filesystem;
  using namespace std::chrono_literals;

  using namespace hyprland_fakes;

  using ev_t = std::pair<std::string, std::string>;

  constexpr auto lua_status = R"({"configProvider": "lua", "backend": "drm"})";

  /**
   * @brief Collects events from a listener thread for the test thread to wait on.
   */
  struct event_sink_t {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::pair<std::string, std::string>> events;

    void operator()(std::string_view event, std::string_view data) {
      std::lock_guard lock {mutex};
      events.emplace_back(event, data);
      cv.notify_all();
    }

    bool wait_for_count(std::size_t count) {
      std::unique_lock lock {mutex};
      return cv.wait_for(lock, 5s, [&]() {
        return events.size() >= count;
      });
    }
  };

  void write_file(const fs::path &path, const std::string &contents) {
    std::ofstream out {path};
    out << contents;
  }
}  // namespace

// Bugs caught: 0, False positives: 0
TEST(HyprlandDiscoveryTest, UsesEnvSignatureWhenItAnswers) {
  temp_dir_t tmp;
  fs::create_directories(tmp.path() / "abc_200_1");
  fs::create_directories(tmp.path() / "abc_300_1");
  fake_request_socket_t a {tmp.path() / "abc_200_1" / ".socket.sock", [](auto &) {
                             return std::string {lua_status};
                           }};
  fake_request_socket_t b {tmp.path() / "abc_300_1" / ".socket.sock", [](auto &) {
                             return std::string {lua_status};
                           }};

  EXPECT_EQ(hyprland::find_instance_signature(tmp.path(), "abc_200_1", nullptr), "abc_200_1");
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandDiscoveryTest, StaleEnvSignatureFallsBackToLiveInstance) {
  temp_dir_t tmp;
  // A dead instance as found on the live machine: socket2 and log left behind, no .socket.sock, no lock.
  auto stale = tmp.path() / "abc_100_1";
  fs::create_directories(stale);
  make_dead_socket(stale / ".socket2.sock");
  write_file(stale / "hyprland.log", "old log\n");
  // Another dead one whose .socket.sock file still exists but refuses connections.
  auto refused = tmp.path() / "abc_150_1";
  fs::create_directories(refused);
  make_dead_socket(refused / ".socket.sock");

  auto live = tmp.path() / "abc_120_1";
  fs::create_directories(live);
  fake_request_socket_t server {live / ".socket.sock", [](auto &) {
                                  return std::string {lua_status};
                                }};

  EXPECT_EQ(hyprland::find_instance_signature(tmp.path(), "abc_100_1", "wayland-1"), "abc_120_1");
  EXPECT_EQ(hyprland::find_instance_signature(tmp.path(), "abc_150_1", nullptr), "abc_120_1");
  EXPECT_EQ(hyprland::find_instance_signature(tmp.path(), nullptr, nullptr), "abc_120_1");
  // The probe is a status request.
  ASSERT_FALSE(server.requests().empty());
  EXPECT_EQ(server.requests().front(), "j/status");
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandDiscoveryTest, MultipleInstancesPreferWaylandDisplayThenNewest) {
  temp_dir_t tmp;
  auto older = tmp.path() / "abc_100_1";
  auto newer = tmp.path() / "abc_200_1";
  fs::create_directories(older);
  fs::create_directories(newer);
  write_file(older / "hyprland.lock", "1234\nwayland-1\n");
  write_file(newer / "hyprland.lock", "5678\nwayland-2\n");
  fake_request_socket_t a {older / ".socket.sock", [](auto &) {
                             return std::string {lua_status};
                           }};
  fake_request_socket_t b {newer / ".socket.sock", [](auto &) {
                             return std::string {lua_status};
                           }};

  EXPECT_EQ(hyprland::find_instance_signature(tmp.path(), nullptr, "wayland-1"), "abc_100_1");
  EXPECT_EQ(hyprland::find_instance_signature(tmp.path(), nullptr, "wayland-2"), "abc_200_1");
  EXPECT_EQ(hyprland::find_instance_signature(tmp.path(), nullptr, "wayland-9"), "abc_200_1");
  EXPECT_EQ(hyprland::find_instance_signature(tmp.path(), "dead_999_1", nullptr), "abc_200_1");
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandDiscoveryTest, NothingAnswering) {
  temp_dir_t tmp;
  fs::create_directories(tmp.path() / "abc_100_1");
  make_dead_socket(tmp.path() / "abc_100_1" / ".socket.sock");

  EXPECT_EQ(hyprland::find_instance_signature(tmp.path(), "abc_100_1", nullptr), std::nullopt);
  EXPECT_EQ(hyprland::find_instance_signature(tmp.path() / "missing", nullptr, nullptr), std::nullopt);
  // A signature must not escape the runtime directory.
  EXPECT_EQ(hyprland::find_instance_signature(tmp.path(), "../abc_100_1", nullptr), std::nullopt);
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandIpcTest, LuaRequestsAreFramedLikeHyprctl) {
  temp_dir_t tmp;
  fs::create_directories(tmp.path() / "sig_1_1");
  fake_request_socket_t server {tmp.path() / "sig_1_1" / ".socket.sock", [](const std::string &req) -> std::string {
                                  if (req == "j/status") {
                                    return lua_status;
                                  }
                                  if (req == "/repl return 1+1") {
                                    return "2";
                                  }
                                  if (req.starts_with("/eval hl.monitor({ output = \"BAD\"")) {
                                    return "error: hl.monitor: error applying field 'mode'";
                                  }
                                  if (req == "/output remove DP-3") {
                                    return "cannot remove a real display. Use the monitor keyword.";
                                  }
                                  return "ok";
                                }};

  auto ipc = hyprland::ipc_t::connect(tmp.path(), "sig_1_1");
  ASSERT_NE(ipc, nullptr);
  EXPECT_EQ(ipc->dialect(), hyprland::config_dialect_e::lua);
  EXPECT_EQ(ipc->signature(), "sig_1_1");
  EXPECT_EQ(ipc->event_socket_path(), tmp.path() / "sig_1_1" / ".socket2.sock");

  EXPECT_TRUE(ipc->create_headless("APOLLO-1a2b3c4d"));
  EXPECT_TRUE(ipc->remove_output("APOLLO-1a2b3c4d"));
  EXPECT_FALSE(ipc->remove_output("DP-3"));
  EXPECT_FALSE(ipc->create_headless("has space"));
  EXPECT_TRUE(ipc->set_monitor({.output = "APOLLO-1a2b3c4d", .mode = "1280x720@60"}));
  EXPECT_FALSE(ipc->set_monitor({.output = "BAD", .mode = "garbage"}));
  EXPECT_TRUE(ipc->set_device_option("my-mouse", "sensitivity", "-0.5"));
  EXPECT_TRUE(ipc->dispatch_lua("hl.dsp.focus({ workspace = 4 })"));
  EXPECT_FALSE(ipc->dispatch_legacy("workspace", "4"));
  EXPECT_FALSE(ipc->keyword("general:gaps_in", "5"));
  EXPECT_EQ(ipc->eval("return 1+1"), "2");

  auto requests = server.requests();
  std::vector<std::string> expected {
    "j/status",
    "/output create headless APOLLO-1a2b3c4d",
    "/output remove APOLLO-1a2b3c4d",
    "/output remove DP-3",
    R"(/eval hl.monitor({ output = "APOLLO-1a2b3c4d", mode = "1280x720@60" }))",
    R"(/eval hl.monitor({ output = "BAD", mode = "garbage" }))",
    R"(/eval hl.device({ name = "my-mouse", sensitivity = -0.5 }))",
    "/dispatch hl.dsp.focus({ workspace = 4 })",
    "/repl return 1+1",
  };
  EXPECT_EQ(requests, expected);
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandIpcTest, HyprlangRequestsUseKeywords) {
  temp_dir_t tmp;
  fs::create_directories(tmp.path() / "sig_1_1");
  // Hyprland 0.54 has no status request.
  fake_request_socket_t server {tmp.path() / "sig_1_1" / ".socket.sock", [](const std::string &req) -> std::string {
                                  return req == "j/status" ? "unknown request" : "ok";
                                }};

  auto ipc = hyprland::ipc_t::connect(tmp.path(), "sig_1_1");
  ASSERT_NE(ipc, nullptr);
  EXPECT_EQ(ipc->dialect(), hyprland::config_dialect_e::hyprlang);

  EXPECT_TRUE(ipc->set_monitor({.output = "HEADLESS-2", .mode = "2560x1600@120", .scale = 1.5, .transform = 1}));
  EXPECT_TRUE(ipc->set_monitor({.output = "HEADLESS-2", .disabled = true}));
  EXPECT_TRUE(ipc->set_device_option("my-mouse", "sensitivity", "-0.5"));
  EXPECT_FALSE(ipc->set_device_option("bad]name", "sensitivity", "-0.5"));
  EXPECT_TRUE(ipc->dispatch_legacy("workspace", "4"));
  EXPECT_FALSE(ipc->dispatch_lua("hl.dsp.focus({ workspace = 4 })"));
  EXPECT_EQ(ipc->eval("return 1"), std::nullopt);

  auto requests = server.requests();
  std::vector<std::string> expected {
    "j/status",
    "/keyword monitor HEADLESS-2,2560x1600@120,auto,1.5,transform,1",
    "/keyword monitor HEADLESS-2,disable",
    "/keyword device[my-mouse]:sensitivity -0.5",
    "/dispatch workspace 4",
  };
  EXPECT_EQ(requests, expected);
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandIpcTest, RequestJsonAddsJsonFlag) {
  temp_dir_t tmp;
  fs::create_directories(tmp.path() / "sig_1_1");
  fake_request_socket_t server {tmp.path() / "sig_1_1" / ".socket.sock", [](const std::string &req) -> std::string {
                                  if (req == "j/status") {
                                    return lua_status;
                                  }
                                  if (req == "j/garbage") {
                                    return "unknown request";
                                  }
                                  return req.starts_with("j") ? R"([{"name": "DP-3", "width": 3840}])" : "not json";
                                }};
  auto ipc = hyprland::ipc_t::connect(tmp.path(), "sig_1_1");
  ASSERT_NE(ipc, nullptr);

  auto monitors = ipc->monitors();
  ASSERT_EQ(monitors.size(), 1);
  EXPECT_EQ(monitors[0].name, "DP-3");
  EXPECT_EQ(monitors[0].width, 3840);
  EXPECT_TRUE(ipc->request_json("j/devices").has_value());
  EXPECT_TRUE(ipc->request_json("a/workspaces").has_value());
  EXPECT_EQ(ipc->request("version"), "not json");
  EXPECT_TRUE(ipc->request_json("/version").has_value());
  EXPECT_EQ(ipc->request_json("garbage"), std::nullopt);

  auto requests = server.requests();
  std::vector<std::string> expected {"j/status", "j/monitors all", "j/devices", "ja/workspaces", "version", "j/version", "j/garbage"};
  EXPECT_EQ(requests, expected);
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandIpcTest, ConnectFailsWithoutLiveSocket) {
  temp_dir_t tmp;
  fs::create_directories(tmp.path() / "sig_1_1");
  make_dead_socket(tmp.path() / "sig_1_1" / ".socket.sock");
  EXPECT_EQ(hyprland::ipc_t::connect(tmp.path(), "sig_1_1"), nullptr);
  EXPECT_EQ(hyprland::ipc_t::connect(tmp.path(), "missing"), nullptr);
  EXPECT_EQ(hyprland::raw_request(tmp.path() / "sig_1_1" / ".socket.sock", "j/status"), std::nullopt);
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandIpcTest, ParseRecordedMonitors) {
  std::ifstream in {SUNSHINE_SOURCE_DIR "/tests/fixtures/hyprland/monitors_all.json"};
  ASSERT_TRUE(in.is_open());
  auto json = nlohmann::json::parse(in);
  auto monitors = hyprland::parse_monitors(json);
  ASSERT_EQ(monitors.size(), 3);

  // Recorded on Hyprland 0.57.2: two 4K monitors (one rotated) and a headless output set to 1280x720@60.
  const auto &hdmi = monitors[0];
  EXPECT_EQ(hdmi.name, "HDMI-A-1");
  EXPECT_EQ(hdmi.description, "Samsung Electric Company Odyssey G80SD REDACTED");
  EXPECT_EQ(hdmi.id, 0);
  EXPECT_EQ(hdmi.width, 3840);
  EXPECT_EQ(hdmi.height, 2160);
  EXPECT_DOUBLE_EQ(hdmi.refresh_hz, 239.991);
  EXPECT_EQ(hdmi.x, 2560);
  EXPECT_EQ(hdmi.y, -560);
  EXPECT_DOUBLE_EQ(hdmi.scale, 1.5);
  EXPECT_EQ(hdmi.transform, 1);
  EXPECT_FALSE(hdmi.disabled);
  EXPECT_EQ(hdmi.current_format, "XBGR2101010");
  EXPECT_TRUE(hdmi.focused);

  const auto &dp = monitors[1];
  EXPECT_EQ(dp.name, "DP-3");
  EXPECT_EQ(dp.id, 1);
  EXPECT_DOUBLE_EQ(dp.refresh_hz, 240.0);
  EXPECT_EQ(dp.x, 0);
  EXPECT_EQ(dp.y, 0);
  EXPECT_EQ(dp.transform, 0);
  EXPECT_FALSE(dp.focused);

  const auto &headless = monitors[2];
  EXPECT_EQ(headless.name, "APOLLO-TEST-fixture");
  EXPECT_EQ(headless.description, "");
  EXPECT_EQ(headless.id, 2);
  EXPECT_EQ(headless.width, 1280);
  EXPECT_EQ(headless.height, 720);
  EXPECT_DOUBLE_EQ(headless.refresh_hz, 60.0);
  EXPECT_EQ(headless.x, 4000);
  EXPECT_DOUBLE_EQ(headless.scale, 1.0);
  EXPECT_EQ(headless.current_format, "XRGB8888");
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandIpcTest, ParseMonitorsToleratesOddInput) {
  EXPECT_TRUE(hyprland::parse_monitors(nlohmann::json::object()).empty());
  auto monitors = hyprland::parse_monitors(nlohmann::json::parse(R"([1, {"name": "X", "width": "bad", "disabled": true}])"));
  ASSERT_EQ(monitors.size(), 1);
  EXPECT_EQ(monitors[0].name, "X");
  EXPECT_EQ(monitors[0].width, 0);
  EXPECT_EQ(monitors[0].id, -1);
  EXPECT_TRUE(monitors[0].disabled);
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandIpcTest, ParseDialect) {
  EXPECT_EQ(hyprland::parse_dialect(R"({"configProvider": "lua", "backend": "drm"})"), hyprland::config_dialect_e::lua);
  EXPECT_EQ(hyprland::parse_dialect(R"({"configProvider": "hyprlang", "backend": "drm"})"), hyprland::config_dialect_e::hyprlang);
  EXPECT_EQ(hyprland::parse_dialect("unknown request"), hyprland::config_dialect_e::hyprlang);
  EXPECT_EQ(hyprland::parse_dialect(""), hyprland::config_dialect_e::hyprlang);
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandIpcTest, ParseMonitorEvent) {
  auto added = hyprland::parse_monitor_event("2,APOLLO-TEST-probe,");
  ASSERT_TRUE(added);
  EXPECT_EQ(added->id, 2);
  EXPECT_EQ(added->name, "APOLLO-TEST-probe");
  EXPECT_EQ(added->description, "");

  auto physical = hyprland::parse_monitor_event("0,DP-3,Samsung Electric Company Odyssey G80SH, rev 2");
  ASSERT_TRUE(physical);
  EXPECT_EQ(physical->name, "DP-3");
  EXPECT_EQ(physical->description, "Samsung Electric Company Odyssey G80SH, rev 2");

  auto no_description = hyprland::parse_monitor_event("5,HEADLESS-1");
  ASSERT_TRUE(no_description);
  EXPECT_EQ(no_description->name, "HEADLESS-1");

  EXPECT_FALSE(hyprland::parse_monitor_event("APOLLO-TEST-probe"));
  EXPECT_FALSE(hyprland::parse_monitor_event("x2,NAME,"));
  EXPECT_FALSE(hyprland::parse_monitor_event("2,,"));
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandLuaTest, QuoteEscapes) {
  EXPECT_EQ(hyprland::lua_quote("DP-3"), R"("DP-3")");
  EXPECT_EQ(hyprland::lua_quote(R"(a"b\c)"), R"("a\"b\\c")");
  EXPECT_EQ(hyprland::lua_quote("line1\nline2\r\t"), R"("line1\nline2\r\t")");
  // Control bytes use three-digit escapes so the following digit stays separate.
  EXPECT_EQ(hyprland::lua_quote(std::string {"\x01"
                                             "2",
                                             2}),
            R"("\0012")");
  EXPECT_EQ(hyprland::lua_quote(std::string {"a\0b", 3}), R"("a\000b")");
  EXPECT_EQ(hyprland::lua_quote("\x7f"), R"("\127")");
  // Code injection attempts stay inside the string.
  EXPECT_EQ(hyprland::lua_quote(R"(x" }) os.exit() --)"), R"("x\" }) os.exit() --")");
  // UTF-8 passes through.
  EXPECT_EQ(hyprland::lua_quote("Odyssey G80SD \xe2\x84\xa2"), "\"Odyssey G80SD \xe2\x84\xa2\"");
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandLuaTest, MonitorCall) {
  hyprland::monitor_rule_t full {
    .output = "APOLLO-1a2b3c4d",
    .mode = "2560x1600@119.998",
    .position = "auto-right",
    .scale = 1.25,
    .transform = 3,
    .bitdepth = 10,
    .disabled = false,
  };
  EXPECT_EQ(
    hyprland::lua_monitor_call(full),
    R"(hl.monitor({ output = "APOLLO-1a2b3c4d", mode = "2560x1600@119.998", position = "auto-right", scale = 1.25, transform = 3, bitdepth = 10, disabled = false }))"
  );

  EXPECT_EQ(hyprland::lua_monitor_call({.output = "DP-3", .scale = 1.0}), R"(hl.monitor({ output = "DP-3", scale = 1 }))");
  EXPECT_EQ(hyprland::lua_monitor_call({.output = "HDMI-A-1", .disabled = true}), R"(hl.monitor({ output = "HDMI-A-1", disabled = true }))");
  EXPECT_EQ(hyprland::lua_monitor_call({.output = R"(we"ird)", .position = "0x0"}), R"(hl.monitor({ output = "we\"ird", position = "0x0" }))");

  EXPECT_EQ(hyprland::lua_monitor_call({.output = ""}), std::nullopt);
  EXPECT_EQ(hyprland::lua_monitor_call({.output = "DP-3", .scale = std::numeric_limits<double>::quiet_NaN()}), std::nullopt);
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandLuaTest, HyprlangMonitorValue) {
  EXPECT_EQ(hyprland::hyprlang_monitor_value({.output = "APOLLO-1a2b3c4d", .mode = "1280x720@60"}), "APOLLO-1a2b3c4d,1280x720@60,auto,auto");
  EXPECT_EQ(
    hyprland::hyprlang_monitor_value({.output = "DP-3", .mode = "3840x2160@240", .position = "0x0", .scale = 1.5, .transform = 1, .bitdepth = 10}),
    "DP-3,3840x2160@240,0x0,1.5,transform,1,bitdepth,10"
  );
  EXPECT_EQ(hyprland::hyprlang_monitor_value({.output = "DP-3"}), "DP-3,preferred,auto,auto");
  EXPECT_EQ(hyprland::hyprlang_monitor_value({.output = "DP-3", .mode = "1920x1080", .disabled = true}), "DP-3,disable");
  EXPECT_EQ(hyprland::hyprlang_monitor_value({.output = "DP-3,evil"}), std::nullopt);
  EXPECT_EQ(hyprland::hyprlang_monitor_value({.output = "DP-3", .mode = "1x1\nexec"}), std::nullopt);
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandLuaTest, DeviceCallAndValues) {
  EXPECT_EQ(hyprland::lua_device_call("logitech-g502", "sensitivity", "-0.5"), R"(hl.device({ name = "logitech-g502", sensitivity = -0.5 }))");
  EXPECT_EQ(hyprland::lua_device_call("kbd", "kb_layout", "us,de"), R"(hl.device({ name = "kbd", kb_layout = "us,de" }))");
  EXPECT_EQ(hyprland::lua_device_call("pad", "enabled", "false"), R"(hl.device({ name = "pad", enabled = false }))");
  EXPECT_EQ(hyprland::lua_device_call("pad", "odd key", "1"), R"(hl.device({ name = "pad", ["odd key"] = 1 }))");
  EXPECT_EQ(hyprland::lua_device_call("", "enabled", "false"), std::nullopt);

  EXPECT_EQ(hyprland::lua_value("42"), "42");
  EXPECT_EQ(hyprland::lua_value("-1.25"), "-1.25");
  EXPECT_EQ(hyprland::lua_value("1."), R"("1.")");
  EXPECT_EQ(hyprland::lua_value("0x10"), R"("0x10")");
  EXPECT_EQ(hyprland::lua_value("true"), "true");
  EXPECT_EQ(hyprland::lua_value("yes"), R"("yes")");
  EXPECT_EQ(hyprland::lua_value("100 200"), R"("100 200")");
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandEventListenerTest, DeliversSplitLinesAndReconnects) {
  temp_dir_t tmp;
  auto path = tmp.path() / ".socket2.sock";
  fake_event_socket_t server {path};

  hyprland::event_listener_t listener {path};
  listener.set_reconnect_interval(50ms);
  event_sink_t sink;
  event_sink_t other;
  listener.subscribe(std::ref(sink));
  auto other_token = listener.subscribe(std::ref(other));

  ASSERT_TRUE(listener.start());
  ASSERT_TRUE(server.wait_for_connections(1));
  EXPECT_TRUE(listener.connected());

  // A line split across writes, and several lines in one write.
  server.send("monitoradd");
  server.send("edv2>>2,APOLLO-TEST-probe,\nconfigreloaded>>\nopenwindow>>80a1b2c3,4,kitty,Terminal >> title\n");
  ASSERT_TRUE(sink.wait_for_count(3));
  ASSERT_TRUE(other.wait_for_count(3));
  {
    std::lock_guard lock {sink.mutex};
    EXPECT_EQ(sink.events[0], ev_t("monitoraddedv2", "2,APOLLO-TEST-probe,"));
    EXPECT_EQ(sink.events[1], ev_t("configreloaded", ""));
    EXPECT_EQ(sink.events[2], ev_t("openwindow", "80a1b2c3,4,kitty,Terminal >> title"));
  }

  listener.unsubscribe(other_token);

  // Drop the connection; the listener must reconnect and keep delivering.
  server.drop_client();
  ASSERT_TRUE(server.wait_for_connections(2));
  // The reconnect is seen by the server before the listener marks itself connected; wait for that too.
  for (int i = 0; i < 100 && !listener.connected(); ++i) {
    std::this_thread::sleep_for(10ms);
  }
  server.send("monitorremovedv2>>2,APOLLO-TEST-probe,\n");
  ASSERT_TRUE(sink.wait_for_count(4));
  {
    std::lock_guard lock {sink.mutex};
    EXPECT_EQ(sink.events[3], ev_t("monitorremovedv2", "2,APOLLO-TEST-probe,"));
  }
  {
    std::lock_guard lock {other.mutex};
    EXPECT_EQ(other.events.size(), 3);
  }

  listener.stop();
  EXPECT_FALSE(listener.connected());
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandEventListenerTest, StartWithoutSocketKeepsRetrying) {
  temp_dir_t tmp;
  auto path = tmp.path() / ".socket2.sock";

  hyprland::event_listener_t listener {path};
  listener.set_reconnect_interval(20ms);
  event_sink_t sink;
  listener.subscribe(std::ref(sink));
  EXPECT_FALSE(listener.start());

  fake_event_socket_t server {path};
  ASSERT_TRUE(server.wait_for_connections(1));
  for (int i = 0; i < 100 && !listener.connected(); ++i) {
    std::this_thread::sleep_for(10ms);
  }
  server.send("configreloaded>>\n");
  ASSERT_TRUE(sink.wait_for_count(1));

  listener.stop();
}

#endif
