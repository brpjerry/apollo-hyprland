/**
 * @file tests/unit/platform/test_virtual_keyboard_live.cpp
 * @brief Unicode text through zwp_virtual_keyboard_v1 (src/platform/linux/input/virtual_keyboard.cpp) against a
 *        private headless Hyprland.
 * @details These tests type into whatever has keyboard focus, so they only run inside the instance that
 *          scripts/hyprland/test-live.sh (run-headless-hyprland.sh) starts, never against a desktop session: they
 *          skip unless APOLLO_HEADLESS_HYPRLAND_DIR names a directory with the instance marker that is also
 *          XDG_RUNTIME_DIR, and WAYLAND_DISPLAY is a socket name inside it. A layer-shell surface with exclusive
 *          keyboard interactivity takes the focus and records what a client receives.
 */
#include "../../tests_common.h"

#if defined(__linux__) && defined(SUNSHINE_BUILD_WAYLAND)

  #include <src/platform/linux/hyprland/ipc.h>
  #include <src/platform/linux/input/unicode_keymap.h>
  #include <src/platform/linux/input/virtual_keyboard.h>

  #include <atomic>
  #include <cstring>
  #include <filesystem>
  #include <functional>
  #include <mutex>
  #include <optional>
  #include <poll.h>
  #include <single-pixel-buffer-v1.h>
  #include <csignal>
  #include <fcntl.h>
  #include <fstream>
  #include <sstream>
  #include <sys/mman.h>
  #include <sys/wait.h>
  #include <thread>
  #include <unistd.h>
  #include <viewporter.h>
  #include <wayland-client.h>
  #include <xkbcommon/xkbcommon.h>
  // The generated header names a parameter "namespace".
  #define namespace namespace_
  #include <wlr-layer-shell-unstable-v1.h>
  #undef namespace

using namespace std::literals;

namespace {
  /**
   * @brief Absolute path of the private instance's Wayland socket, or an explanation why there is none.
   */
  std::optional<std::string> private_wayland_socket(std::string &why) {
    const char *dir = std::getenv("APOLLO_HEADLESS_HYPRLAND_DIR");
    const char *runtime = std::getenv("XDG_RUNTIME_DIR");
    const char *display = std::getenv("WAYLAND_DISPLAY");
    if (!dir || !*dir) {
      why = "not inside a private headless Hyprland (run scripts/hyprland/test-live.sh)";
      return std::nullopt;
    }
    std::error_code ec;
    if (!std::filesystem::exists(std::filesystem::path {dir} / ".apollo-headless-hyprland", ec)) {
      why = "APOLLO_HEADLESS_HYPRLAND_DIR has no instance marker";
      return std::nullopt;
    }
    if (!runtime || std::filesystem::path {runtime} != std::filesystem::path {dir}) {
      why = "XDG_RUNTIME_DIR is not the private instance's directory";
      return std::nullopt;
    }
    if (!display || !*display || std::string_view {display}.find('/') != std::string_view::npos || std::getenv("WAYLAND_SOCKET")) {
      why = "WAYLAND_DISPLAY does not name a socket of the private instance";
      return std::nullopt;
    }
    auto socket = std::filesystem::path {dir} / display;
    if (!std::filesystem::exists(socket, ec)) {
      why = "the private instance's Wayland socket is gone";
      return std::nullopt;
    }
    return socket.string();
  }

  /**
   * @brief A focused client: an overlay layer surface with exclusive keyboard interactivity that records the
   *        keymaps, modifiers and text its wl_keyboard receives. Runs its own connection on a thread.
   */
  class key_recorder_t {
  public:
    struct key_t {
      std::uint32_t keysym;
      std::string utf8;
      std::uint32_t mods;  ///< Depressed + latched + locked modifiers when the key was pressed
    };

    explicit key_recorder_t(const std::string &socket):
        socket {socket} {
      thread = std::thread {[this]() {
        run();
      }};
    }

    ~key_recorder_t() {
      stop = true;
      thread.join();
    }

    /// Waits until the surface has keyboard focus.
    bool wait_focus(std::chrono::milliseconds timeout) {
      return wait([this]() {
        return focused.load();
      },
                  timeout);
    }

