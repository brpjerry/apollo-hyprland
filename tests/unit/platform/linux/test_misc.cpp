/**
 * @file tests/unit/platform/linux/test_misc.cpp
 * @brief Tests for Linux platform utility functions.
 */

#ifdef __linux__

  // test includes
  #include "../../../tests_common.h"
  #include "../../../tests_log_checker.h"

  // standard includes
  #include <array>
  #include <atomic>
  #include <cerrno>
  #include <chrono>
  #include <condition_variable>
  #include <cstdlib>
  #include <cstring>
  #include <mutex>
  #include <optional>
  #include <string>
  #include <thread>
  #include <vector>

  // system includes
  #include <arpa/inet.h>
  #include <linux/rtnetlink.h>
  #include <sys/prctl.h>
  #include <sys/resource.h>
  #include <unistd.h>

  // local includes
  #include "src/config.h"
  #include "src/logging.h"
  #include "src/platform/common.h"
  #include "src/platform/linux/misc.h"
  #include "src/utility.h"

namespace {
  /**
   * @brief Restore one environment variable when a test scope ends.
   */
  class environment_guard_t {
  public:
    /**
     * @brief Save the current value of an environment variable.
     *
     * @param name Environment variable name to restore.
     */
    explicit environment_guard_t(const char *name):
        name_ {name} {
      if (const char *value = std::getenv(name)) {
        value_ = value;
      }
    }

    /**
     * @brief Restore the saved value or remove the variable when it was originally absent.
     */
    ~environment_guard_t() {
      if (value_) {
        setenv(name_.c_str(), value_->c_str(), 1);
      } else {
        unsetenv(name_.c_str());
      }
    }

  private:
    std::string name_;  ///< Environment variable name.
    std::optional<std::string> value_;  ///< Value present before the test changed the environment.
  };

  constexpr std::array unsafe_environment_variables {
    "GDK_PIXBUF_MODULEDIR",
    "GDK_PIXBUF_MODULE_FILE",
    "GIO_EXTRA_MODULES",
    "GTK3_MODULES",
    "GTK_EXE_PREFIX",
    "GTK_IM_MODULE_FILE",
    "GTK_MODULES",
    "GTK_PATH",
    "QML2_IMPORT_PATH",
    "QML_IMPORT_PATH",
    "QT_PLUGIN_PATH",
    "QT_QPA_PLATFORM_PLUGIN_PATH",
  };

  /**
   * @brief Synchronize asynchronous test logging before changing the process environment.
   *
   * GoogleTest emits test-start records before fixture setup. Draining those records prevents the logging formatter
   * from reading environment-dependent state concurrently with `setenv()` or `unsetenv()` on older glibc releases.
   */
  class ProcessEnvironmentSecurity: public testing::Test {
  protected:
    /**
     * @brief Wait for pending test-start log records to finish formatting.
     */
    void SetUp() override {
      logging::log_flush();
    }
  };
}  // namespace

TEST_F(ProcessEnvironmentSecurity, RemovesModuleLoaders) {
  std::array<std::optional<environment_guard_t>, unsafe_environment_variables.size()> guards;
  for (std::size_t index = 0; index < unsafe_environment_variables.size(); ++index) {
    const auto *variable = unsafe_environment_variables[index];
    guards[index].emplace(variable);
    ASSERT_EQ(setenv(variable, "/tmp/untrusted-module", 1), 0);
  }

  ASSERT_TRUE(platf::sanitize_process_environment());
  for (const auto *variable : unsafe_environment_variables) {
    EXPECT_EQ(std::getenv(variable), nullptr) << variable;
  }
}

TEST_F(ProcessEnvironmentSecurity, PreservesTraySessionVariables) {
  environment_guard_t platform_theme_guard {"QT_QPA_PLATFORMTHEME"};
  environment_guard_t wayland_display_guard {"WAYLAND_DISPLAY"};
  ASSERT_EQ(setenv("QT_QPA_PLATFORMTHEME", "gtk3", 1), 0);
  ASSERT_EQ(setenv("WAYLAND_DISPLAY", "wayland-test", 1), 0);

  ASSERT_TRUE(platf::sanitize_process_environment());
  EXPECT_STREQ(std::getenv("QT_QPA_PLATFORMTHEME"), "gtk3");
  EXPECT_STREQ(std::getenv("WAYLAND_DISPLAY"), "wayland-test");
}

