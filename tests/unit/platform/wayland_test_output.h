/**
 * @file tests/unit/platform/wayland_test_output.h
 * @brief Helpers for live Wayland capture tests on Hyprland: a temporary headless output, and a layer-shell
 *        client that redraws an output every frame so damage-driven capture has something to capture.
 */
#pragma once

#if defined(__linux__) && defined(SUNSHINE_BUILD_WAYLAND)

  // standard includes
  #include <algorithm>
  #include <atomic>
  #include <chrono>
  #include <cstdlib>
  #include <cstring>
  #include <memory>
  #include <optional>
  #include <random>
  #include <string>
  #include <thread>

  // platform includes
  #include <poll.h>
  #include <xf86drm.h>
  #include <single-pixel-buffer-v1.h>
  #include <viewporter.h>
  #include <wayland-client.h>
  // The generated header names a parameter "namespace".
  #define namespace namespace_
  #include <wlr-layer-shell-unstable-v1.h>
  #undef namespace

  // local includes
  #include <src/platform/linux/hyprland/ipc.h>

namespace test_wl {
  using namespace std::chrono_literals;

  /**
   * @brief Whether the tests run in the private headless Hyprland of scripts/hyprland/test-live.sh, which gives it its
   *        own XDG_RUNTIME_DIR. Tests that create or reconfigure outputs must skip otherwise: in a desktop session they
   *        would change the user's compositor.
   */
  inline bool in_private_hyprland() {
    const char *private_dir = std::getenv("APOLLO_HEADLESS_HYPRLAND_DIR");
    const char *runtime_dir = std::getenv("XDG_RUNTIME_DIR");
    return private_dir && runtime_dir && std::string {private_dir} == runtime_dir && std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
  }

  /**
   * @brief The first DRM render node, or "" without one: no GPU, e.g. CI's vkms, which has only a primary node.
   *        Video memory capture and the GL converter need a render node.
   */
  inline std::string drm_render_node() {
    drmDevicePtr devices[16];
    int count = drmGetDevices2(0, devices, 16);
    std::string node;
    for (int i = 0; i < count && node.empty(); ++i) {
      if (devices[i]->available_nodes & (1 << DRM_NODE_RENDER)) {
        node = devices[i]->nodes[DRM_NODE_RENDER];
      }
    }
    drmFreeDevices(devices, std::max(count, 0));
    return node;
  }

  /**
   * @brief A headless Hyprland output named APOLLO-TEST-<suffix>, removed again when this object is destroyed
   *        (also when a test assertion fails).
   */
  class headless_output_t {
  public:
    /**
     * @param mode Hyprland mode string, e.g. "3840x2160@120".
     */
    static std::unique_ptr<headless_output_t> create(const std::string &mode) {
      auto ipc = hyprland::ipc_t::connect();
      if (!ipc) {
        return nullptr;
      }

      std::random_device rd;
      auto name = "APOLLO-TEST-" + std::to_string(rd() % 100000);

      if (!ipc->create_headless(name)) {
        return nullptr;
      }

      std::unique_ptr<headless_output_t> output {new headless_output_t(std::move(ipc), name)};

      // Far to the right of any real monitor, so the test output does not disturb the desktop layout.
      hyprland::monitor_rule_t rule;
      rule.output = name;
      rule.mode = mode;
      rule.position = "20000x0";
      rule.scale = 1.0;
      if (!output->ipc->set_monitor(rule)) {
        return nullptr;
      }

      // Wait until the compositor applied the mode.
      auto deadline = std::chrono::steady_clock::now() + 3s;
      while (std::chrono::steady_clock::now() < deadline) {
        for (const auto &monitor : output->ipc->monitors()) {
          if (monitor.name == name && mode.starts_with(std::to_string(monitor.width) + "x" + std::to_string(monitor.height))) {
            return output;
          }
        }
        std::this_thread::sleep_for(20ms);
      }

      return nullptr;
    }

    ~headless_output_t() {
      ipc->remove_output(name);
    }

    /// Removes the output now (e.g. to test what capture does when its output disappears).
    void remove() {
      ipc->remove_output(name);
    }

