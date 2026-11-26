/**
 * @file tests/unit/platform/test_wayland_display.cpp
 * @brief Test wl::display_t (src/platform/linux/wayland.*) against a socket that is not a compositor.
 */
#include "../../tests_common.h"

#if defined(__linux__) && defined(SUNSHINE_BUILD_WAYLAND)

  #include <src/platform/linux/wayland.h>

  #include <chrono>
  #include <cstdio>
  #include <cstdlib>
  #include <filesystem>
  #include <string>
  #include <sys/socket.h>
  #include <sys/un.h>
  #include <unistd.h>

namespace {
  using namespace std::literals;

  /**
   * @brief A listening Unix socket in a temporary directory, standing in for a compositor that accepts the
   *        connection and then goes away.
   */
  struct fake_compositor_t {
    std::filesystem::path dir;
    std::filesystem::path path;
    int listener = -1;

    fake_compositor_t() {
      auto tmpl = (std::filesystem::temp_directory_path() / "awl-XXXXXX").string();
      if (!mkdtemp(tmpl.data())) {
        return;
      }
      dir = tmpl;
      path = dir / "wayland-test";
      listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      sockaddr_un addr {};
      addr.sun_family = AF_UNIX;
      std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
      if (bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0 || listen(listener, 1) < 0) {
        close(listener);
        listener = -1;
      }
    }

    ~fake_compositor_t() {
      if (listener >= 0) {
        close(listener);
      }
      std::error_code ec;
      std::filesystem::remove_all(dir, ec);
    }

    /// Accepts the pending connection; the caller closes it to hang up.
    int accept_client() const {
      return accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
    }
  };
}  // namespace

// Bugs caught: 0, False positives: 0
TEST(WaylandDisplayTest, DispatchReportsAClosedConnection) {
  fake_compositor_t compositor;
  ASSERT_GE(compositor.listener, 0);

  wl::display_t display;
  ASSERT_EQ(display.init(compositor.path.c_str()), 0);
  int peer = compositor.accept_client();
  ASSERT_GE(peer, 0);

  // A live connection with nothing to read only times out.
  EXPECT_FALSE(display.dispatch(10ms));
  EXPECT_FALSE(display.broken());

  close(peer);
  // The hangup is seen at once, not after the timeout, and stays visible to the capture loops.
  auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(display.dispatch(5s));
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
  EXPECT_TRUE(display.broken());
  EXPECT_FALSE(display.dispatch(5s)) << "a broken connection must not report dispatched events";
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
}

#endif
