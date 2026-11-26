/**
 * @file tests/integration/test_abs_input_live.cpp
 * @brief Inject absolute mouse, pen, touch and relative mouse input on a live Hyprland session and check where it
 * lands.
 *
 * Moves the user's pointer, so it only runs with APOLLO_TEST_INJECT_INPUT=1 (and needs a live Hyprland, a Wayland
 * display with wlr-screencopy and write access to /dev/uinput). It creates headless outputs named
 * APOLLO-TEST-<random>, touches only those, and restores the pointer position and focused monitor afterwards.
 *
 * Optional: APOLLO_TEST_TOUCH_OUTPUT=<an existing APOLLO-TEST-* output> also taps the middle and the quarter points
 * of that output, e.g. to read the positions from a `wev` window placed on it.
 */
#include "../tests_common.h"

#if defined(__linux__) && defined(SUNSHINE_BUILD_WAYLAND)

  // standard includes
  #include <cmath>
  #include <cstdlib>
  #include <format>
  #include <random>
  #include <thread>
  #include <unistd.h>

  // local includes
  #include <moonlight-common-c/src/Limelight.h>
  #include <src/input.h>
  #include <src/platform/common.h>
  #include <src/platform/linux/hyprland/ipc.h>
  #include <src/platform/linux/input/inputtino_abs.h>
  #include <src/video.h>

namespace video {
  input::touch_port_t make_port(platf::display_t *display, const config_t &config);
}

namespace {
  using namespace std::chrono_literals;

  struct point_t {
    double x, y;
  };

  std::optional<point_t> cursor(const hyprland::ipc_t &ipc) {
    auto json = ipc.request_json("j/cursorpos");
    if (!json || !json->contains("x") || !json->contains("y")) {
      return std::nullopt;
    }
    return point_t {(*json)["x"].get<double>(), (*json)["y"].get<double>()};
  }

  std::optional<std::string> focused_monitor(const hyprland::ipc_t &ipc) {
    for (const auto &m : ipc.monitors(false)) {
      if (m.focused) {
        return m.name;
      }
    }
    return std::nullopt;
  }

  std::optional<hyprland::monitor_info_t> find_monitor(const hyprland::ipc_t &ipc, const std::string &name) {
    for (auto &m : ipc.monitors()) {
      if (m.name == name) {
        return m;
      }
    }
    return std::nullopt;
  }

  bool move_cursor(const hyprland::ipc_t &ipc, double x, double y) {
    if (ipc.dialect() == hyprland::config_dialect_e::lua) {
      return ipc.dispatch_lua(std::format("hl.dsp.cursor.move({{ x = {}, y = {} }})", std::lround(x), std::lround(y)));
    }
    return ipc.dispatch_legacy("movecursor", std::format("{} {}", std::lround(x), std::lround(y)));
  }

  bool focus_monitor(const hyprland::ipc_t &ipc, const std::string &name) {
    if (ipc.dialect() == hyprland::config_dialect_e::lua) {
      return ipc.dispatch_lua(std::format("hl.dsp.focus({{ monitor = {} }})", hyprland::lua_quote(name)));
    }
    return ipc.dispatch_legacy("focusmonitor", name);
  }

  /**
   * @brief Call `inject(lx, ly)` until the pointer is within 1 px of `want`.
   * @details A new device takes a moment to reach the compositor, and the kernel drops repeated events with an
   * unchanged value, so retries first inject a point 2 px away.
   * @return The last pointer position.
   */
  template<class F>
  std::optional<point_t> inject_until(const hyprland::ipc_t &ipc, point_t want, float lx, float ly, F &&inject) {
    std::optional<point_t> pos;
    auto deadline = std::chrono::steady_clock::now() + 3s;
    for (bool retry = false; std::chrono::steady_clock::now() < deadline; retry = true) {
      if (retry) {
        inject(lx < 2 ? lx + 2 : lx - 2, ly < 2 ? ly + 2 : ly - 2);
        std::this_thread::sleep_for(20ms);
      }
      inject(lx, ly);
      for (int i = 0; i < 10; ++i) {
        std::this_thread::sleep_for(20ms);
        pos = cursor(ipc);
        if (pos && std::abs(pos->x - want.x) <= 1 && std::abs(pos->y - want.y) <= 1) {
          return pos;
        }
      }
    }
    return pos;
  }