TEST(LinuxCaptureSessionHint, LogsOnlyWithoutSessionVariablesAndWithoutKms) {
  logging::log_flush();
  environment_guard_t wayland_guard {"WAYLAND_DISPLAY"};
  environment_guard_t display_guard {"DISPLAY"};
  const auto original_capture = config::video.capture;
  auto restore_capture = util::fail_guard([&]() {
    config::video.capture = original_capture;
  });

  ASSERT_EQ(unsetenv("WAYLAND_DISPLAY"), 0);
  ASSERT_EQ(unsetenv("DISPLAY"), 0);

  for (const char *capture : {"", "kwin", "kms"}) {
    config::video.capture = capture;
    EXPECT_EQ(platf::log_missing_graphical_session_hint(), config::video.capture != "kms") << capture;
  }
  EXPECT_TRUE(log_checker::line_contains("test_sunshine.log", "Error: Neither WAYLAND_DISPLAY nor DISPLAY is set"));

  config::video.capture = "kwin";
  ASSERT_EQ(setenv("WAYLAND_DISPLAY", "wayland-test", 1), 0);
  EXPECT_FALSE(platf::log_missing_graphical_session_hint());

  ASSERT_EQ(unsetenv("WAYLAND_DISPLAY"), 0);
  ASSERT_EQ(setenv("DISPLAY", ":1", 1), 0);
  EXPECT_FALSE(platf::log_missing_graphical_session_hint());
}

/**
 * @brief Append one netlink message to a datagram.
 *
 * @param datagram Datagram being built.
 * @param type Message type.
 * @param payload Message payload.
 */
static void append_netlink_message(std::vector<char> &datagram, std::uint16_t type, const std::vector<char> &payload) {
  nlmsghdr header {};
  header.nlmsg_len = NLMSG_LENGTH(payload.size());
  header.nlmsg_type = type;
  const auto offset = datagram.size();
  datagram.resize(offset + NLMSG_SPACE(payload.size()));
  std::memcpy(datagram.data() + offset, &header, sizeof(header));
  std::memcpy(datagram.data() + offset + NLMSG_HDRLEN, payload.data(), payload.size());
}

/**
 * @brief An RTM_NEWROUTE payload for the main IPv4 table.
 *
 * @param gateway Gateway address, or nullptr for none.
 * @param local Preferred source address.
 * @param destination_prefix Prefix length of the destination, 0 for the default route.
 */
static std::vector<char> route_payload(const char *gateway, const char *local, std::uint8_t destination_prefix = 0) {
  std::vector<char> payload(NLMSG_ALIGN(sizeof(rtmsg)));
  rtmsg route {};
  route.rtm_dst_len = destination_prefix;
  route.rtm_family = AF_INET;
  route.rtm_table = RT_TABLE_MAIN;
  std::memcpy(payload.data(), &route, sizeof(route));
  auto add = [&payload](std::uint16_t type, const char *address) {
    in_addr value {};
    inet_pton(AF_INET, address, &value);
    rtattr attribute {};
    attribute.rta_len = RTA_LENGTH(sizeof(value));
    attribute.rta_type = type;
    const auto offset = payload.size();
    payload.resize(offset + RTA_SPACE(sizeof(value)));
    std::memcpy(payload.data() + offset, &attribute, sizeof(attribute));
    std::memcpy(payload.data() + offset + RTA_LENGTH(0), &value, sizeof(value));
  };
  if (gateway) {
    add(RTA_GATEWAY, gateway);
  }
  add(RTA_PREFSRC, local);
  return payload;
}

