/**
 * @file tests/integration/test_touch_pen_uinput.cpp
 * @brief A client's touch screen and pen tablet (libvirtualhid's Linux devices, driven through platf::touch_update()
 *        and platf::pen_update()) as libinput sees them.
 * @details Creates real uinput devices, so it only runs with APOLLO_TEST_UINPUT=1, write access to /dev/uinput and
 *          read access to /dev/input/event* (the input group). Each device is grabbed (EVIOCGRAB) before any event
 *          is written, so its events reach only this test's libinput context, never the compositor or the desktop.
 *          The devices (and the shared keyboard and mouse platf::input() creates) still appear in the desktop's
 *          device list while the test runs.
 */
#include "../tests_common.h"

#if defined(__linux__) && defined(TESTS_HAVE_LIBINPUT)

  // standard includes
  #include <algorithm>
  #include <cerrno>
  #include <chrono>
  #include <cmath>
  #include <cstdlib>
  #include <cstring>
  #include <thread>
  #include <vector>

  // platform includes
  #include <fcntl.h>
  #include <linux/input.h>
  #include <sys/ioctl.h>
  #include <unistd.h>

  // lib includes
  #include <libinput.h>
  #include <libudev.h>

  // local includes
  #include <moonlight-common-c/src/Limelight.h>
  #include <src/platform/linux/input/abs_input.h>
  #include <src/platform/virtualhid_input.h>

using namespace std::literals;

namespace {
  /**
   * @brief A grabbed libvirtualhid device read through a private libinput path context.
   */
  struct grabbed_t {
    int fd = -1;
    libinput *li = nullptr;

    static int open_restricted(const char *, int, void *data) {
      int fd = dup(*static_cast<int *>(data));
      return fd < 0 ? -errno : fd;
    }

    static void close_restricted(int fd, void *) {
      close(fd);
    }

    static constexpr libinput_interface iface {open_restricted, close_restricted};

    grabbed_t(const lvh::VirtualDevice &device, std::string &why) {
      std::string node;
      // The node appears, and gets its group permissions from udev, shortly after creation.
      for (int i = 0; i < 100 && node.empty(); ++i) {
        for (const auto &n : device.device_nodes()) {
          if (n.kind == lvh::DeviceNodeKind::input_event) {
            node = n.path;
          }
        }
        if (node.empty()) {
          std::this_thread::sleep_for(20ms);
        }
      }
      if (node.empty()) {
        why = "no event node for " + device.profile().name;
        return;
      }
      for (int i = 0; i < 100 && fd < 0; ++i) {
        fd = open(node.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
          std::this_thread::sleep_for(20ms);
        }
      }
      if (fd < 0) {
        why = "cannot open " + node + ": " + std::strerror(errno);
        return;
      }
      if (ioctl(fd, EVIOCGRAB, 1) != 0) {
        why = "cannot grab " + node;
        close(fd);
        fd = -1;
        return;
      }
      // libinput's path backend wants the udev properties (ID_INPUT_*).
      std::system("udevadm settle -t 5 > /dev/null 2>&1");
      li = libinput_path_create_context(&iface, &fd);
      if (!li || !libinput_path_add_device(li, node.c_str())) {
        why = "libinput does not accept " + node;
        return;
      }
      drain();
    }

    ~grabbed_t() {
      if (li) {
        libinput_unref(li);
      }
      if (fd >= 0) {
        ioctl(fd, EVIOCGRAB, 0);
        close(fd);
      }
    }

    bool ok() const {
      return li != nullptr;
    }

    // Runs `write` and returns what libinput makes of the events it sends.
    template<class W, class F>
    void send(W &&write, F &&each) {
      write();
      auto deadline = std::chrono::steady_clock::now() + 200ms;
      bool got = false;
      while (std::chrono::steady_clock::now() < deadline) {
        libinput_dispatch(li);
        while (auto ev = libinput_get_event(li)) {
          got = true;
          each(ev);
          libinput_event_destroy(ev);
        }
        if (got) {
          // Give a multi-frame batch a moment to arrive completely.
          std::this_thread::sleep_for(10ms);
          libinput_dispatch(li);
          while (auto ev = libinput_get_event(li)) {
            each(ev);
            libinput_event_destroy(ev);
          }
          return;
        }
        std::this_thread::sleep_for(5ms);
      }
    }

