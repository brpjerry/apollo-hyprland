/**
 * @file tests/integration/test_hyprland_live.cpp
 * @brief Test src/platform/linux/hyprland/ipc.* against a live Hyprland session, if one is running.
 *
 * Creates a headless output named APOLLO-TEST-<random>, changes its mode and removes it again.
 * Skipped when no Hyprland instance answers, or when APOLLO_SKIP_LIVE_HYPRLAND is set.
 */
#include "../tests_common.h"

#ifdef SUNSHINE_BUILD_WAYLAND

  // standard includes
  #include <condition_variable>
  #include <cstdlib>
  #include <format>
  #include <mutex>
  #include <random>
  #include <thread>

  // local includes
  #include <src/platform/linux/hyprland/ipc.h>

namespace {
  using namespace std::chrono_literals;

  /**
   * @brief Removes the output when the test ends, including on assertion failure.
   */
  struct output_guard_t {
    const hyprland::ipc_t &ipc;
    std::string name;
    bool active = false;

    ~output_guard_t() {
      if (active) {
        ipc.remove_output(name);
      }
    }
  };

  /**
   * @brief Records monitoraddedv2 / monitorremovedv2 events for one output name.
   */
  struct monitor_events_t {
    std::string name;
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::string> events;
    std::vector<hyprland::monitor_event_t> payloads;

    void operator()(std::string_view event, std::string_view data) {
      if (event != "monitoraddedv2" && event != "monitorremovedv2") {
        return;
      }
      auto parsed = hyprland::parse_monitor_event(data);
      if (!parsed || parsed->name != name) {
        return;
      }
      std::lock_guard lock {mutex};
      events.emplace_back(event);
      payloads.push_back(*parsed);
      cv.notify_all();
    }

    bool wait_for(std::string_view event) {
      std::unique_lock lock {mutex};
      return cv.wait_for(lock, 5s, [&]() {
        return std::ranges::find(events, event) != events.end();
      });
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
}  // namespace

// Bugs caught: 0, False positives: 0
TEST(HyprlandLiveTest, HeadlessOutputLifecycle) {
  if (std::getenv("APOLLO_SKIP_LIVE_HYPRLAND")) {
    GTEST_SKIP() << "APOLLO_SKIP_LIVE_HYPRLAND is set";
  }
  auto ipc = hyprland::ipc_t::connect();
  if (!ipc) {
    GTEST_SKIP() << "no live Hyprland instance found";
  }

  std::random_device rd;
  const auto name = std::format("APOLLO-TEST-{:08x}", rd());

  monitor_events_t events;
  events.name = name;
  hyprland::event_listener_t listener {ipc->event_socket_path()};
  listener.subscribe(std::ref(events));
  ASSERT_TRUE(listener.start());

  output_guard_t guard {*ipc, name};
  ASSERT_TRUE(ipc->create_headless(name));
  guard.active = true;

  ASSERT_TRUE(events.wait_for("monitoraddedv2")) << "no monitoraddedv2 for " << name;
  {
    std::lock_guard lock {events.mutex};
    EXPECT_GE(events.payloads.front().id, 0);
  }

  ASSERT_TRUE(ipc->set_monitor({.output = name, .mode = "1280x720@60", .scale = 1.0}));

  // The rule is applied on the next monitor reload, so poll for it.
  std::optional<hyprland::monitor_info_t> monitor;
  for (int i = 0; i < 50; ++i) {
    monitor = find_monitor(*ipc, name);
    if (monitor && monitor->width == 1280 && monitor->height == 720) {
      break;
    }
    std::this_thread::sleep_for(100ms);
  }
  ASSERT_TRUE(monitor) << name << " missing from monitors()";
  EXPECT_EQ(monitor->width, 1280);
  EXPECT_EQ(monitor->height, 720);
  EXPECT_NEAR(monitor->refresh_hz, 60.0, 0.5);
  EXPECT_DOUBLE_EQ(monitor->scale, 1.0);
  EXPECT_FALSE(monitor->disabled);

  ASSERT_TRUE(ipc->remove_output(name));
  guard.active = false;
  ASSERT_TRUE(events.wait_for("monitorremovedv2")) << "no monitorremovedv2 for " << name;
  EXPECT_FALSE(find_monitor(*ipc, name));

  listener.stop();
}

#endif
