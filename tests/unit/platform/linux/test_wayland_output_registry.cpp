/**
 * @file tests/unit/platform/linux/test_wayland_output_registry.cpp
 * @brief Follow outputs that a private in-process compositor adds, describes and removes after setup.
 */
#ifdef SUNSHINE_BUILD_WAYLAND
  // test includes
  #include "../../../tests_common.h"

  // standard includes
  #include <array>
  #include <cstdint>
  #include <list>

  // system includes
  #include <poll.h>
  #include <sys/socket.h>
  #include <wayland-server.h>

  // local includes
  #include <src/platform/linux/wayland.h>

namespace {
  /// zxdg_output_v1 event opcodes (xdg-output-unstable-v1.xml).
  enum xdg_event_e : std::uint32_t {
    XDG_LOGICAL_POSITION = 0,
    XDG_LOGICAL_SIZE = 1,
  };

  /**
   * @brief One output of the private compositor and what it tells its client.
   */
  struct server_output_t {
    int x {0};  ///< Logical position.
    int y {0};  ///< Logical position.
    int width {1920};  ///< Mode and logical width.
    int height {1080};  ///< Mode and logical height.
    bool describe_on_bind {true};  ///< Send the xdg-output position and size as soon as the client asks for them.
    wl_global *global {};  ///< The wl_output global.
    wl_resource *output {};  ///< The client's wl_output.
    wl_resource *xdg_output {};  ///< The client's zxdg_output_v1.
    bool xdg_output_destroyed {false};  ///< The client destroyed its zxdg_output_v1.

    /**
     * @brief Send the logical position and size.
     */
    void send_xdg() const {
      wl_resource_post_event(xdg_output, XDG_LOGICAL_POSITION, x, y);
      wl_resource_post_event(xdg_output, XDG_LOGICAL_SIZE, width, height);
    }

    /**
     * @brief Complete an update (xdg-output version 3 has no done event of its own).
     */
    void send_done() const {
      wl_output_send_done(output);
    }
  };

  /// Requests of zxdg_output_v1: destroy.
  struct xdg_output_impl_t {
    void (*destroy)(wl_client *, wl_resource *);
  };

  /// Requests of zxdg_output_manager_v1: destroy, get_xdg_output.
  struct xdg_output_manager_impl_t {
    void (*destroy)(wl_client *, wl_resource *);
    void (*get_xdg_output)(wl_client *, wl_resource *, std::uint32_t, wl_resource *);
  };

  const xdg_output_impl_t xdg_output_impl {
    .destroy = [](wl_client *, wl_resource *resource) {  // NOSONAR(cpp:S5008): Required by the Wayland C request signature.
      static_cast<server_output_t *>(wl_resource_get_user_data(resource))->xdg_output_destroyed = true;
      wl_resource_destroy(resource);
    },
  };

  const xdg_output_manager_impl_t xdg_output_manager_impl {
    .destroy = [](wl_client *, wl_resource *resource) {  // NOSONAR(cpp:S5008): Required by the Wayland C request signature.
      wl_resource_destroy(resource);
    },
    .get_xdg_output = [](wl_client *client, wl_resource *manager, std::uint32_t id, wl_resource *output) {  // NOSONAR(cpp:S5008): Required by the Wayland C request signature.
      auto state = static_cast<server_output_t *>(wl_resource_get_user_data(output));
      state->xdg_output = wl_resource_create(client, &zxdg_output_v1_interface, wl_resource_get_version(manager), id);
      wl_resource_set_implementation(state->xdg_output, &xdg_output_impl, state, nullptr);
      if (state->describe_on_bind) {
        state->send_xdg();
        state->send_done();
      }
    },
  };

  /**
   * @brief A compositor without a socket, and one client connected to it over a socket pair.
   */
  class WaylandOutputRegistryTest: public testing::Test {  // NOSONAR(cpp:S3656): TEST_F generates subclasses that access the shared fixture state.
  protected:
    /**
     * @brief Create the compositor with an xdg-output manager and one output, and connect the client.
     */
    void SetUp() override {
      server = wl_display_create();
      ASSERT_NE(server, nullptr);
      ASSERT_NE(wl_global_create(server, &zxdg_output_manager_v1_interface, 3, nullptr, bind_manager), nullptr);
      first = add_output({});
      ASSERT_NE(first->global, nullptr);
      std::array<int, 2> sockets {};
      ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets.data()), 0);
      ASSERT_NE(wl_client_create(server, sockets[0]), nullptr);
      client = wl_display_connect_to_fd(sockets[1]);
      ASSERT_NE(client, nullptr);
      registry = wl_display_get_registry(client);
      // What enumerate_outputs() does, on this connection.
      interface.listen(registry);
      ASSERT_TRUE(sync());
      ASSERT_NE(interface.output_manager, nullptr);
      for (auto &monitor : interface.monitors) {
        monitor->listen(interface.output_manager);
      }
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