  struct AbsInputLiveTest: PlatformTestSuite {
    void SetUp() override {
      const char *inject = std::getenv("APOLLO_TEST_INJECT_INPUT");
      if (!inject || std::string_view {inject} != "1") {
        GTEST_SKIP() << "set APOLLO_TEST_INJECT_INPUT=1 to inject input into the live session";
      }
      if (!std::getenv("WAYLAND_DISPLAY")) {
        GTEST_SKIP() << "WAYLAND_DISPLAY is not set";
      }
      if (access("/dev/uinput", W_OK) != 0) {
        GTEST_SKIP() << "no write access to /dev/uinput";
      }
      ipc = hyprland::ipc_t::connect();
      if (!ipc) {
        GTEST_SKIP() << "no live Hyprland instance found";
      }

      saved_cursor = cursor(*ipc);
      saved_focus = focused_monitor(*ipc);

      std::random_device rd;
      auto tag = rd();
      for (auto suffix : {"a", "b"}) {
        auto name = std::format("APOLLO-TEST-{:08x}{}", tag, suffix);
        ASSERT_TRUE(ipc->create_headless(name)) << name;
        outputs.emplace_back(name);
        ASSERT_TRUE(wait_for_output(name, 1920, 1.0)) << name << " did not appear";
      }

      input = platf::input();
      client = platf::allocate_client_input_context(input);
    }

    void TearDown() override {
      client.reset();
      input.reset();
      if (!ipc) {
        return;
      }
      for (const auto &name : outputs) {
        ipc->remove_output(name);
      }
      if (saved_focus) {
        focus_monitor(*ipc, *saved_focus);
      }
      if (saved_cursor) {
        move_cursor(*ipc, saved_cursor->x, saved_cursor->y);
      }
    }

    bool wait_for_output(const std::string &name, int width, double scale) {
      for (int i = 0; i < 50; ++i) {
        auto m = find_monitor(*ipc, name);
        if (m && m->width == width && m->scale == scale) {
          return true;
        }
        std::this_thread::sleep_for(100ms);
      }
      return false;
    }

    /**
     * @brief The port input would use while streaming `name`, through the real capture and video code.
     */
    std::optional<platf::touch_port_t> port_for(const std::string &name, int *logical_w, int *logical_h) {
      video::config_t config {};
      config.width = 1920;
      config.height = 1080;
      config.framerate = 60;
      auto disp = platf::display(platf::mem_type_e::system, name, config);
      if (!disp) {
        return std::nullopt;
      }
      config.width = disp->width;
      config.height = disp->height;
      auto tp = video::make_port(disp.get(), config);
      *logical_w = tp.logical_width;
      *logical_h = tp.logical_height;
      return input::make_abs_port(tp);
    }

    /**
     * @brief Absolute mouse and pen hover at points of `name`; checks the pointer lands within 1 logical px.
     */
    void check_output(const std::string &name) {
      int w = 0;
      int h = 0;
      auto port = port_for(name, &w, &h);
      ASSERT_TRUE(port) << name;
      std::cout << name << ": logical " << w << 'x' << h << " at " << port->offset_x << ',' << port->offset_y
                << ", layout " << port->width << 'x' << port->height << " at " << port->layout_x << ','
                << port->layout_y << std::endl;

      const std::pair<double, double> points[] {{0, 0}, {0.25, 0.75}, {0.5, 0.5}, {1, 1}};
      for (auto [fx, fy] : points) {
        // Display-local coordinates, as input::client_to_touchport() returns them; stay on the last pixel.
        float lx = std::min<float>(fx * w, w - 1);
        float ly = std::min<float>(fy * h, h - 1);
        point_t want {port->offset_x + lx, port->offset_y + ly};

        auto pos = inject_until(*ipc, want, lx, ly, [&](float x, float y) {
          platf::abs_mouse(input, *port, x, y);
        });
        ASSERT_TRUE(pos);
        std::cout << "  mouse " << lx << ',' << ly << " -> " << pos->x << ',' << pos->y << " (want " << want.x << ','
                  << want.y << ')' << std::endl;
        EXPECT_NEAR(pos->x, want.x, 1) << name << " mouse";
        EXPECT_NEAR(pos->y, want.y, 1) << name << " mouse";

        // Park the pointer 40 px away (towards the middle of the output) so the pen has to move it.
        move_cursor(*ipc, want.x + (fx < 0.5 ? 40 : -40), want.y + (fy < 0.5 ? 40 : -40));

        pos = inject_until(*ipc, want, lx, ly, [&](float x, float y) {
          platf::pen_input_t pen {
            LI_TOUCH_EVENT_HOVER,
            LI_TOOL_TYPE_PEN,
            0,
            LI_TILT_UNKNOWN,
            LI_ROT_UNKNOWN,
            x / port->width,
            y / port->height,
            0.5f,
            0,
            0,
          };
          platf::pen_update(client.get(), *port, pen);
        });
        ASSERT_TRUE(pos);
        std::cout << "  pen   " << lx << ',' << ly << " -> " << pos->x << ',' << pos->y << std::endl;
        EXPECT_NEAR(pos->x, want.x, 1) << name << " pen";
        EXPECT_NEAR(pos->y, want.y, 1) << name << " pen";
      }
    }