    bool wait(const std::function<bool()> &done, std::chrono::milliseconds timeout) {
      auto deadline = std::chrono::steady_clock::now() + timeout;
      while (std::chrono::steady_clock::now() < deadline) {
        if (done()) {
          return true;
        }
        std::this_thread::sleep_for(10ms);
      }
      return done();
    }

    std::string text() {
      std::lock_guard lg {lock};
      std::string out;
      for (auto &k : keys) {
        out += k.utf8;
      }
      return out;
    }

    std::vector<key_t> pressed() {
      std::lock_guard lg {lock};
      return keys;
    }

    /// Keymaps received, oldest first.
    std::vector<std::string> keymaps() {
      std::lock_guard lg {lock};
      return keymap_texts;
    }

    std::atomic<bool> failed {false};
    std::atomic<bool> focused {false};

  private:
    wl_keyboard *keyboard {nullptr};

    struct globals_t {
      wl_compositor *compositor {nullptr};
      zwlr_layer_shell_v1 *layer_shell {nullptr};
      wp_viewporter *viewporter {nullptr};
      wp_single_pixel_buffer_manager_v1 *single_pixel {nullptr};
      wl_seat *seat {nullptr};
    };

    static void on_keymap(void *data, wl_keyboard *, std::uint32_t format, std::int32_t fd, std::uint32_t size) {
      auto self = static_cast<key_recorder_t *>(data);
      if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        void *map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (map != MAP_FAILED) {
          std::string text(static_cast<const char *>(map), strnlen(static_cast<const char *>(map), size));
          munmap(map, size);
          std::lock_guard lg {self->lock};
          if (self->state) {
            xkb_state_unref(self->state);
            self->state = nullptr;
          }
          if (self->keymap) {
            xkb_keymap_unref(self->keymap);
          }
          self->keymap = xkb_keymap_new_from_string(self->ctx, text.c_str(), XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
          if (self->keymap) {
            self->state = xkb_state_new(self->keymap);
          } else {
            self->failed = true;
          }
          self->keymap_texts.push_back(std::move(text));
        }
      }
      close(fd);
    }

    static void on_key(void *data, wl_keyboard *, std::uint32_t, std::uint32_t, std::uint32_t key, std::uint32_t key_state) {
      auto self = static_cast<key_recorder_t *>(data);
      std::lock_guard lg {self->lock};
      if (key_state != WL_KEYBOARD_KEY_STATE_PRESSED || !self->state) {
        return;
      }
      char buf[32] {};
      xkb_state_key_get_utf8(self->state, key + 8, buf, sizeof(buf));
      self->keys.push_back({xkb_state_key_get_one_sym(self->state, key + 8), buf, self->mods});
    }

    static void on_modifiers(void *data, wl_keyboard *, std::uint32_t, std::uint32_t depressed, std::uint32_t latched, std::uint32_t locked, std::uint32_t group) {
      auto self = static_cast<key_recorder_t *>(data);
      std::lock_guard lg {self->lock};
      self->mods = depressed | latched | locked;
      if (self->state) {
        xkb_state_update_mask(self->state, depressed, latched, locked, 0, 0, group);
      }
    }

    static constexpr wl_keyboard_listener keyboard_listener {
      .keymap = on_keymap,
      .enter = [](void *data, wl_keyboard *, std::uint32_t, wl_surface *, wl_array *) {
        static_cast<key_recorder_t *>(data)->focused = true;
      },
      .leave = [](void *data, wl_keyboard *, std::uint32_t, wl_surface *) {
        static_cast<key_recorder_t *>(data)->focused = false;
      },
      .key = on_key,
      .modifiers = on_modifiers,
      .repeat_info = [](void *, wl_keyboard *, std::int32_t, std::int32_t) {},
    };

    static constexpr wl_seat_listener seat_listener {
      .capabilities = [](void *data, wl_seat *seat, std::uint32_t caps) {
        auto self = static_cast<key_recorder_t *>(data);
        if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !self->keyboard) {
          self->keyboard = wl_seat_get_keyboard(seat);
          wl_keyboard_add_listener(self->keyboard, &keyboard_listener, self);
        }
      },
      .name = [](void *, wl_seat *, const char *) {},
    };

