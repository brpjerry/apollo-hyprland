/**
 * @file src/platform/linux/input/touch_pen.cpp
 * @brief Definitions for the per-client virtual touch screen and pen tablet.
 */
// local includes
#include "touch_pen.h"

// standard includes
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>

// platform includes
#include <unistd.h>

// lib includes
#include <libevdev/libevdev-uinput.h>
#include <libevdev/libevdev.h>
#include <moonlight-common-c/src/Limelight.h>

namespace platf::touch_pen {
  namespace {
    constexpr double pi = 3.14159265358979323846;

    void add(events_t &events, std::uint16_t type, std::uint16_t code, std::int32_t value) {
      events.push_back({type, code, value});
    }

    void syn(events_t &events) {
      add(events, EV_SYN, SYN_REPORT, 0);
    }
  }  // namespace

  int touch_orientation(std::uint16_t rotation) {
    if (rotation == LI_ROT_UNKNOWN) {
      return 0;
    }
    // An ellipse looks the same turned by 180 degrees: fold into [0, 180), then into (-90, 90].
    int a = rotation % 180;
    return a > 90 ? a - 180 : a;
  }

  int touch_pressure(float pressure) {
    if (!(pressure > 0.0f)) {
      return touch_axes::pressure_max / 2;
    }
    return std::clamp((int) std::lround(std::min(pressure, 1.0f) * touch_axes::pressure_max), 1, touch_axes::pressure_max);
  }

  std::pair<int, int> contact_area(float major, float minor, std::uint16_t rotation, double units_x, double units_y) {
    if (!(major > 0.0f) || !(minor > 0.0f)) {
      return {0, 0};
    }
    double theta = (rotation == LI_ROT_UNKNOWN ? 45.0 : double(rotation % 360)) * pi / 180.0;
    // The major axis points theta clockwise from vertical; the minor axis is perpendicular to it.
    double major_units = major * std::hypot(std::sin(theta) * units_x, std::cos(theta) * units_y);
    double minor_units = minor * std::hypot(std::cos(theta) * units_x, std::sin(theta) * units_y);
    auto clamp = [](double v) {
      return std::clamp((int) std::lround(v), 1, touch_axes::major_max);
    };
    int a = clamp(major_units);
    int b = clamp(minor_units);
    return {std::max(a, b), std::min(a, b)};
  }

  int touch_state_t::contacts() const {
    return (int) std::count_if(slots_.begin(), slots_.end(), [](const slot_t &s) {
      return s.active;
    });
  }

  int touch_state_t::find(std::uint32_t pointer_id) const {
    for (int i = 0; i < touch_axes::slots; ++i) {
      if (slots_[i].active && slots_[i].pointer_id == pointer_id) {
        return i;
      }
    }
    return -1;
  }

  void touch_state_t::select(events_t &events, int slot) {
    if (current_slot_ != slot) {
      add(events, EV_ABS, ABS_MT_SLOT, slot);
      current_slot_ = slot;
    }
  }