    std::unique_ptr<hyprland::ipc_t> ipc;
    std::string name;

  private:
    headless_output_t(std::unique_ptr<hyprland::ipc_t> ipc, std::string name):
        ipc {std::move(ipc)},
        name {std::move(name)} {
    }
  };

  /**
   * @brief Covers an output with a background layer surface whose color changes on every frame callback,
   *        so the whole output is damaged at its refresh rate. Runs its own Wayland connection on a thread.
   */
  class damage_source_t {
  public:
    explicit damage_source_t(const std::string &output_name):
        target_name {output_name} {
      thread = std::thread {[this]() {
        run();
      }};

      auto deadline = std::chrono::steady_clock::now() + 3s;
      while (!running && !failed && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
      }

      // Let the compositor's layer fade-in animation finish, so captured frames show only our colors.
      if (running) {
        std::this_thread::sleep_for(750ms);
      }
    }

    ~damage_source_t() {
      stop = true;
      thread.join();
    }

    bool ok() const {
      return running && !failed;
    }

    std::atomic<std::uint64_t> frames {0};

  private:
    struct globals_t {
      wl_compositor *compositor {nullptr};
      zwlr_layer_shell_v1 *layer_shell {nullptr};
      wp_viewporter *viewporter {nullptr};
      wp_single_pixel_buffer_manager_v1 *single_pixel {nullptr};
      std::vector<std::pair<wl_output *, std::string>> outputs;
    };

    static void output_geometry(void *, wl_output *, std::int32_t, std::int32_t, std::int32_t, std::int32_t, std::int32_t, const char *, const char *, std::int32_t) {}

    static void output_mode(void *, wl_output *, std::uint32_t, std::int32_t, std::int32_t, std::int32_t) {}

    static void output_done(void *, wl_output *) {}

    static void output_scale(void *, wl_output *, std::int32_t) {}

    static void output_name(void *data, wl_output *output, const char *name) {
      auto globals = static_cast<globals_t *>(data);
      for (auto &[o, n] : globals->outputs) {
        if (o == output) {
          n = name;
        }
      }
    }

    static void output_description(void *, wl_output *, const char *) {}

    static constexpr wl_output_listener output_listener {
      .geometry = output_geometry,
      .mode = output_mode,
      .done = output_done,
      .scale = output_scale,
      .name = output_name,
      .description = output_description,
    };

    static void global(void *data, wl_registry *registry, std::uint32_t id, const char *interface, std::uint32_t version) {
      auto globals = static_cast<globals_t *>(data);
      if (!std::strcmp(interface, wl_compositor_interface.name)) {
        globals->compositor = (wl_compositor *) wl_registry_bind(registry, id, &wl_compositor_interface, 4);
      } else if (!std::strcmp(interface, zwlr_layer_shell_v1_interface.name)) {
        globals->layer_shell = (zwlr_layer_shell_v1 *) wl_registry_bind(registry, id, &zwlr_layer_shell_v1_interface, 1);
      } else if (!std::strcmp(interface, wp_viewporter_interface.name)) {
        globals->viewporter = (wp_viewporter *) wl_registry_bind(registry, id, &wp_viewporter_interface, 1);
      } else if (!std::strcmp(interface, wp_single_pixel_buffer_manager_v1_interface.name)) {
        globals->single_pixel = (wp_single_pixel_buffer_manager_v1 *) wl_registry_bind(registry, id, &wp_single_pixel_buffer_manager_v1_interface, 1);
      } else if (!std::strcmp(interface, wl_output_interface.name) && version >= 4) {
        auto output = (wl_output *) wl_registry_bind(registry, id, &wl_output_interface, 4);
        globals->outputs.emplace_back(output, "");
        wl_output_add_listener(output, &output_listener, globals);
      }
    }

    static void global_remove(void *, wl_registry *, std::uint32_t) {}

    static constexpr wl_registry_listener registry_listener {
      .global = global,
      .global_remove = global_remove,
    };

    struct surface_state_t {
      bool configured {false};
      bool closed {false};
      bool frame_due {true};
      std::uint32_t width {0};
      std::uint32_t height {0};
    };