    static constexpr wl_registry_listener registry_listener {
      .global = [](void *data, wl_registry *registry, std::uint32_t id, const char *interface, std::uint32_t) {
        auto g = static_cast<globals_t *>(data);
        if (!std::strcmp(interface, wl_compositor_interface.name)) {
          g->compositor = (wl_compositor *) wl_registry_bind(registry, id, &wl_compositor_interface, 4);
        } else if (!std::strcmp(interface, zwlr_layer_shell_v1_interface.name)) {
          g->layer_shell = (zwlr_layer_shell_v1 *) wl_registry_bind(registry, id, &zwlr_layer_shell_v1_interface, 1);
        } else if (!std::strcmp(interface, wp_viewporter_interface.name)) {
          g->viewporter = (wp_viewporter *) wl_registry_bind(registry, id, &wp_viewporter_interface, 1);
        } else if (!std::strcmp(interface, wp_single_pixel_buffer_manager_v1_interface.name)) {
          g->single_pixel = (wp_single_pixel_buffer_manager_v1 *) wl_registry_bind(registry, id, &wp_single_pixel_buffer_manager_v1_interface, 1);
        } else if (!std::strcmp(interface, wl_seat_interface.name) && !g->seat) {
          g->seat = (wl_seat *) wl_registry_bind(registry, id, &wl_seat_interface, 5);
        }
      },
      .global_remove = [](void *, wl_registry *, std::uint32_t) {},
    };

    static constexpr zwlr_layer_surface_v1_listener layer_listener {
      .configure = [](void *data, zwlr_layer_surface_v1 *surface, std::uint32_t serial, std::uint32_t, std::uint32_t) {
        zwlr_layer_surface_v1_ack_configure(surface, serial);
        *static_cast<bool *>(data) = true;
      },
      .closed = [](void *, zwlr_layer_surface_v1 *) {},
    };

    void run() {
      ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
      auto display = wl_display_connect(socket.c_str());
      if (!display || !ctx) {
        failed = true;
        return;
      }
      globals_t g;
      auto registry = wl_display_get_registry(display);
      wl_registry_add_listener(registry, &registry_listener, &g);
      wl_display_roundtrip(display);
      if (!g.compositor || !g.layer_shell || !g.viewporter || !g.single_pixel || !g.seat) {
        failed = true;
        wl_display_disconnect(display);
        return;
      }
      wl_seat_add_listener(g.seat, &seat_listener, this);

      bool configured = false;
      auto surface = wl_compositor_create_surface(g.compositor);
      auto viewport = wp_viewporter_get_viewport(g.viewporter, surface);
      auto layer = zwlr_layer_shell_v1_get_layer_surface(g.layer_shell, surface, nullptr, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "apollo-test-keys");
      zwlr_layer_surface_v1_add_listener(layer, &layer_listener, &configured);
      zwlr_layer_surface_v1_set_size(layer, 64, 64);
      zwlr_layer_surface_v1_set_keyboard_interactivity(layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
      wl_surface_commit(surface);
      wl_display_roundtrip(display);
      auto buffer = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(g.single_pixel, 0, 0xffffffffu, 0, 0xffffffffu);
      wp_viewport_set_destination(viewport, 64, 64);
      wl_surface_attach(surface, buffer, 0, 0);
      wl_surface_damage_buffer(surface, 0, 0, 1, 1);
      wl_surface_commit(surface);

      while (!stop && !failed) {
        wl_display_flush(display);
        pollfd pfd {wl_display_get_fd(display), POLLIN, 0};
        if (poll(&pfd, 1, 20) > 0) {
          if (wl_display_dispatch(display) < 0) {
            failed = true;
          }
        } else {
          wl_display_dispatch_pending(display);
        }
      }

      if (keyboard) {
        wl_keyboard_release(keyboard);
      }
      zwlr_layer_surface_v1_destroy(layer);
      wp_viewport_destroy(viewport);
      wl_surface_destroy(surface);
      wl_buffer_destroy(buffer);
      wl_display_roundtrip(display);
      wl_display_disconnect(display);
      std::lock_guard lg {lock};
      if (state) {
        xkb_state_unref(state);
      }
      if (keymap) {
        xkb_keymap_unref(keymap);
      }
      xkb_context_unref(ctx);
    }

    std::string socket;
    std::thread thread;
    std::atomic<bool> stop {false};

    std::mutex lock;
    xkb_context *ctx {nullptr};
    xkb_keymap *keymap {nullptr};
    xkb_state *state {nullptr};
    std::uint32_t mods {0};
    std::vector<key_t> keys;
    std::vector<std::string> keymap_texts;
  };