    static void bind_manager(wl_client *client, void *, std::uint32_t version, std::uint32_t id) {  // NOSONAR(cpp:S5008): Required by the Wayland C global-bind signature.
      auto resource = wl_resource_create(client, &zxdg_output_manager_v1_interface, static_cast<int>(version), id);
      wl_resource_set_implementation(resource, &xdg_output_manager_impl, nullptr, nullptr);
    }

    static void bind_output(wl_client *client, void *data, std::uint32_t version, std::uint32_t id) {  // NOSONAR(cpp:S5008): Required by the Wayland C global-bind signature.
      auto state = static_cast<server_output_t *>(data);
      state->output = wl_resource_create(client, &wl_output_interface, static_cast<int>(version), id);
      wl_resource_set_implementation(state->output, nullptr, state, nullptr);
      wl_output_send_mode(state->output, WL_OUTPUT_MODE_CURRENT, state->width, state->height, 60000);
      wl_output_send_done(state->output);
    }

    /**
     * @brief Advertise a wl_output global.
     * @param output What the output is and how it describes itself.
     * @return The output's state, owned by the fixture.
     */
    server_output_t *add_output(const server_output_t &output) {
      auto &state = outputs.emplace_back(output);
      state.global = wl_global_create(server, &wl_output_interface, 2, &state, bind_output);
      return &state;
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
    std::list<server_output_t> outputs;  ///< Its outputs (a list: their addresses are handed to libwayland).
    server_output_t *first {};  ///< Output advertised at setup.
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
  EXPECT_EQ(monitor.geometry.logical_width, 1920);
  EXPECT_FALSE(monitor.updating);
  auto watch = wl::watch_output(interface, monitor);
  EXPECT_FALSE(wl::output_change(watch, interface));

  // wl_global_remove() announces the removal and keeps the global for clients that are still binding it.
  wl_global_remove(first->global);
  ASSERT_TRUE(sync());
  // The client destroys the xdg-output object as it reads the removal; the next sync has that request handled.
  ASSERT_TRUE(sync());

  ASSERT_EQ(interface.monitors.size(), 1u);
  EXPECT_TRUE(monitor.removed);
  EXPECT_EQ(wl::output_change(watch, interface), "the output was removed");
  // The xdg-output object is released; the monitor and its wl_output stay for the capture that holds them.
  EXPECT_TRUE(first->xdg_output_destroyed);
  EXPECT_FALSE(monitor.listening());
}

// Bugs caught: 0, False positives: 0
TEST_F(WaylandOutputRegistryTest, OutputAddedAfterSetupIsWatchedOnceDescribed) {
  ASSERT_EQ(interface.monitors.size(), 1u);
  auto watch = wl::watch_output(interface, *interface.monitors.front());
  const auto first_name = interface.monitors.front()->global_name;

  // A 4K output to the right, whose logical position and size come later than its mode.
  auto second = add_output({.x = 1920, .width = 3840, .height = 2160, .describe_on_bind = false});
  ASSERT_NE(second->global, nullptr);
  // The first sync delivers the global; the client binds it and asks for its xdg-output on the way, so the second
  // one sees those requests answered.
  ASSERT_TRUE(sync());
  ASSERT_TRUE(sync());

  ASSERT_EQ(interface.monitors.size(), 2u);
  auto &added = *interface.monitors.back();
  EXPECT_NE(added.global_name, first_name);
  EXPECT_TRUE(added.listening()) << "an output added after setup is not asked for its logical geometry";
  ASSERT_NE(second->xdg_output, nullptr);
  // Its mode alone (3840x2160 at 0,0) says nothing about where it is.
  EXPECT_FALSE(wl::output_change(watch, interface));

  // Half an update, then the done event that completes it, in separate reads.
  second->send_xdg();
  ASSERT_TRUE(sync());
  EXPECT_TRUE(added.updating);
  EXPECT_FALSE(wl::output_change(watch, interface));
  second->send_done();
  ASSERT_TRUE(sync());
  EXPECT_FALSE(added.updating);
  EXPECT_EQ(wl::output_change(watch, interface), "the output layout changed");

  // Removing the new output leaves the first one alone.
  wl_global_remove(second->global);
  ASSERT_TRUE(sync());
  ASSERT_TRUE(sync());
  EXPECT_FALSE(interface.monitors.front()->removed);
  EXPECT_TRUE(added.removed);
  EXPECT_TRUE(second->xdg_output_destroyed);
  EXPECT_FALSE(wl::output_change(watch, interface));
}

#endif