    static void layer_configure(void *data, zwlr_layer_surface_v1 *surface, std::uint32_t serial, std::uint32_t width, std::uint32_t height) {
      auto state = static_cast<surface_state_t *>(data);
      zwlr_layer_surface_v1_ack_configure(surface, serial);
      state->configured = true;
      state->width = width;
      state->height = height;
    }

    static void layer_closed(void *data, zwlr_layer_surface_v1 *) {
      static_cast<surface_state_t *>(data)->closed = true;
    }

    static constexpr zwlr_layer_surface_v1_listener layer_listener {
      .configure = layer_configure,
      .closed = layer_closed,
    };

    static void frame_done(void *data, wl_callback *callback, std::uint32_t) {
      wl_callback_destroy(callback);
      static_cast<surface_state_t *>(data)->frame_due = true;
    }

    static constexpr wl_callback_listener frame_listener {
      .done = frame_done,
    };

    void run() {
      auto display = wl_display_connect(nullptr);
      if (!display) {
        failed = true;
        return;
      }

      globals_t globals;
      auto registry = wl_display_get_registry(display);
      wl_registry_add_listener(registry, &registry_listener, &globals);
      wl_display_roundtrip(display);
      wl_display_roundtrip(display);

      wl_output *output = nullptr;
      for (auto &[o, n] : globals.outputs) {
        if (n == target_name) {
          output = o;
        }
      }

      if (!globals.compositor || !globals.layer_shell || !globals.viewporter || !globals.single_pixel || !output) {
        failed = true;
        wl_display_disconnect(display);
        return;
      }

      surface_state_t state;
      auto surface = wl_compositor_create_surface(globals.compositor);
      auto viewport = wp_viewporter_get_viewport(globals.viewporter, surface);
      auto layer = zwlr_layer_shell_v1_get_layer_surface(globals.layer_shell, surface, output, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "apollo-test-damage");
      zwlr_layer_surface_v1_add_listener(layer, &layer_listener, &state);
      zwlr_layer_surface_v1_set_anchor(layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
      zwlr_layer_surface_v1_set_size(layer, 0, 0);
      zwlr_layer_surface_v1_set_exclusive_zone(layer, -1);
      wl_surface_commit(surface);
      wl_display_roundtrip(display);

      if (!state.configured) {
        failed = true;
      }

      // Two opaque colors, alternated every frame.
      wl_buffer *buffers[2] = {
        wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(globals.single_pixel, 0xffffffffu, 0, 0, 0xffffffffu),
        wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(globals.single_pixel, 0, 0, 0xffffffffu, 0xffffffffu),
      };

      while (!stop && !failed && !state.closed) {
        if (state.frame_due) {
          state.frame_due = false;
          wp_viewport_set_destination(viewport, (std::int32_t) std::max(1u, state.width), (std::int32_t) std::max(1u, state.height));
          wl_surface_attach(surface, buffers[frames % 2], 0, 0);
          wl_surface_damage_buffer(surface, 0, 0, 1, 1);
          auto callback = wl_surface_frame(surface);
          wl_callback_add_listener(callback, &frame_listener, &state);
          wl_surface_commit(surface);
          ++frames;
          running = true;
        }

        wl_display_flush(display);
        pollfd pfd {wl_display_get_fd(display), POLLIN, 0};
        if (poll(&pfd, 1, 50) > 0) {
          if (wl_display_dispatch(display) < 0) {
            failed = true;
          }
        } else {
          wl_display_dispatch_pending(display);
        }
      }

      zwlr_layer_surface_v1_destroy(layer);
      wp_viewport_destroy(viewport);
      wl_surface_destroy(surface);
      wl_buffer_destroy(buffers[0]);
      wl_buffer_destroy(buffers[1]);
      wl_display_roundtrip(display);
      wl_display_disconnect(display);
    }

    std::string target_name;
    std::thread thread;
    std::atomic<bool> stop {false};
    std::atomic<bool> running {false};
    std::atomic<bool> failed {false};
  };
}  // namespace test_wl

#endif