// Bugs caught: 0, False positives: 0
TEST(GatewayRouteTest, ScansEachDatagramOnItsOwn) {
  std::string local_ip;

  // A route without a gateway, then the default route.
  std::vector<char> datagram;
  append_netlink_message(datagram, RTM_NEWROUTE, route_payload(nullptr, "10.0.0.2"));
  append_netlink_message(datagram, RTM_NEWROUTE, route_payload("192.168.1.1", "192.168.1.50"));
  EXPECT_EQ(platf::scan_gateway_routes(datagram.data(), datagram.size(), local_ip), platf::route_scan_e::found);
  EXPECT_EQ(local_ip, "192.168.1.50");

  // Nothing yet: the caller reads the next datagram (into the start of its buffer).
  local_ip.clear();
  datagram.clear();
  append_netlink_message(datagram, RTM_NEWROUTE, route_payload(nullptr, "10.0.0.2"));
  EXPECT_EQ(platf::scan_gateway_routes(datagram.data(), datagram.size(), local_ip), platf::route_scan_e::more);
  EXPECT_TRUE(local_ip.empty());

  datagram.clear();
  append_netlink_message(datagram, NLMSG_DONE, std::vector<char>(sizeof(int)));
  EXPECT_EQ(platf::scan_gateway_routes(datagram.data(), datagram.size(), local_ip), platf::route_scan_e::done);

  // The kernel sends no NLMSG_DONE after an error, so an error ends the scan.
  datagram.clear();
  append_netlink_message(datagram, NLMSG_ERROR, std::vector<char>(sizeof(nlmsgerr)));
  EXPECT_EQ(platf::scan_gateway_routes(datagram.data(), datagram.size(), local_ip), platf::route_scan_e::failed);

  // A truncated message is not read past its end.
  datagram.clear();
  append_netlink_message(datagram, RTM_NEWROUTE, route_payload("192.168.1.1", "192.168.1.50"));
  EXPECT_EQ(platf::scan_gateway_routes(datagram.data(), NLMSG_HDRLEN + 4, local_ip), platf::route_scan_e::more);
}

// Bugs caught: 0, False positives: 0
TEST(GatewayRouteTest, OnlyTheDefaultRouteCounts) {
  // A more specific route with its own gateway, listed first, is not the default gateway's interface.
  std::string local_ip;
  std::vector<char> datagram;
  append_netlink_message(datagram, RTM_NEWROUTE, route_payload("10.8.0.1", "10.8.0.5", 24));
  append_netlink_message(datagram, RTM_NEWROUTE, route_payload("192.168.1.1", "192.168.1.50"));
  EXPECT_EQ(platf::scan_gateway_routes(datagram.data(), datagram.size(), local_ip), platf::route_scan_e::found);
  EXPECT_EQ(local_ip, "192.168.1.50");
}

namespace {
  namespace tp = platf::thread_priority;

  /**
   * @brief Fake system calls for thread priority requests, counting how often each is used.
   */
  struct fake_priority_backend_t {
    tp::rtkit_result_e rtkit_result {tp::rtkit_result_e::unavailable};  ///< What RTKit answers.
    int lowest_allowed {0};  ///< Lowest nice level set_nice accepts; lower ones fail with EACCES.
    int floor {20};  ///< What nice_floor reports.
    int current {0};  ///< What current_nice reports.
    std::chrono::milliseconds rtkit_delay {0};  ///< How long RTKit takes to answer.
    std::atomic<int> rtkit_calls {0};  ///< RTKit requests made.
    std::mutex mutex;  ///< Guards set_nice_calls: several threads may request at once.
    std::vector<int> set_nice_calls;  ///< Nice levels passed to set_nice.

    /**
     * @brief Backend whose calls go to this fake.
     *
     * @return Backend for an adjuster.
     */
    tp::backend_t backend() {
      return {
        .rtkit = [this](pid_t, int, std::string &error) {
          ++rtkit_calls;
          std::this_thread::sleep_for(rtkit_delay);
          error = "The name is not activatable";
          return rtkit_result;
        },
        .set_nice = [this](pid_t, int nice) {
          std::lock_guard lock {mutex};
          set_nice_calls.push_back(nice);
          return nice < lowest_allowed ? EACCES : 0;
        },
        .nice_floor = [this]() {
          return floor;
        },
        .current_nice = [this](pid_t) {
          return current;
        },
      };
    }
  };
}  // namespace

// Bugs caught: 0, False positives: 0
TEST(ThreadPriorityTest, RtkitSetsHigherPriority) {
  fake_priority_backend_t fake;
  fake.rtkit_result = tp::rtkit_result_e::ok;
  tp::adjuster_t adjuster {fake.backend()};

  auto outcome = adjuster.apply(1, -10, "test");
  EXPECT_EQ(outcome.method, tp::method_e::rtkit);
  EXPECT_EQ(outcome.nice, -10);
  EXPECT_EQ(fake.rtkit_calls.load(), 1);
  EXPECT_TRUE(fake.set_nice_calls.empty());
}

// Bugs caught: 0, False positives: 0
TEST(ThreadPriorityTest, MissingRtkitIsAskedOnlyOnce) {
  fake_priority_backend_t fake;
  fake.lowest_allowed = -20;
  tp::adjuster_t adjuster {fake.backend()};

  EXPECT_EQ(adjuster.apply(1, -10, "first").method, tp::method_e::setpriority);
  EXPECT_EQ(adjuster.apply(2, -15, "second").method, tp::method_e::setpriority);
  EXPECT_EQ(fake.rtkit_calls.load(), 1);
  EXPECT_EQ(fake.set_nice_calls, (std::vector<int> {-10, -15}));
}