  events_t touch_state_t::place(std::uint32_t pointer_id, const contact_t &c) {
    events_t events;
    int slot = find(pointer_id);
    bool fresh = slot < 0;
    if (fresh) {
      for (int i = 0; i < touch_axes::slots; ++i) {
        if (!slots_[i].active) {
          slot = i;
          break;
        }
      }
      if (slot < 0) {
        return events;  // More contacts than slots: drop this one
      }
    }
    bool first = contacts() == 0;

    select(events, slot);
    auto &s = slots_[slot];
    if (fresh) {
      if (s.palm) {
        // libinput reads the tool type when the tracking ID starts; a palm left in this slot would hide the touch.
        add(events, EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_FINGER);
        s.palm = false;
      }
      add(events, EV_ABS, ABS_MT_TRACKING_ID, next_tracking_id_);
      next_tracking_id_ = (next_tracking_id_ + 1) & 0xFFFF;
      s.active = true;
      s.pointer_id = pointer_id;
    }
    add(events, EV_ABS, ABS_MT_POSITION_X, std::clamp(c.x, 0, touch_axes::max_x));
    add(events, EV_ABS, ABS_MT_POSITION_Y, std::clamp(c.y, 0, touch_axes::max_y));
    add(events, EV_ABS, ABS_MT_PRESSURE, std::clamp(c.pressure, 0, touch_axes::pressure_max));
    add(events, EV_ABS, ABS_MT_TOUCH_MAJOR, std::clamp(c.major, 0, touch_axes::major_max));
    add(events, EV_ABS, ABS_MT_TOUCH_MINOR, std::clamp(c.minor, 0, touch_axes::major_max));
    add(events, EV_ABS, ABS_MT_ORIENTATION, std::clamp(c.orientation, -90, 90));
    // Single-touch emulation for clients of the legacy axes.
    add(events, EV_ABS, ABS_X, std::clamp(c.x, 0, touch_axes::max_x));
    add(events, EV_ABS, ABS_Y, std::clamp(c.y, 0, touch_axes::max_y));
    add(events, EV_ABS, ABS_PRESSURE, std::clamp(c.pressure, 0, touch_axes::pressure_max));
    if (first) {
      add(events, EV_KEY, BTN_TOUCH, 1);
    }
    syn(events);
    return events;
  }

  void touch_state_t::lift(events_t &events, int slot, bool cancel) {
    select(events, slot);
    auto &s = slots_[slot];
    if (cancel) {
      // A contact that turns into a palm is cancelled by libinput (LIBINPUT_EVENT_TOUCH_CANCEL, wl_touch.cancel).
      add(events, EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_PALM);
      s.palm = true;
    }
    add(events, EV_ABS, ABS_MT_TRACKING_ID, -1);
    s.active = false;
  }

  events_t touch_state_t::handle(std::uint8_t event_type, std::uint32_t pointer_id, const contact_t &contact) {
    events_t events;
    switch (event_type) {
      case LI_TOUCH_EVENT_DOWN:
      case LI_TOUCH_EVENT_MOVE:
        return place(pointer_id, contact);
      case LI_TOUCH_EVENT_HOVER:
      case LI_TOUCH_EVENT_UP:
      case LI_TOUCH_EVENT_HOVER_LEAVE:
      case LI_TOUCH_EVENT_CANCEL:
        {
          // Touch screens have no hover: a hover reports nothing, unless the contact was down, which means it lifted.
          int slot = find(pointer_id);
          if (slot < 0) {
            return events;
          }
          lift(events, slot, event_type == LI_TOUCH_EVENT_CANCEL);
          break;
        }
      case LI_TOUCH_EVENT_CANCEL_ALL:
        for (int i = 0; i < touch_axes::slots; ++i) {
          if (slots_[i].active) {
            lift(events, i, true);
          }
        }
        if (events.empty()) {
          return events;
        }
        break;
      default:
        return events;
    }
    if (contacts() == 0) {
      add(events, EV_KEY, BTN_TOUCH, 0);
      add(events, EV_ABS, ABS_PRESSURE, 0);
    }
    syn(events);
    return events;
  }

  std::pair<int, int> pen_tilt(std::uint8_t tilt, std::uint16_t rotation) {
    if (tilt == LI_TILT_UNKNOWN || rotation == LI_ROT_UNKNOWN) {
      return {0, 0};
    }
    // Rotation: degrees from vertical in Y (parallel to the screen); tilt: degrees from vertical in Z.
    double rotation_rads = double(rotation % 360) * pi / 180.0;
    double tilt_rads = double(std::min<int>(tilt, 90)) * pi / 180.0;
    double r = std::sin(tilt_rads);
    double z = std::cos(tilt_rads);
    double tilt_x = std::atan2(std::sin(-rotation_rads) * r, z) * 180.0 / pi;
    double tilt_y = std::atan2(std::cos(-rotation_rads) * r, z) * 180.0 / pi;
    return {std::clamp((int) std::lround(tilt_x), -90, 90), std::clamp((int) std::lround(tilt_y), -90, 90)};
  }

