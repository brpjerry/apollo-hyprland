/**
 * @file tests/integration/test_touch_pen_uinput.cpp
 * @brief The virtual touch screen and pen tablet (src/platform/linux/input/touch_pen.*) as libinput sees them.
 * @details Creates real uinput devices, so it only runs with APOLLO_TEST_UINPUT=1, write access to /dev/uinput and
 *          read access to /dev/input/event* (the input group). Each device is grabbed (EVIOCGRAB) before any event
 *          is written, so its events reach only this test's libinput context, never the compositor or the desktop.
 */
#include "../tests_common.h"

#if defined(__linux__) && defined(TESTS_HAVE_LIBINPUT)

  // standard includes
  #include <cerrno>
  #include <chrono>
  #include <cmath>
  #include <cstdlib>
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
  #include <src/platform/linux/input/touch_pen.h>

using namespace platf::touch_pen;
using namespace std::literals;

namespace {
  /**
   * @brief A grabbed uinput device read through a private libinput path context.
   */
  struct grabbed_t {
    std::unique_ptr<uinput_device_t> device;
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

    explicit grabbed_t(std::unique_ptr<uinput_device_t> dev, std::string &why):
        device {std::move(dev)} {
      auto node = device->devnode();
      // The node appears, and gets its group permissions from udev, shortly after creation.
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

    // Writes the events and returns what libinput makes of them.
    template<class F>
    void send(const events_t &events, F &&each) {
      device->write(events);
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

    std::vector<libinput_event_type> types(const events_t &events) {
      std::vector<libinput_event_type> out;
      send(events, [&](libinput_event *ev) {
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
      if (!uinput_available()) {
        GTEST_SKIP() << "no write access to /dev/uinput";
      }
    }

    std::unique_ptr<grabbed_t> grab(std::unique_ptr<uinput_device_t> device, const std::string &error) {
      if (!device) {
        ADD_FAILURE() << "creating the device failed: " << error;
        return nullptr;
      }
      std::string why;
      auto g = std::make_unique<grabbed_t>(std::move(device), why);
      if (!g->ok()) {
        ADD_FAILURE() << why;
        return nullptr;
      }
      return g;
    }
  };

  contact_t middle() {
    contact_t c;
    c.x = touch_axes::max_x / 2;
    c.y = touch_axes::max_y / 2;
    c.pressure = 100;
    c.major = 300;
    c.minor = 200;
    return c;
  }
}  // namespace

// Bugs caught: 0, False positives: 0
TEST_F(TouchPenUinputTest, TouchScreenThroughLibinput) {
  std::string error;
  auto g = grab(uinput_device_t::touch_screen("Apollo test touch (grabbed)", error), error);
  ASSERT_TRUE(g);
  touch_state_t touch;

  // Hover: nothing at all.
  EXPECT_TRUE(g->types(touch.handle(LI_TOUCH_EVENT_HOVER, 1, middle())).empty());

  double x = -1;
  g->send(touch.handle(LI_TOUCH_EVENT_DOWN, 1, middle()), [&](libinput_event *ev) {
    if (libinput_event_get_type(ev) == LIBINPUT_EVENT_TOUCH_DOWN) {
      x = libinput_event_touch_get_x_transformed(libinput_event_get_touch_event(ev), 1920);
    }
  });
  EXPECT_NEAR(x, 960.0, 1.0);
  EXPECT_EQ(g->types(touch.handle(LI_TOUCH_EVENT_MOVE, 1, middle())), std::vector<libinput_event_type> {});  // Same position
  auto moved = middle();
  moved.x += 100;
  EXPECT_EQ(g->types(touch.handle(LI_TOUCH_EVENT_MOVE, 1, moved)), std::vector {LIBINPUT_EVENT_TOUCH_MOTION});
  EXPECT_EQ(g->types(touch.handle(LI_TOUCH_EVENT_UP, 1, {})), std::vector {LIBINPUT_EVENT_TOUCH_UP});

  // Two fingers, then cancel-all: both are cancelled, not lifted (no tap).
  EXPECT_EQ(g->types(touch.handle(LI_TOUCH_EVENT_DOWN, 2, middle())), std::vector {LIBINPUT_EVENT_TOUCH_DOWN});
  EXPECT_EQ(g->types(touch.handle(LI_TOUCH_EVENT_DOWN, 3, moved)), std::vector {LIBINPUT_EVENT_TOUCH_DOWN});
  auto cancelled = g->types(touch.handle(LI_TOUCH_EVENT_CANCEL_ALL, 0, {}));
  EXPECT_EQ(cancelled, (std::vector {LIBINPUT_EVENT_TOUCH_CANCEL, LIBINPUT_EVENT_TOUCH_CANCEL}));

  // The slots work again afterwards.
  EXPECT_EQ(g->types(touch.handle(LI_TOUCH_EVENT_DOWN, 4, middle())), std::vector {LIBINPUT_EVENT_TOUCH_DOWN});
  EXPECT_EQ(g->types(touch.handle(LI_TOUCH_EVENT_UP, 4, {})), std::vector {LIBINPUT_EVENT_TOUCH_UP});
}

// Bugs caught: 0, False positives: 0
TEST_F(TouchPenUinputTest, PenTabletThroughLibinput) {
  std::string error;
  auto g = grab(uinput_device_t::pen_tablet("Apollo test pen (grabbed)", error), error);
  ASSERT_TRUE(g);
  pen_state_t pen;

  pen_sample_t s;
  s.tool = BTN_TOOL_PEN;
  s.x = pen_axes::max_x / 4;
  s.y = pen_axes::max_y / 4;
  s.distance = 0.5f;
  s.rotation = 30;
  std::tie(s.tilt_x, s.tilt_y) = pen_tilt(45, 0);

  struct seen_t {
    std::vector<libinput_event_type> types;
    bool tip_down = false;
    bool has_rotation = false;
    double rotation = -1;
    double tilt_y = 0;
    double pressure = -1;
  };
  auto collect = [&](const events_t &events) {
    seen_t seen;
    g->send(events, [&](libinput_event *ev) {
      seen.types.push_back(libinput_event_get_type(ev));
      if (auto t = libinput_event_get_tablet_tool_event(ev)) {
        auto tool = libinput_event_tablet_tool_get_tool(t);
        seen.has_rotation = libinput_tablet_tool_has_rotation(tool);
        seen.rotation = libinput_event_tablet_tool_get_rotation(t);
        seen.tilt_y = libinput_event_tablet_tool_get_tilt_y(t);
        seen.pressure = libinput_event_tablet_tool_get_pressure(t);
        if (libinput_event_get_type(ev) == LIBINPUT_EVENT_TABLET_TOOL_TIP) {
          seen.tip_down = libinput_event_tablet_tool_get_tip_state(t) == LIBINPUT_TABLET_TOOL_TIP_DOWN;
        }
      }
    });
    return seen;
  };

  // Hover: in proximity, no tip; rotation and tilt arrive.
  auto hover = collect(pen.handle(LI_TOUCH_EVENT_HOVER, s));
  ASSERT_FALSE(hover.types.empty());
  EXPECT_EQ(hover.types.front(), LIBINPUT_EVENT_TABLET_TOOL_PROXIMITY);
  EXPECT_EQ(std::ranges::count(hover.types, LIBINPUT_EVENT_TABLET_TOOL_TIP), 0);
  EXPECT_TRUE(hover.has_rotation);
  EXPECT_NEAR(hover.rotation, 30.0, 0.5);
  EXPECT_NEAR(hover.tilt_y, 45.0, 1.0);

  // Contact with unknown pressure: the tip goes down.
  auto down = collect(pen.handle(LI_TOUCH_EVENT_DOWN, s));
  EXPECT_TRUE(down.tip_down);
  EXPECT_GT(down.pressure, 0.0);

  auto up = collect(pen.handle(LI_TOUCH_EVENT_UP, s));
  EXPECT_EQ(std::ranges::count(up.types, LIBINPUT_EVENT_TABLET_TOOL_TIP), 1);
  EXPECT_FALSE(up.tip_down);

  // Leaving: proximity out.
  auto leave = collect(pen.handle(LI_TOUCH_EVENT_HOVER_LEAVE, s));
  EXPECT_EQ(std::ranges::count(leave.types, LIBINPUT_EVENT_TABLET_TOOL_PROXIMITY), 1);

  // A cancel during contact lifts the tip and leaves proximity.
  collect(pen.handle(LI_TOUCH_EVENT_DOWN, s));
  auto cancel = collect(pen.handle(LI_TOUCH_EVENT_CANCEL_ALL, {}));
  EXPECT_EQ(std::ranges::count(cancel.types, LIBINPUT_EVENT_TABLET_TOOL_TIP), 1);
  EXPECT_EQ(std::ranges::count(cancel.types, LIBINPUT_EVENT_TABLET_TOOL_PROXIMITY), 1);
}

#endif