// Bugs caught: 0, False positives: 0
TEST(ThreadPriorityTest, RtkitRefusalIsRetriedOnTheNextThread) {
  fake_priority_backend_t fake;
  fake.rtkit_result = tp::rtkit_result_e::failed;
  fake.lowest_allowed = -20;
  tp::adjuster_t adjuster {fake.backend()};

  adjuster.apply(1, -10, "first");
  adjuster.apply(2, -10, "second");
  EXPECT_EQ(fake.rtkit_calls.load(), 2);
  EXPECT_EQ(fake.set_nice_calls.size(), 2u);
}

// Bugs caught: 0, False positives: 0
TEST(ThreadPriorityTest, RefusalWarnsOncePerProcess) {
  fake_priority_backend_t fake;
  tp::adjuster_t adjuster {fake.backend()};

  auto first = adjuster.apply(1, -10, "first");
  auto second = adjuster.apply(2, -15, "second");
  auto third = adjuster.apply(3, -10, "third");
  EXPECT_EQ(first.method, tp::method_e::failed);
  EXPECT_TRUE(first.warned);
  EXPECT_EQ(second.method, tp::method_e::failed);
  EXPECT_FALSE(second.warned);
  EXPECT_FALSE(third.warned);
  EXPECT_TRUE(log_checker::line_contains("test_sunshine.log", "Could not raise the priority of thread [first] to nice -10"));
}

// Bugs caught: 0, False positives: 0
TEST(ThreadPriorityTest, ClampsToRlimitNice) {
  fake_priority_backend_t fake;
  fake.lowest_allowed = -5;
  fake.floor = -5;
  tp::adjuster_t adjuster {fake.backend()};

  auto outcome = adjuster.apply(1, -15, "test");
  EXPECT_EQ(outcome.method, tp::method_e::clamped);
  EXPECT_EQ(outcome.nice, -5);
  EXPECT_FALSE(outcome.warned);
  EXPECT_EQ(fake.set_nice_calls, (std::vector<int> {-15, -5}));
}

// Bugs caught: 0, False positives: 0
TEST(ThreadPriorityTest, NoClampBelowAnInheritedPriority) {
  // A process started at nice -10, with a limit of -5: a thread that asks for -15 keeps the -10 it inherited.
  fake_priority_backend_t fake;
  fake.lowest_allowed = -5;
  fake.floor = -5;
  fake.current = -10;
  tp::adjuster_t adjuster {fake.backend()};

  EXPECT_EQ(adjuster.apply(1, -15, "test").method, tp::method_e::failed);
  EXPECT_EQ(fake.set_nice_calls, (std::vector<int> {-15}));
}