  int pen_state_t::tool() const {
    return tool_;
  }

  bool pen_state_t::touching() const {
    return touching_;
  }

  void pen_state_t::proximity_out(events_t &events) {
    if (!tool_) {
      return;
    }
    if (touching_) {
      add(events, EV_KEY, BTN_TOUCH, 0);
      touching_ = false;
    }
    add(events, EV_ABS, ABS_PRESSURE, 0);
    buttons(events, 0);
    add(events, EV_KEY, tool_, 0);
    syn(events);
    tool_ = 0;
  }

  void pen_state_t::proximity_in(events_t &events, int tool) {
    if (!tool) {
      tool = tool_ ? tool_ : last_tool_;
    }
    if (tool_ == tool) {
      return;
    }
    // libinput needs the old tool out of proximity, in its own frame, before the new one comes in.
    proximity_out(events);
    add(events, EV_KEY, tool, 1);
    tool_ = tool;
    last_tool_ = tool;
  }

  void pen_state_t::axes(events_t &events, const pen_sample_t &s) {
    add(events, EV_ABS, ABS_X, std::clamp(s.x, 0, pen_axes::max_x));
    add(events, EV_ABS, ABS_Y, std::clamp(s.y, 0, pen_axes::max_y));
    add(events, EV_ABS, ABS_TILT_X, std::clamp(s.tilt_x, -90, 90));
    add(events, EV_ABS, ABS_TILT_Y, std::clamp(s.tilt_y, -90, 90));
    if (s.rotation >= 0) {
      // libinput reports ABS_Z as (value / 360 * 360 + 90) mod 360 degrees.
      add(events, EV_ABS, ABS_Z, (s.rotation % 360 + 270) % 360);
    }
  }

  void pen_state_t::buttons(events_t &events, std::uint8_t b) {
    static constexpr std::pair<std::uint8_t, int> map[] {
      {LI_PEN_BUTTON_PRIMARY, BTN_STYLUS},
      {LI_PEN_BUTTON_SECONDARY, BTN_STYLUS2},
      {LI_PEN_BUTTON_TERTIARY, BTN_STYLUS3},
    };
    for (auto [bit, code] : map) {
      if ((b & bit) != (buttons_ & bit)) {
        add(events, EV_KEY, code, (b & bit) ? 1 : 0);
      }
    }
    buttons_ = b;
  }