    template<class W>
    std::vector<libinput_event_type> types(W &&write) {
      std::vector<libinput_event_type> out;
      send(write, [&](libinput_event *ev) {
        if (libinput_event_get_type(ev) != LIBINPUT_EVENT_TOUCH_FRAME) {
          out.push_back(libinput_event_get_type(ev));
        }
      });
      return out;
    }

    void drain() {
      libinput_dispatch(li);
      while (auto ev = libinput_get_event(li)) {
        libinput_event_destroy(ev);
      }
    }
  };

  class TouchPenUinputTest: public ::testing::Test {
  protected:
    void SetUp() override {
      const char *enabled = std::getenv("APOLLO_TEST_UINPUT");
      if (!enabled || std::string_view {enabled} != "1") {
        GTEST_SKIP() << "set APOLLO_TEST_UINPUT=1 to create (grabbed) uinput devices";
      }
      if (access("/dev/uinput", W_OK) != 0) {
        GTEST_SKIP() << "no write access to /dev/uinput";
      }
      // No compositor: the devices stay unbound, and the test talks to nothing but its own libinput context.
      platf::abs_input::set_compositor_for_testing(nullptr);
      input = platf::input();
      client = platf::allocate_client_input_context(input);
    }

    void TearDown() override {
      client.reset();
      input.reset();
    }

    platf::virtualhid::client_context_t &context() {
      return platf::virtualhid::get_client_context(client.get());
    }

    std::unique_ptr<grabbed_t> grab(const lvh::VirtualDevice *device) {
      if (!device) {
        ADD_FAILURE() << "libvirtualhid created no device";
        return nullptr;
      }
      std::string why;
      auto g = std::make_unique<grabbed_t>(*device, why);
      if (!g->ok()) {
        ADD_FAILURE() << why;
        return nullptr;
      }
      return g;
    }

    platf::input_t input;
    std::unique_ptr<platf::client_input_t> client;
    const platf::touch_port_t port {0, 0, 1920, 1080, 1920, 1080};
  };

  platf::touch_input_t finger(std::uint8_t type, std::uint32_t id, float x = 0.5f, float y = 0.5f) {
    return {type, LI_ROT_UNKNOWN, id, x, y, 0.4f, 0, 0};
  }

  platf::pen_input_t stylus(std::uint8_t type, float pressure_or_distance) {
    return {type, LI_TOOL_TYPE_UNKNOWN, 0, 45, 0, 0.25f, 0.25f, pressure_or_distance, 0, 0};
  }
}  // namespace

// Bugs caught: 0, False positives: 0
TEST_F(TouchPenUinputTest, TouchScreenThroughLibinput) {
  auto g = grab(context().touch.get());
  ASSERT_TRUE(g);
  auto touch = [&](const platf::touch_input_t &t) {
    return [&, t] {
      platf::touch_update(client.get(), port, t);
    };
  };

  // Hover: nothing at all.
  EXPECT_TRUE(g->types(touch(finger(LI_TOUCH_EVENT_HOVER, 1))).empty());

  double x = -1;
  g->send(touch(finger(LI_TOUCH_EVENT_DOWN, 1)), [&](libinput_event *ev) {
    if (libinput_event_get_type(ev) == LIBINPUT_EVENT_TOUCH_DOWN) {
      x = libinput_event_touch_get_x_transformed(libinput_event_get_touch_event(ev), 1920);
    }
  });
  EXPECT_NEAR(x, 960.0, 1.0);
  EXPECT_EQ(g->types(touch(finger(LI_TOUCH_EVENT_MOVE, 1, 0.6f))), std::vector {LIBINPUT_EVENT_TOUCH_MOTION});
  // Hovering again lifts the finger.
  EXPECT_EQ(g->types(touch(finger(LI_TOUCH_EVENT_HOVER, 1))), std::vector {LIBINPUT_EVENT_TOUCH_UP});

  // Two fingers, then cancel-all: both are lifted (libvirtualhid lifts rather than cancels them).
  EXPECT_EQ(g->types(touch(finger(LI_TOUCH_EVENT_DOWN, 2))), std::vector {LIBINPUT_EVENT_TOUCH_DOWN});
  EXPECT_EQ(g->types(touch(finger(LI_TOUCH_EVENT_DOWN, 3, 0.6f))), std::vector {LIBINPUT_EVENT_TOUCH_DOWN});
  auto lifted = g->types(touch(finger(LI_TOUCH_EVENT_CANCEL_ALL, 0)));
  EXPECT_EQ(lifted.size(), 2u);

  // The slots work again afterwards.
  EXPECT_EQ(g->types(touch(finger(LI_TOUCH_EVENT_DOWN, 4))), std::vector {LIBINPUT_EVENT_TOUCH_DOWN});
  EXPECT_EQ(g->types(touch(finger(LI_TOUCH_EVENT_UP, 4))), std::vector {LIBINPUT_EVENT_TOUCH_UP});
}