  std::string read_file(const std::filesystem::path &path) {
    std::ifstream in {path};
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
  }

  bool have(const char *program) {
    return std::system(("command -v "s + program + " > /dev/null 2>&1").c_str()) == 0;
  }

  /**
   * @brief A child process whose stdout goes to a file, killed when this object goes away.
   */
  struct child_t {
    pid_t pid = -1;
    std::filesystem::path out;

    child_t(const std::vector<std::string> &argv, std::filesystem::path out_path):
        out {std::move(out_path)} {
      pid = fork();
      if (pid == 0) {
        int fd = open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        dup2(fd, STDOUT_FILENO);
        int null = open("/dev/null", O_RDWR);
        dup2(null, STDIN_FILENO);
        dup2(null, STDERR_FILENO);
        std::vector<char *> args;
        for (auto &a : argv) {
          args.push_back(const_cast<char *>(a.c_str()));
        }
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
      }
    }

    /// Waits for the process to exit by itself; returns false on timeout.
    bool wait_exit(std::chrono::milliseconds timeout) {
      auto deadline = std::chrono::steady_clock::now() + timeout;
      while (std::chrono::steady_clock::now() < deadline) {
        int status;
        if (waitpid(pid, &status, WNOHANG) == pid) {
          pid = -1;
          return true;
        }
        std::this_thread::sleep_for(20ms);
      }
      return false;
    }

    ~child_t() {
      if (pid > 0) {
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
      }
    }
  };

  /// Waits until the private instance's focused window has class @p window_class.
  bool wait_active_window(const std::string &window_class, std::chrono::milliseconds timeout) {
    auto ipc = hyprland::ipc_t::connect();
    if (!ipc) {
      return false;
    }
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      auto json = ipc->request_json("j/activewindow");
      if (json && json->is_object() && json->value("class", "") == window_class) {
        return true;
      }
      std::this_thread::sleep_for(50ms);
    }
    return false;
  }

  class VirtualKeyboardLiveTest: public ::testing::Test {
  protected:
    void SetUp() override {
      std::string why;
      auto s = private_wayland_socket(why);
      if (!s) {
        GTEST_SKIP() << why;
      }
      socket = *s;
    }

    std::string socket;
  };
}  // namespace

// Bugs caught: 0, False positives: 0
TEST_F(VirtualKeyboardLiveTest, TypesUnicodeTextIntoTheFocusedClient) {
  auto vk = platf::keyboard::virtual_keyboard_t::connect(socket.c_str());
  ASSERT_TRUE(vk) << "no zwp_virtual_keyboard_manager_v1";
  EXPECT_FALSE(platf::keyboard::unicode_keymap::is_ours(vk->restore_keymap()));

  // The headless instance has no other keyboard, so the seat gains one only with our virtual keyboard.
  key_recorder_t recorder {socket};
  ASSERT_TRUE(recorder.wait_focus(5s)) << "the recorder surface never got keyboard focus";
  ASSERT_FALSE(recorder.failed);
  auto uploads_before = vk->keymap_uploads();
  auto keymaps_before = recorder.keymaps().size();

  const std::u32string text = U"héllo 世界 🎮";
  ASSERT_TRUE(vk->type(text));
  ASSERT_TRUE(recorder.wait([&]() {
    return recorder.text() == "héllo 世界 🎮";
  },
                            5s))
    << "received: " << recorder.text();

  // One keymap for the whole event, then the normal keymap again.
  EXPECT_EQ(vk->keymap_uploads() - uploads_before, 2u);
  ASSERT_TRUE(recorder.wait([&]() {
    auto maps = recorder.keymaps();
    return !maps.empty() && !platf::keyboard::unicode_keymap::is_ours(maps.back());
  },
                            2s))
    << "the normal keymap was not restored";
  auto maps = recorder.keymaps();
  std::size_t ours = 0;
  for (auto i = keymaps_before; i < maps.size(); ++i) {
    ours += platf::keyboard::unicode_keymap::is_ours(maps[i]);
  }
  EXPECT_EQ(ours, 1u);

  // Exact keysyms, and no modifiers held (nothing that could trigger a key binding).
  auto keys = recorder.pressed();
  ASSERT_EQ(keys.size(), 10u);
  EXPECT_EQ(keys[1].keysym, (std::uint32_t) XKB_KEY_eacute);
  EXPECT_EQ(keys[6].keysym, 0x01004E16u);
  EXPECT_EQ(keys[9].keysym, 0x0101F3AEu);
  for (auto &k : keys) {
    EXPECT_EQ(k.mods, 0u) << k.utf8;
  }
}