  events_t pen_state_t::handle(std::uint8_t event_type, const pen_sample_t &s) {
    events_t events;
    switch (event_type) {
      case LI_TOUCH_EVENT_HOVER:
        proximity_in(events, s.tool);
        axes(events, s);
        add(events, EV_ABS, ABS_DISTANCE, std::clamp((int) std::lround(s.distance * pen_axes::distance_max), 0, pen_axes::distance_max));
        add(events, EV_ABS, ABS_PRESSURE, 0);
        if (touching_) {
          add(events, EV_KEY, BTN_TOUCH, 0);
          touching_ = false;
        }
        buttons(events, s.buttons);
        break;
      case LI_TOUCH_EVENT_DOWN:
      case LI_TOUCH_EVENT_MOVE:
        {
          proximity_in(events, s.tool);
          axes(events, s);
          add(events, EV_ABS, ABS_DISTANCE, 0);
          // libinput decides contact by pressure alone; unknown pressure (0) must still be a contact.
          int pressure = s.pressure > 0.0f ? (int) std::lround(std::min(s.pressure, 1.0f) * pen_axes::pressure_max) : pen_axes::pressure_max / 2;
          add(events, EV_ABS, ABS_PRESSURE, std::clamp(pressure, 1, pen_axes::pressure_max));
          if (!touching_) {
            add(events, EV_KEY, BTN_TOUCH, 1);
            touching_ = true;
          }
          buttons(events, s.buttons);
          break;
        }
      case LI_TOUCH_EVENT_UP:
        if (!tool_) {
          return events;
        }
        // The tip lifts; the pen stays in proximity until the client reports it left.
        axes(events, s);
        add(events, EV_ABS, ABS_PRESSURE, 0);
        if (touching_) {
          add(events, EV_KEY, BTN_TOUCH, 0);
          touching_ = false;
        }
        buttons(events, s.buttons);
        break;
      case LI_TOUCH_EVENT_BUTTON_ONLY:
        if (!tool_) {
          // libinput ignores buttons out of proximity; remember them for when the pen comes in.
          buttons_ = s.buttons;
          return events;
        }
        buttons(events, s.buttons);
        if (events.empty()) {
          return events;
        }
        break;
      case LI_TOUCH_EVENT_HOVER_LEAVE:
      case LI_TOUCH_EVENT_CANCEL:
      case LI_TOUCH_EVENT_CANCEL_ALL:
        proximity_out(events);
        return events;
      default:
        return events;
    }
    syn(events);
    return events;
  }

  uinput_device_t::uinput_device_t(libevdev_uinput *device):
      device_ {device} {
  }

  uinput_device_t::~uinput_device_t() {
    libevdev_uinput_destroy(device_);
  }

  void uinput_device_t::write(const events_t &events) {
    for (auto &e : events) {
      libevdev_uinput_write_event(device_, e.type, e.code, e.value);
    }
  }

  std::string uinput_device_t::devnode() const {
    const char *node = libevdev_uinput_get_devnode(device_);
    return node ? node : "";
  }

  namespace {
    libevdev_uinput *create(libevdev *dev, std::string &error) {
      libevdev_uinput *out = nullptr;
      int err = libevdev_uinput_create_from_device(dev, LIBEVDEV_UINPUT_OPEN_MANAGED, &out);
      libevdev_free(dev);
      if (err != 0) {
        error = std::strerror(-err);
        return nullptr;
      }
      return out;
    }

    libevdev *new_device(const std::string &name) {
      libevdev *dev = libevdev_new();
      libevdev_set_name(dev, name.c_str());
      libevdev_set_id_vendor(dev, 0xBEEF);
      libevdev_set_id_product(dev, 0xDEAD);
      libevdev_set_id_version(dev, 0x111);
      libevdev_set_id_bustype(dev, BUS_USB);
      return dev;
    }
  }  // namespace