// Bugs caught: 0, False positives: 0
TEST_F(TouchPenUinputTest, PenTabletThroughLibinput) {
  auto g = grab(context().pen.get());
  ASSERT_TRUE(g);

  struct seen_t {
    std::vector<libinput_event_type> types;
    bool tip_down = false;
    double x = -1;
    double pressure = -1;
  };

  auto collect = [&](const platf::pen_input_t &pen) {
    seen_t seen;
    g->send(
      [&] {
        platf::pen_update(client.get(), port, pen);
      },
      [&](libinput_event *ev) {
        seen.types.push_back(libinput_event_get_type(ev));
        if (auto t = libinput_event_get_tablet_tool_event(ev)) {
          seen.x = libinput_event_tablet_tool_get_x_transformed(t, 1920);
          seen.pressure = libinput_event_tablet_tool_get_pressure(t);
          if (libinput_event_get_type(ev) == LIBINPUT_EVENT_TABLET_TOOL_TIP) {
            seen.tip_down = libinput_event_tablet_tool_get_tip_state(t) == LIBINPUT_TABLET_TOOL_TIP_DOWN;
          }
        }
      }
    );
    return seen;
  };

  // Hover with an unknown tool: a pen comes into proximity, no tip.
  auto hover = collect(stylus(LI_TOUCH_EVENT_HOVER, 0.5f));
  ASSERT_FALSE(hover.types.empty());
  EXPECT_EQ(hover.types.front(), LIBINPUT_EVENT_TABLET_TOOL_PROXIMITY);
  EXPECT_EQ(std::ranges::count(hover.types, LIBINPUT_EVENT_TABLET_TOOL_TIP), 0);
  EXPECT_NEAR(hover.x, 480.0, 1.0);

  // Contact with unknown pressure: the tip goes down.
  auto down = collect(stylus(LI_TOUCH_EVENT_DOWN, 0.0f));
  EXPECT_TRUE(down.tip_down);
  EXPECT_GT(down.pressure, 0.0);

  // A barrel button keeps the tip down where it is.
  auto button = stylus(LI_TOUCH_EVENT_BUTTON_ONLY, 0.0f);
  button.penButtons = LI_PEN_BUTTON_PRIMARY;
  button.x = button.y = 0;
  auto pressed = collect(button);
  EXPECT_EQ(std::ranges::count(pressed.types, LIBINPUT_EVENT_TABLET_TOOL_BUTTON), 1);
  EXPECT_EQ(std::ranges::count(pressed.types, LIBINPUT_EVENT_TABLET_TOOL_TIP), 0);
  button.penButtons = 0;
  collect(button);

  auto up = collect(stylus(LI_TOUCH_EVENT_UP, 0.0f));
  EXPECT_EQ(std::ranges::count(up.types, LIBINPUT_EVENT_TABLET_TOOL_TIP), 1);
  EXPECT_FALSE(up.tip_down);

  // A cancel during contact lifts the tip (libvirtualhid keeps the pen in proximity).
  collect(stylus(LI_TOUCH_EVENT_DOWN, 0.5f));
  auto cancel = collect(stylus(LI_TOUCH_EVENT_CANCEL_ALL, 0.0f));
  EXPECT_EQ(std::ranges::count(cancel.types, LIBINPUT_EVENT_TABLET_TOOL_TIP), 1);
}

#endif
