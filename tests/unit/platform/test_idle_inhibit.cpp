/**
 * @file tests/unit/platform/test_idle_inhibit.cpp
 * @brief Test src/platform/linux/idle_inhibit.* against fake ScreenSaver and logind services on a private bus.
 */
#include "../../tests_common.h"

#ifdef SUNSHINE_BUILD_GIO

  // standard includes
  #include <array>
  #include <chrono>
  #include <condition_variable>
  #include <cstdio>
  #include <cstdlib>
  #include <filesystem>
  #include <functional>
  #include <iostream>
  #include <mutex>
  #include <string>
  #include <thread>
  #include <vector>

  // platform includes
  #include <gio/gio.h>
  #include <gio/gunixfdlist.h>
  #include <poll.h>
  #include <signal.h>
  #include <sys/wait.h>
  #include <unistd.h>

  // local includes
  #include <src/platform/linux/idle_inhibit.h>

namespace {
  namespace fs = std::filesystem;
  using namespace std::chrono_literals;

  /**
   * @brief A private dbus-daemon, killed on destruction.
   */
  class private_bus_t {
  public:
    private_bus_t() {
      // Kept short: the socket path must fit in sun_path.
      auto base = fs::temp_directory_path();
      if (base.native().size() > 40) {
        base = "/tmp";
      }
      auto tmpl = (base / "aidle-XXXXXX").string();
      if (!::mkdtemp(tmpl.data())) {
        return;
      }
      dir_ = tmpl;
      auto socket = "unix:path=" + (dir_ / "bus").string();
      std::array<int, 2> out {-1, -1};
      if (::pipe(out.data()) != 0) {
        return;
      }
      pid_ = ::fork();
      if (pid_ == 0) {
        ::dup2(out[1], STDOUT_FILENO);
        ::close(out[0]);
        ::close(out[1]);
        ::execlp("dbus-daemon", "dbus-daemon", "--session", "--nofork", "--nosyslog", "--print-address=1", ("--address=" + socket).c_str(), nullptr);
        ::_exit(127);
      }
      ::close(out[1]);
      // dbus-daemon prints its address once it listens.
      std::string line;
      pollfd pfd {out[0], POLLIN, 0};
      char c;
      while (::poll(&pfd, 1, 5000) > 0 && ::read(out[0], &c, 1) == 1 && c != '\n') {
        line += c;
      }
      ::close(out[0]);
      address_ = line;
    }

    ~private_bus_t() {
      if (pid_ > 0) {
        ::kill(pid_, SIGTERM);
        ::waitpid(pid_, nullptr, 0);
      }
      if (!dir_.empty()) {
        std::error_code ec;
        fs::remove_all(dir_, ec);
      }
    }

    const std::string &address() const {
      return address_;
    }

  private:
    fs::path dir_;
    pid_t pid_ = -1;
    std::string address_;
  };