  std::unique_ptr<uinput_device_t> uinput_device_t::touch_screen(const std::string &name, std::string &error) {
    libevdev *dev = new_device(name);
    libevdev_enable_event_type(dev, EV_KEY);
    libevdev_enable_event_code(dev, EV_KEY, BTN_LEFT, nullptr);
    libevdev_enable_event_code(dev, EV_KEY, BTN_TOUCH, nullptr);
    libevdev_enable_event_type(dev, EV_ABS);
    input_absinfo slot {0, 0, touch_axes::slots - 1, 0, 0, 0};
    libevdev_enable_event_code(dev, EV_ABS, ABS_MT_SLOT, &slot);
    input_absinfo x {0, 0, touch_axes::max_x, 0, 0, 0};
    libevdev_enable_event_code(dev, EV_ABS, ABS_X, &x);
    libevdev_enable_event_code(dev, EV_ABS, ABS_MT_POSITION_X, &x);
    input_absinfo y {0, 0, touch_axes::max_y, 0, 0, 0};
    libevdev_enable_event_code(dev, EV_ABS, ABS_Y, &y);
    libevdev_enable_event_code(dev, EV_ABS, ABS_MT_POSITION_Y, &y);
    input_absinfo tracking {0, 0, 65535, 0, 0, 0};
    libevdev_enable_event_code(dev, EV_ABS, ABS_MT_TRACKING_ID, &tracking);
    input_absinfo pressure {0, 0, touch_axes::pressure_max, 0, 0, 0};
    libevdev_enable_event_code(dev, EV_ABS, ABS_PRESSURE, &pressure);
    libevdev_enable_event_code(dev, EV_ABS, ABS_MT_PRESSURE, &pressure);
    input_absinfo major {0, 0, touch_axes::major_max, 0, 0, 0};
    libevdev_enable_event_code(dev, EV_ABS, ABS_MT_TOUCH_MAJOR, &major);
    libevdev_enable_event_code(dev, EV_ABS, ABS_MT_TOUCH_MINOR, &major);
    input_absinfo orientation {0, -90, 90, 0, 0, 0};
    libevdev_enable_event_code(dev, EV_ABS, ABS_MT_ORIENTATION, &orientation);
    input_absinfo tool {0, 0, MT_TOOL_MAX, 0, 0, 0};
    libevdev_enable_event_code(dev, EV_ABS, ABS_MT_TOOL_TYPE, &tool);
    // https://docs.kernel.org/input/event-codes.html#touchscreens
    libevdev_enable_property(dev, INPUT_PROP_DIRECT);

    auto out = create(dev, error);
    return out ? std::unique_ptr<uinput_device_t>(new uinput_device_t(out)) : nullptr;
  }

  std::unique_ptr<uinput_device_t> uinput_device_t::pen_tablet(const std::string &name, std::string &error) {
    libevdev *dev = new_device(name);
    libevdev_enable_event_type(dev, EV_KEY);
    for (int code : {BTN_TOUCH, BTN_STYLUS, BTN_STYLUS2, BTN_STYLUS3, BTN_TOOL_PEN, BTN_TOOL_RUBBER, BTN_TOOL_BRUSH, BTN_TOOL_PENCIL, BTN_TOOL_AIRBRUSH}) {
      libevdev_enable_event_code(dev, EV_KEY, code, nullptr);
    }
    libevdev_enable_event_type(dev, EV_ABS);
    input_absinfo x {0, 0, pen_axes::max_x, 1, 0, pen_axes::resolution};
    libevdev_enable_event_code(dev, EV_ABS, ABS_X, &x);
    input_absinfo y {0, 0, pen_axes::max_y, 1, 0, pen_axes::resolution};
    libevdev_enable_event_code(dev, EV_ABS, ABS_Y, &y);
    input_absinfo pressure {0, 0, pen_axes::pressure_max, 0, 0, 0};
    libevdev_enable_event_code(dev, EV_ABS, ABS_PRESSURE, &pressure);
    input_absinfo distance {0, 0, pen_axes::distance_max, 0, 0, 0};
    libevdev_enable_event_code(dev, EV_ABS, ABS_DISTANCE, &distance);
    input_absinfo tilt {0, -90, 90, 0, 0, pen_axes::tilt_resolution};
    libevdev_enable_event_code(dev, EV_ABS, ABS_TILT_X, &tilt);
    libevdev_enable_event_code(dev, EV_ABS, ABS_TILT_Y, &tilt);
    input_absinfo rotation {0, 0, pen_axes::rotation_max, 0, 0, 0};
    libevdev_enable_event_code(dev, EV_ABS, ABS_Z, &rotation);
    // https://docs.kernel.org/input/event-codes.html#tablets
    libevdev_enable_property(dev, INPUT_PROP_POINTER);
    libevdev_enable_property(dev, INPUT_PROP_DIRECT);

    auto out = create(dev, error);
    return out ? std::unique_ptr<uinput_device_t>(new uinput_device_t(out)) : nullptr;
  }

  bool uinput_available() {
    return access("/dev/uinput", W_OK) == 0;
  }
}  // namespace platf::touch_pen