// Bugs caught: 0, False positives: 0
TEST(ThreadPriorityTest, ThreadsStartingTogetherAskMissingRtkitOnce) {
  fake_priority_backend_t fake;
  fake.lowest_allowed = -20;
  fake.rtkit_delay = std::chrono::milliseconds(50);
  tp::adjuster_t adjuster {fake.backend()};
  std::mutex mutex;

  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back([&adjuster, &mutex, i]() {
      auto outcome = adjuster.apply(i + 1, -10, "test");
      std::lock_guard lock {mutex};
      EXPECT_EQ(outcome.method, tp::method_e::setpriority);
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  EXPECT_EQ(fake.rtkit_calls.load(), 1);
}

// Bugs caught: 0, False positives: 0
TEST(ThreadPriorityTest, RtkitIsUsedOnceATransientFailurePasses) {
  fake_priority_backend_t fake;
  fake.rtkit_result = tp::rtkit_result_e::failed;
  fake.lowest_allowed = -20;
  tp::adjuster_t adjuster {fake.backend()};

  EXPECT_EQ(adjuster.apply(1, -10, "first").method, tp::method_e::setpriority);
  fake.rtkit_result = tp::rtkit_result_e::ok;
  EXPECT_EQ(adjuster.apply(2, -10, "second").method, tp::method_e::rtkit);
}

// Bugs caught: 0, False positives: 0
TEST(ThreadPriorityTest, NoClampWhenRlimitNiceAllowsNothing) {
  fake_priority_backend_t fake;
  fake.floor = 0;
  tp::adjuster_t adjuster {fake.backend()};

  EXPECT_EQ(adjuster.apply(1, -10, "test").method, tp::method_e::failed);
  EXPECT_EQ(fake.set_nice_calls, (std::vector<int> {-10}));
}

// Bugs caught: 0, False positives: 0
TEST(ThreadPriorityTest, LowerPriorityDoesNotAskRtkit) {
  fake_priority_backend_t fake;
  fake.rtkit_result = tp::rtkit_result_e::ok;
  tp::adjuster_t adjuster {fake.backend()};

  EXPECT_EQ(adjuster.apply(1, 10, "test").method, tp::method_e::setpriority);
  EXPECT_EQ(fake.rtkit_calls.load(), 0);
}

// Bugs caught: 0, False positives: 0
TEST(ThreadPriorityTest, SystemBackendSetsOnlyTheGivenThread) {
  const auto backend = tp::system_backend();
  const int main_nice = getpriority(PRIO_PROCESS, static_cast<id_t>(gettid()));
  if (main_nice >= 19) {
    GTEST_SKIP() << "The test runs at the lowest priority already";
  }

  // Raising the nice level needs no privilege. It is done from this thread to a worker that waits, so that a backend
  // which ignored the thread id (and set the calling thread) would fail.
  std::mutex mutex;
  std::condition_variable cv;
  pid_t worker_tid = 0;
  bool done = false;
  int worker_nice = -100;
  std::thread worker {[&]() {
    std::unique_lock lock {mutex};
    worker_tid = gettid();
    cv.notify_all();
    cv.wait(lock, [&]() {
      return done;
    });
    worker_nice = getpriority(PRIO_PROCESS, static_cast<id_t>(gettid()));
  }};
  int status = -1;
  {
    std::unique_lock lock {mutex};
    cv.wait(lock, [&]() {
      return worker_tid != 0;
    });
    status = backend.set_nice(worker_tid, main_nice + 1);
    EXPECT_EQ(backend.current_nice(worker_tid), main_nice + 1);
    done = true;
    cv.notify_all();
  }
  worker.join();

  EXPECT_EQ(status, 0);
  EXPECT_EQ(worker_nice, main_nice + 1);
  EXPECT_EQ(getpriority(PRIO_PROCESS, static_cast<id_t>(gettid())), main_nice);
  EXPECT_EQ(backend.current_nice(gettid()), main_nice);
  const int floor = backend.nice_floor();
  EXPECT_GE(floor, -20);
  EXPECT_LE(floor, 20);
}

// Bugs caught: 0, False positives: 0
TEST(PreciseSleepTest, NeverReturnsBeforeTheDeadline) {
  for (int i = 0; i < 200; ++i) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds((i % 10) * 50);
    platf::precise_sleep_until(deadline);
    ASSERT_GE(std::chrono::steady_clock::now(), deadline) << "sleep " << i;
  }
}

// Bugs caught: 0, False positives: 0
TEST(PreciseSleepTest, SetsTheTimerSlackOfItsThread) {
  int slack = -1;
  std::thread worker {[&]() {
    // The worker inherits the slack of this thread, which earlier sleeps may have set to 1 ns already.
    prctl(PR_SET_TIMERSLACK, 50'000UL, 0UL, 0UL, 0UL);
    platf::precise_sleep_until(std::chrono::steady_clock::now() + std::chrono::microseconds(100));
    slack = prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0);
  }};
  worker.join();
  EXPECT_EQ(slack, 1);
}

// Bugs caught: 0, False positives: 0
TEST(PreciseSleepTest, PassedDeadlineReturnsAtOnce) {
  const auto start = std::chrono::steady_clock::now();
  platf::precise_sleep_until(start - std::chrono::seconds(1));
  platf::precise_sleep_until(std::chrono::steady_clock::time_point::min());
  auto timer = platf::create_high_precision_timer();
  ASSERT_TRUE(timer);
  EXPECT_TRUE(static_cast<bool>(*timer));
  timer->sleep_for(std::chrono::nanoseconds(-1'000'000));
  // A wait of a second (or forever) is what this catches; the headroom is for preemption.
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(250));
}

// Bugs caught: 0, False positives: 0
TEST(PreciseSleepTest, TimerSleepsForTheDuration) {
  auto timer = platf::create_high_precision_timer();
  ASSERT_TRUE(timer);
  const auto start = std::chrono::steady_clock::now();
  timer->sleep_for(std::chrono::microseconds(500));
  EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::microseconds(500));
}

#endif  // __linux__