  constexpr auto introspection_xml = R"(<node>
    <interface name="org.freedesktop.ScreenSaver">
      <method name="Inhibit"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="u" direction="out"/></method>
      <method name="UnInhibit"><arg type="u" direction="in"/></method>
    </interface>
    <interface name="org.freedesktop.login1.Manager">
      <method name="Inhibit"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="h" direction="out"/></method>
    </interface>
  </node>)";

  /**
   * @brief Serves org.freedesktop.ScreenSaver or org.freedesktop.login1 on a bus, from its own main loop thread.
   */
  class fake_service_t {
  public:
    enum kind_e {
      screensaver,
      logind,
    };

    fake_service_t(const std::string &address, kind_e kind):
        kind_ {kind} {
      thread_ = std::thread {[this, address]() {
        run(address);
      }};
      std::unique_lock lock {mutex_};
      cv_.wait_for(lock, 5s, [this]() {
        return started_;
      });
    }

    ~fake_service_t() {
      {
        std::lock_guard lock {mutex_};
        if (loop_) {
          g_main_loop_quit(loop_);
        }
      }
      thread_.join();
      if (inhibit_read_fd_ >= 0) {
        ::close(inhibit_read_fd_);
      }
    }

    bool ready() {
      std::lock_guard lock {mutex_};
      return ready_;
    }

    std::vector<std::string> calls() {
      std::lock_guard lock {mutex_};
      return calls_;
    }

    /**
     * @brief For logind: whether the inhibitor fd handed out is still open somewhere (no POLLHUP on our end).
     */
    bool inhibitor_open() {
      std::lock_guard lock {mutex_};
      if (inhibit_read_fd_ < 0) {
        return false;
      }
      pollfd pfd {inhibit_read_fd_, POLLIN, 0};
      return ::poll(&pfd, 1, 0) == 0;
    }

  private:
    void run(const std::string &address) {
      GMainContext *context = g_main_context_new();
      g_main_context_push_thread_default(context);
      GMainLoop *loop = g_main_loop_new(context, FALSE);

      auto flags = static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION);
      GDBusConnection *bus = g_dbus_connection_new_for_address_sync(address.c_str(), flags, nullptr, nullptr, nullptr);
      GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(introspection_xml, nullptr);
      bool ok = bus && node;
      guint registration = 0;
      if (ok) {
        static const GDBusInterfaceVTable vtable {&fake_service_t::method_call, nullptr, nullptr, {}};
        const char *name = kind_ == screensaver ? "org.freedesktop.ScreenSaver" : "org.freedesktop.login1";
        const char *path = kind_ == screensaver ? "/org/freedesktop/ScreenSaver" : "/org/freedesktop/login1";
        const char *iface = kind_ == screensaver ? "org.freedesktop.ScreenSaver" : "org.freedesktop.login1.Manager";
        registration = g_dbus_connection_register_object(bus, path, g_dbus_node_info_lookup_interface(node, iface), &vtable, this, nullptr, nullptr);
        GVariant *reply = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName", g_variant_new("(su)", name, 4u), G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, nullptr);
        ok = registration != 0 && reply != nullptr;
        if (reply) {
          g_variant_unref(reply);
        }
      }
      {
        std::lock_guard lock {mutex_};
        loop_ = loop;
        started_ = true;
        ready_ = ok;
      }
      cv_.notify_all();
      if (ok) {
        g_main_loop_run(loop);
      }
      {
        std::lock_guard lock {mutex_};
        loop_ = nullptr;
      }
      if (registration) {
        g_dbus_connection_unregister_object(bus, registration);
      }
      if (node) {
        g_dbus_node_info_unref(node);
      }
      if (bus) {
        g_dbus_connection_close_sync(bus, nullptr, nullptr);
        g_object_unref(bus);
      }
      g_main_loop_unref(loop);
      g_main_context_pop_thread_default(context);
      g_main_context_unref(context);
    }

    static void method_call(GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *method, GVariant *params, GDBusMethodInvocation *invocation, gpointer user_data) {
      auto *self = static_cast<fake_service_t *>(user_data);
      gchar *printed = g_variant_print(params, FALSE);
      {
        std::lock_guard lock {self->mutex_};
        self->calls_.push_back(std::string {method} + printed);
      }
      g_free(printed);

      if (self->kind_ == screensaver) {
        if (std::string_view {method} == "Inhibit") {
          g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", 42u));
        } else {
          g_dbus_method_invocation_return_value(invocation, nullptr);
        }
        return;
      }

      // logind: hand out the write end of a pipe and keep the read end, like logind keeps its FIFO.
      int fds[2];
      if (::pipe(fds) != 0) {
        g_dbus_method_invocation_return_dbus_error(invocation, "org.freedesktop.DBus.Error.Failed", "pipe");
        return;
      }
      GUnixFDList *list = g_unix_fd_list_new();
      int index = g_unix_fd_list_append(list, fds[1], nullptr);
      ::close(fds[1]);
      {
        std::lock_guard lock {self->mutex_};
        self->inhibit_read_fd_ = fds[0];
      }
      g_dbus_method_invocation_return_value_with_unix_fd_list(invocation, g_variant_new("(h)", index), list);
      g_object_unref(list);
    }

    kind_e kind_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    GMainLoop *loop_ = nullptr;
    bool started_ = false;
    bool ready_ = false;
    std::vector<std::string> calls_;
    int inhibit_read_fd_ = -1;
  };

  bool wait_until(const std::function<bool()> &condition) {
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!condition()) {
      if (std::chrono::steady_clock::now() > deadline) {
        return false;
      }
      std::this_thread::sleep_for(10ms);
    }
    return true;
  }
}  // namespace

