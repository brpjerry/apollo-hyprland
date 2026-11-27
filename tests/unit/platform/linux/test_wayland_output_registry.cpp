/**
 * @file tests/unit/platform/linux/test_wayland_output_registry.cpp
 * @brief Follow outputs that a private in-process compositor adds and removes after setup.
 */
#ifdef SUNSHINE_BUILD_WAYLAND
  // test includes
  #include "../../../tests_common.h"

  // standard includes
  #include <array>
  #include <cstdint>

  // system includes
  #include <poll.h>
  #include <sys/socket.h>
  #include <wayland-server.h>

  // local includes
  #include <src/platform/linux/wayland.h>

namespace {
  /**
   * @brief A compositor without a socket, and one client connected to it over a socket pair.
   */
  class WaylandOutputRegistryTest: public testing::Test {  // NOSONAR(cpp:S3656): TEST_F generates subclasses that access the shared fixture state.
  protected:
    /**
     * @brief Create the compositor with one output and connect the client.
     */
    void SetUp() override {
      server = wl_display_create();
      ASSERT_NE(server, nullptr);
      first = add_output();
      ASSERT_NE(first, nullptr);
      std::array<int, 2> sockets {};
      ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets.data()), 0);
      ASSERT_NE(wl_client_create(server, sockets[0]), nullptr);
      client = wl_display_connect_to_fd(sockets[1]);
      ASSERT_NE(client, nullptr);
      registry = wl_display_get_registry(client);
      interface.listen(registry);
      ASSERT_TRUE(sync());
    }

    /**
     * @brief Disconnect the client and destroy the compositor.
     */
    void TearDown() override {
      if (registry) {
        wl_registry_destroy(registry);
      }
      if (client) {
        wl_display_disconnect(client);
      }
      if (server) {
        wl_display_destroy_clients(server);
        wl_display_destroy(server);
      }
    }

    /**
     * @brief Advertise a wl_output global.
     * @return The global.
     */
    wl_global *add_output() {
      return wl_global_create(server, &wl_output_interface, 2, nullptr, [](wl_client *client, void *, std::uint32_t version, std::uint32_t id) {  // NOSONAR(cpp:S5008): Required by the Wayland C global-bind signature.
        wl_resource_create(client, &wl_output_interface, static_cast<int>(version), id);
      });
    }

    /**
     * @brief Pump both ends until a wl_display.sync callback arrives.
     * @return True when the callback completes without a protocol error or timeout.
     */
    bool sync() {
      bool done = false;
      auto callback = wl_display_sync(client);
      static const wl_callback_listener listener {
        [](void *data, wl_callback *callback, std::uint32_t) {  // NOSONAR(cpp:S5008): Required by the Wayland C listener signature.
          *static_cast<bool *>(data) = true;
          wl_callback_destroy(callback);
        },
      };
      wl_callback_add_listener(callback, &listener, &done);
      while (!done) {
        if (wl_display_flush(client) < 0 || wl_event_loop_dispatch(wl_display_get_event_loop(server), 0) < 0) {
          return false;
        }
        wl_display_flush_clients(server);
        pollfd fd {wl_display_get_fd(client), POLLIN, 0};
        if (poll(&fd, 1, 1000) <= 0 || wl_display_dispatch(client) < 0) {
          return false;
        }
      }
      return wl_display_get_error(client) == 0;
    }

    wl_display *server {};  ///< In-process compositor.
    wl_global *first {};  ///< Output advertised at setup.
    wl_display *client {};  ///< Client connection.
    wl_registry *registry {};  ///< Client registry.
    wl::interface_t interface;  ///< Registry state under test.
  };
}  // namespace

// Bugs caught: 0, False positives: 0
TEST_F(WaylandOutputRegistryTest, RemovedOutputIsMarkedAndReported) {
  ASSERT_EQ(interface.monitors.size(), 1u);
  auto &monitor = *interface.monitors.front();
  EXPECT_NE(monitor.global_name, 0u);
  EXPECT_FALSE(monitor.removed);
  auto watch = wl::watch_output(interface, monitor);
  EXPECT_FALSE(wl::output_change(watch, interface));

  // wl_global_remove() announces the removal and keeps the global for clients that are still binding it.
  wl_global_remove(first);
  ASSERT_TRUE(sync());

  ASSERT_EQ(interface.monitors.size(), 1u);
  EXPECT_TRUE(monitor.removed);
  EXPECT_EQ(wl::output_change(watch, interface), "the output was removed");
}

// Bugs caught: 0, False positives: 0
TEST_F(WaylandOutputRegistryTest, OutputAddedAfterSetupIsTracked) {
  ASSERT_EQ(interface.monitors.size(), 1u);
  const auto first_name = interface.monitors.front()->global_name;

  auto second = add_output();
  ASSERT_NE(second, nullptr);
  ASSERT_TRUE(sync());

  ASSERT_EQ(interface.monitors.size(), 2u);
  EXPECT_NE(interface.monitors.back()->global_name, first_name);

  // Removing the new output leaves the first one alone.
  wl_global_remove(second);
  ASSERT_TRUE(sync());
  EXPECT_FALSE(interface.monitors.front()->removed);
  EXPECT_TRUE(interface.monitors.back()->removed);
}

#endif