// Bugs caught: 0, False positives: 0
TEST_F(VirtualKeyboardLiveTest, TypesIntoToolkitClients) {
  // Real toolkits turn keysyms into text themselves (GTK's input method, kitty's own key handling).
  struct client_t {
    std::string window_class;
    std::vector<std::string> argv;
  };
  auto dir = std::filesystem::temp_directory_path() / ("apollo-vk-" + std::to_string(getpid()));
  std::filesystem::create_directories(dir);
  auto kitty_out = dir / "kitty.txt";
  std::vector<client_t> clients;
  if (have("zenity")) {
    clients.push_back({"zenity", {"zenity", "--entry", "--text=apollo"}});
  }
  if (have("kitty")) {
    clients.push_back({"kitty", {"kitty", "--config=NONE", "--class=kitty", "sh", "-c", "IFS= read -r line; printf %s \"$line\" > " + kitty_out.string()}});
  }
  if (clients.empty()) {
    GTEST_SKIP() << "neither zenity nor kitty is installed";
  }

  auto vk = platf::keyboard::virtual_keyboard_t::connect(socket.c_str());
  ASSERT_TRUE(vk);
  for (auto &client : clients) {
    SCOPED_TRACE(client.window_class);
    child_t child {client.argv, dir / (client.window_class + ".out")};
    ASSERT_GT(child.pid, 0);
    if (!wait_active_window(client.window_class, 15s)) {
      ADD_FAILURE() << client.window_class << " never got focus";
      continue;
    }
    std::this_thread::sleep_for(500ms);  // Let the toolkit focus its text field
    ASSERT_TRUE(vk->type(U"héllo 世界 🎮"));
    ASSERT_TRUE(vk->type(U"\n"));
    ASSERT_TRUE(child.wait_exit(10s)) << client.window_class << " did not get Return";
    auto got = client.window_class == "kitty" ? read_file(kitty_out) : read_file(child.out);
    while (!got.empty() && got.back() == '\n') {
      got.pop_back();
    }
    EXPECT_EQ(got, "héllo 世界 🎮");
  }
  std::filesystem::remove_all(dir);
}

// Bugs caught: 0, False positives: 0
TEST_F(VirtualKeyboardLiveTest, TypesEventsBackToBackAndLongText) {
  auto vk = platf::keyboard::virtual_keyboard_t::connect(socket.c_str());
  ASSERT_TRUE(vk);
  key_recorder_t recorder {socket};
  ASSERT_TRUE(recorder.wait_focus(5s));

  // Separate events in quick succession, like a soft keyboard sends them, and one event that needs two keymaps.
  std::u32string expected;
  for (auto word : {U"Grüße"sv, U" "sv, U"Ελληνικά"sv, U" "sv, U"😀👍"sv, U"\n"sv}) {
    ASSERT_TRUE(vk->type(word));
    expected += word;
  }
  std::u32string many;
  for (char32_t c = 0x4E00; c < 0x4E00 + 260; ++c) {
    many += c;
  }
  auto uploads_before = vk->keymap_uploads();
  ASSERT_TRUE(vk->type(many));
  EXPECT_EQ(vk->keymap_uploads() - uploads_before, 3u);  // two batches and the restore
  expected += many;

  std::string expected_utf8;
  for (auto c : expected) {
    char buf[8] {};
    xkb_keysym_to_utf8(platf::keyboard::unicode_keymap::keysym_for(c), buf, sizeof(buf));
    expected_utf8 += buf;
  }
  EXPECT_TRUE(recorder.wait([&]() {
    return recorder.text() == expected_utf8;
  },
                            10s))
    << "received: " << recorder.text();
}

#endif