    void tap(const platf::touch_port_t &port, float lx, float ly) {
      platf::touch_input_t touch {LI_TOUCH_EVENT_DOWN, 0, 0, lx / port.width, ly / port.height, 0.5f, 0, 0};
      platf::touch_update(client.get(), port, touch);
      std::this_thread::sleep_for(50ms);
      touch.eventType = LI_TOUCH_EVENT_UP;
      platf::touch_update(client.get(), port, touch);
      std::this_thread::sleep_for(50ms);
    }

    std::unique_ptr<hyprland::ipc_t> ipc;
    std::optional<point_t> saved_cursor;
    std::optional<std::string> saved_focus;
    std::vector<std::string> outputs;
    platf::input_t input;
    std::unique_ptr<platf::client_input_t> client;
  };
}  // namespace

// Bugs caught: 0, False positives: 0
TEST_F(AbsInputLiveTest, AbsoluteMouseAndPenLandOnStreamedOutput) {
  // Every physical output (moves the pointer only), then the test output at scale 1 and 1.5.
  for (const auto &m : ipc->monitors(false)) {
    if (!m.name.starts_with("APOLLO")) {
      check_output(m.name);
    }
  }
  check_output(outputs[0]);

  ASSERT_TRUE(ipc->set_monitor({.output = outputs[0], .scale = 1.5}));
  ASSERT_TRUE(wait_for_output(outputs[0], 1920, 1.5));
  check_output(outputs[0]);
}

// Bugs caught: 0, False positives: 0
TEST_F(AbsInputLiveTest, RelativeMouseIsOneToOne) {
  int w = 0;
  int h = 0;
  auto port = port_for(outputs[0], &w, &h);
  ASSERT_TRUE(port);
  point_t start {port->offset_x + w / 2.0, port->offset_y + h / 2.0};
  auto pos = inject_until(*ipc, start, w / 2.0f, h / 2.0f, [&](float x, float y) {
    platf::abs_mouse(input, *port, x, y);
  });
  ASSERT_TRUE(pos);
  ASSERT_NEAR(pos->x, start.x, 1);

  point_t want {pos->x + 37, pos->y - 23};
  platf::move_mouse(input, 37, -23);
  auto deadline = std::chrono::steady_clock::now() + 1s;
  std::optional<point_t> moved;
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(20ms);
    moved = cursor(*ipc);
    if (moved && moved->x == want.x && moved->y == want.y) {
      break;
    }
  }
  ASSERT_TRUE(moved);
  EXPECT_EQ(moved->x, want.x);
  EXPECT_EQ(moved->y, want.y);
}

// Bugs caught: 0, False positives: 0
TEST_F(AbsInputLiveTest, TouchIsBoundToStreamedOutput) {
  // Unbound, Hyprland sends a touch to the focused monitor. Focus output A and touch output B: the touch only reaches
  // B (and focuses it) if the device was bound to B. If binding failed it lands on A, never on a real monitor.
  ASSERT_TRUE(focus_monitor(*ipc, outputs[0]));
  ASSERT_EQ(focused_monitor(*ipc), outputs[0]);

  int w = 0;
  int h = 0;
  auto port = port_for(outputs[1], &w, &h);
  ASSERT_TRUE(port);

  std::optional<std::string> focused;
  for (int i = 0; i < 20 && focused != outputs[1]; ++i) {
    tap(*port, w / 2.0f, h / 2.0f);
    focused = focused_monitor(*ipc);
  }
  EXPECT_EQ(focused, outputs[1]);

  if (const char *extra = std::getenv("APOLLO_TEST_TOUCH_OUTPUT")) {
    std::string name {extra};
    ASSERT_TRUE(name.starts_with("APOLLO-TEST-")) << "APOLLO_TEST_TOUCH_OUTPUT must name an APOLLO-TEST-* output";
    port = port_for(name, &w, &h);
    ASSERT_TRUE(port) << name;
    for (auto [fx, fy] : {std::pair {0.5f, 0.5f}, {0.25f, 0.75f}, {0.75f, 0.25f}}) {
      std::cout << name << ": touch at " << fx * w << ',' << fy * h << " (global " << port->offset_x + fx * w << ','
                << port->offset_y + fy * h << ')' << std::endl;
      tap(*port, fx * w, fy * h);
    }
    EXPECT_EQ(focused_monitor(*ipc), name);
  }
}

#endif