// Bugs caught: 0, False positives: 0
TEST(IdleInhibitTest, HoldsScreenSaverAndLogindUntilReleased) {
  private_bus_t session;
  private_bus_t system;
  if (session.address().empty() || system.address().empty()) {
    GTEST_SKIP() << "dbus-daemon is not available";
  }
  fake_service_t screensaver {session.address(), fake_service_t::screensaver};
  fake_service_t logind {system.address(), fake_service_t::logind};
  ASSERT_TRUE(screensaver.ready());
  ASSERT_TRUE(logind.ready());

  platf::idle::inhibitor_t inhibitor {{
    .session_bus_address = session.address(),
    .system_bus_address = system.address(),
  }};
  ASSERT_TRUE(inhibitor.acquire());
  EXPECT_TRUE(inhibitor.screensaver_held());
  EXPECT_TRUE(inhibitor.logind_held());
  EXPECT_TRUE(logind.inhibitor_open());

  // A second acquire (another client starting) takes nothing new.
  EXPECT_TRUE(inhibitor.acquire());

  inhibitor.release();
  EXPECT_FALSE(inhibitor.screensaver_held());
  EXPECT_FALSE(inhibitor.logind_held());
  EXPECT_TRUE(wait_until([&]() {
    return !logind.inhibitor_open();
  }));

  std::vector<std::string> expected_screensaver {
    "Inhibit('Apollo', 'Streaming to a Moonlight client')",
    "UnInhibit(42,)",
  };
  EXPECT_EQ(screensaver.calls(), expected_screensaver);
  std::vector<std::string> expected_logind {
    "Inhibit('idle', 'Apollo', 'Streaming to a Moonlight client', 'block')",
  };
  EXPECT_EQ(logind.calls(), expected_logind);

  // Releasing twice is harmless.
  inhibitor.release();
}

// Bugs caught: 0, False positives: 0
TEST(IdleInhibitTest, MissingServicesAreSkipped) {
  private_bus_t session;
  private_bus_t system;
  if (session.address().empty() || system.address().empty()) {
    GTEST_SKIP() << "dbus-daemon is not available";
  }
  // Only logind answers, as on a compositor without a ScreenSaver service.
  fake_service_t logind {system.address(), fake_service_t::logind};
  ASSERT_TRUE(logind.ready());

  platf::idle::inhibitor_t inhibitor {{
    .session_bus_address = session.address(),
    .system_bus_address = system.address(),
    .timeout = 1000ms,
  }};
  EXPECT_TRUE(inhibitor.acquire());
  EXPECT_FALSE(inhibitor.screensaver_held());
  EXPECT_TRUE(inhibitor.logind_held());
  inhibitor.release();

  // No bus at all: nothing held, no crash.
  platf::idle::inhibitor_t nothing {{
    .session_bus_address = "unix:path=/nonexistent/apollo-bus",
    .system_bus_address = "unix:path=/nonexistent/apollo-bus",
  }};
  EXPECT_FALSE(nothing.acquire());
  nothing.release();
}

// Bugs caught: 0, False positives: 0
TEST(IdleInhibitTest, DroppingTheConnectionReleasesScreenSaver) {
  private_bus_t session;
  if (session.address().empty()) {
    GTEST_SKIP() << "dbus-daemon is not available";
  }
  fake_service_t screensaver {session.address(), fake_service_t::screensaver};
  ASSERT_TRUE(screensaver.ready());
  {
    platf::idle::inhibitor_t inhibitor {{
      .session_bus_address = session.address(),
      .use_logind = false,
    }};
    ASSERT_TRUE(inhibitor.acquire());
    EXPECT_TRUE(inhibitor.screensaver_held());
    // The destructor releases.
  }
  std::vector<std::string> expected {
    "Inhibit('Apollo', 'Streaming to a Moonlight client')",
    "UnInhibit(42,)",
  };
  EXPECT_EQ(screensaver.calls(), expected);
}

/**
 * @brief Against the real session and system buses: the logind inhibitor shows up in `systemd-inhibit --list`.
 * @details Opt-in (APOLLO_TEST_REAL_IDLE_INHIBIT=1): it inhibits the desktop session's idle for a moment, which
 *          keeps it awake but never blanks or locks it.
 */
// Bugs caught: 0, False positives: 0
TEST(IdleInhibitTest, RealBusesListTheInhibitor) {
  if (!std::getenv("APOLLO_TEST_REAL_IDLE_INHIBIT")) {
    GTEST_SKIP() << "set APOLLO_TEST_REAL_IDLE_INHIBIT=1 to inhibit idle on the real buses";
  }
  const std::string why = "Apollo unit test " + std::to_string(::getpid());
  auto listed = [&]() {
    std::string out;
    if (FILE *p = ::popen("systemd-inhibit --list --no-pager --no-legend 2>/dev/null", "r")) {
      char buf[512];
      while (std::fgets(buf, sizeof(buf), p)) {
        out += buf;
      }
      ::pclose(p);
    }
    return out.find(why) != std::string::npos;
  };

  platf::idle::inhibitor_t inhibitor {{.why = why}};
  ASSERT_TRUE(inhibitor.acquire());
  EXPECT_TRUE(inhibitor.logind_held());
  EXPECT_TRUE(listed());
  std::cout << "ScreenSaver inhibitor held: " << inhibitor.screensaver_held() << std::endl;
  inhibitor.release();
  EXPECT_TRUE(wait_until([&]() {
    return !listed();
  }));
}

#endif
