/**
 * @file src/platform/linux/input/inputtino_abs.cpp
 * @brief Definitions for mapping absolute mouse, touch and pen input onto the streamed output.
 */
// standard includes
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <mutex>

// local includes
#include "inputtino_abs.h"
#include "src/logging.h"

#ifdef SUNSHINE_BUILD_WAYLAND
  #include "src/platform/linux/hyprland/ipc.h"
#endif

using namespace std::literals;

namespace platf::abs_input {
  namespace {
    double clamp01(double value) {
      if (!std::isfinite(value)) {
        return 0.0;
      }
      return std::clamp(value, 0.0, 1.0);
    }

    bool same_port(const touch_port_t &a, const touch_port_t &b) {
      return a.offset_x == b.offset_x && a.offset_y == b.offset_y && a.width == b.width && a.height == b.height &&
             a.layout_x == b.layout_x && a.layout_y == b.layout_y && a.output_width == b.output_width &&
             a.output_height == b.output_height;
    }

    /**
     * @brief A point and the output rectangle (x, y, w, h) holding it as fractions of `layout`.
     * The output part is left at the whole box if its size is unknown.
     */
    target_t in_layout(double x, double y, double w, double h, double px, double py, const output_rect_t &layout) {
      if (layout.width <= 0 || layout.height <= 0) {
        return {{0, 0}};
      }
      auto fx = [&](double v) {
        return clamp01((v - layout.x) / layout.width);
      };
      auto fy = [&](double v) {
        return clamp01((v - layout.y) / layout.height);
      };
      target_t target {{fx(px), fy(py)}};
      if (w > 0 && h > 0) {
        target.min = {fx(x), fy(y)};
        target.max = {fx(x + w), fy(y + h)};
      }
      return target;
    }
  }  // namespace

  target_t layout_target(const touch_port_t &port, double local_x, double local_y) {
    const output_rect_t layout {{}, port.layout_x, port.layout_y, port.width, port.height};
    return in_layout(
      port.offset_x,
      port.offset_y,
      port.output_width,
      port.output_height,
      port.offset_x + local_x,
      port.offset_y + local_y,
      layout
    );
  }

  target_t layout_target(const touch_port_t &port, const output_rect_t &output, const output_rect_t &layout, double local_x, double local_y) {
    double sx = port.output_width > 0 ? double(output.width) / port.output_width : 1.0;
    double sy = port.output_height > 0 ? double(output.height) / port.output_height : 1.0;
    return in_layout(output.x, output.y, output.width, output.height, output.x + local_x * sx, output.y + local_y * sy, layout);
  }

  std::optional<target_t> output_target(const touch_port_t &port, double local_x, double local_y) {
    if (port.output_width <= 0 || port.output_height <= 0) {
      return std::nullopt;
    }
    return target_t {{clamp01(local_x / port.output_width), clamp01(local_y / port.output_height)}};
  }

  int libinput_units(double at, double min, double max, int axis_max) {
    // libinput puts value v at v / (max - min + 1) of the target box, so aim at fraction * (axis_max + 1).
    // Round up, so a point lands at or just past its target (inside the pixel a client pointed at, and never on the
    // output left of / above the streamed one). The tolerance only absorbs rounding in this arithmetic: a target
    // that is 0.0005 units past a whole unit still needs the next one.
    const double n = axis_max + 1.0;
    const double eps = 1e-9;
    // Smallest value at or past `min`, largest value before `max`: stay on the streamed output.
    double lo = std::ceil(clamp01(min) * n - eps);
    double hi = std::ceil(clamp01(max) * n - eps) - 1;
    hi = std::max(hi, lo);
    double units = std::ceil(clamp01(at) * n - eps);
    units = std::clamp(units, lo, hi);
    return int(std::clamp(units, 0.0, double(axis_max)));
  }

  int libinput_units(double fraction, int axis_max) {
    return libinput_units(fraction, 0.0, 1.0, axis_max);
  }

  float libinput_fraction(double at, double min, double max, int axis_max) {
    return float(double(libinput_units(at, min, max, axis_max)) / axis_max);
  }

  std::string compositor_device_name(std::string_view evdev_name) {
    std::string name {evdev_name};
    for (auto &c : name) {
      if (c == ' ' || c == '\n') {
        c = '-';
      } else {
        c = char(std::tolower(static_cast<unsigned char>(c)));
      }
    }
    return name;
  }

  bool is_instance_of(std::string_view name, std::string_view base) {
    if (base.empty() || !name.starts_with(base)) {
      return false;
    }
    auto rest = name.substr(base.size());
    if (rest.empty()) {
      return true;
    }
    if (rest.size() < 2 || rest.front() != '-') {
      return false;
    }
    rest.remove_prefix(1);
    return std::ranges::all_of(rest, [](char c) {
      return c >= '0' && c <= '9';
    });
  }

  output_rect_t logical_rect(std::string name, int x, int y, int mode_width, int mode_height, double scale, int transform) {
    if (!(scale > 0) || !std::isfinite(scale)) {
      scale = 1.0;
    }
    int width = int(std::lround(mode_width / scale));
    int height = int(std::lround(mode_height / scale));
    if (transform % 2) {
      std::swap(width, height);
    }
    return {std::move(name), x, y, width, height};
  }

  std::optional<output_rect_t> find_output(const std::vector<output_rect_t> &outputs, const touch_port_t &port) {
    const output_rect_t *found = nullptr;
    for (const auto &output : outputs) {
      if (output.x != port.offset_x || output.y != port.offset_y) {
        continue;
      }
      bool same_size = output.width == port.output_width && output.height == port.output_height;
      if (!found || same_size) {
        found = &output;
      }
      if (same_size) {
        break;
      }
    }
    if (!found) {
      return std::nullopt;
    }
    return *found;
  }

  std::optional<output_rect_t> find_output(const std::vector<output_rect_t> &outputs, std::string_view name) {
    for (const auto &output : outputs) {
      if (output.name == name) {
        return output;
      }
    }
    return std::nullopt;
  }

  std::optional<output_rect_t> bounding_box(const std::vector<output_rect_t> &outputs) {
    if (outputs.empty()) {
      return std::nullopt;
    }
    int x0 = outputs.front().x;
    int y0 = outputs.front().y;
    int x1 = x0 + outputs.front().width;
    int y1 = y0 + outputs.front().height;
    for (const auto &o : outputs) {
      x0 = std::min(x0, o.x);
      y0 = std::min(y0, o.y);
      x1 = std::max(x1, o.x + o.width);
      y1 = std::max(y1, o.y + o.height);
    }
    return output_rect_t {{}, x0, y0, x1 - x0, y1 - y0};
  }

  layout_mapper_t::layout_mapper_t(std::shared_ptr<compositor_t> compositor):
      compositor_ {std::move(compositor)} {
  }

  target_t layout_mapper_t::map(const touch_port_t &port, double local_x, double local_y) {
    if (!compositor_) {
      return layout_target(port, local_x, local_y);
    }

    outputs_t outputs;
    if (!port_ || !same_port(*port_, port)) {
      // A new port describes the layout as it is now: find the output it is for, then follow that output by name.
      port_ = port;
      outputs = compositor_->outputs(true);
      auto output = outputs ? find_output(*outputs, port) : std::nullopt;
      output_ = output ? output->name : std::string {};
      if (output_.empty()) {
        BOOST_LOG(debug) << "Absolute mouse: no output at "sv << port.offset_x << ',' << port.offset_y << "; using the stream's layout"sv;
      }
    } else if (!output_.empty()) {
      outputs = compositor_->outputs(false);
    }

    if (outputs && !output_.empty()) {
      auto output = find_output(*outputs, output_);
      auto layout = bounding_box(*outputs);
      if (output && layout) {
        return layout_target(port, *output, *layout, local_x, local_y);
      }
    }
    return layout_target(port, local_x, local_y);
  }

  output_mapper_t::output_mapper_t(std::string evdev_name, std::shared_ptr<compositor_t> compositor):
      evdev_name_ {std::move(evdev_name)},
      compositor_ {std::move(compositor)} {
  }

  target_t output_mapper_t::map(const touch_port_t &port, double local_x, double local_y) {
    if (compositor_) {
      bool changed = !port_ || !same_port(*port_, port) || generation_ != compositor_->generation();
      bool retry = bound_output_.empty() && std::chrono::steady_clock::now() >= retry_at_;
      if (changed || retry) {
        rebind(port);
      }
    }

    if (!bound_output_.empty()) {
      if (auto target = output_target(port, local_x, local_y)) {
        return *target;
      }
    }
    return layout_target(port, local_x, local_y);
  }

  const std::string &output_mapper_t::bound_output() const {
    return bound_output_;
  }

  void output_mapper_t::rebind(const touch_port_t &port) {
    port_ = port;

    std::optional<output_rect_t> output;
    if (port.output_width > 0 && port.output_height > 0) {
      if (auto outputs = compositor_->outputs(true)) {
        output = find_output(*outputs, port);
      }
    }

    bool bound = output && compositor_->bind_to_output(evdev_name_, output->name);
    // Read after talking to the compositor, which may have just connected and started a new generation.
    generation_ = compositor_->generation();

    if (bound) {
      if (bound_output_ != output->name) {
        BOOST_LOG(info) << "Mapping ["sv << evdev_name_ << "] to output ["sv << output->name << ']';
      }
      bound_output_ = output->name;
      return;
    }

    if (!bound_output_.empty()) {
      // Don't leave it bound to an output that no longer matches the stream.
      compositor_->bind_to_output(evdev_name_, {});
      bound_output_.clear();
    }
    BOOST_LOG(debug) << "Not binding ["sv << evdev_name_ << "] to an output; mapping it onto the whole desktop"sv;
    retry_at_ = std::chrono::steady_clock::now() + retry_interval;
  }

  flat_accel_t::flat_accel_t(std::string evdev_name, std::shared_ptr<compositor_t> compositor):
      evdev_name_ {std::move(evdev_name)},
      compositor_ {std::move(compositor)} {
  }

  void flat_accel_t::ensure() {
    if (!compositor_) {
      return;
    }
    if (applied_generation_ == compositor_->generation()) {
      return;
    }
    auto now = std::chrono::steady_clock::now();
    if (!applied_generation_ && now < retry_at_) {
      return;
    }
    if (compositor_->set_flat_accel(evdev_name_)) {
      BOOST_LOG(debug) << "Set flat acceleration on ["sv << evdev_name_ << ']';
      applied_generation_ = compositor_->generation();
    } else {
      applied_generation_.reset();
      retry_at_ = now + output_mapper_t::retry_interval;
    }
  }

#ifdef SUNSHINE_BUILD_WAYLAND
  namespace {
    class hyprland_compositor_t: public compositor_t {
    public:
      static constexpr auto reconnect_interval = 5s;
      static constexpr auto outputs_max_age = 1s;

      outputs_t outputs(bool refresh) override {
        auto now = std::chrono::steady_clock::now();
        {
          std::lock_guard lock {mutex_};
          if (!refresh && outputs_ && !outputs_dirty_.load(std::memory_order_acquire) && now - outputs_time_ < outputs_max_age) {
            return outputs_;
          }
        }

        auto ipc = connection();
        if (!ipc) {
          return nullptr;
        }
        // Clear before asking, so an event arriving during the request marks the new copy dirty again.
        outputs_dirty_.store(false, std::memory_order_release);
        auto outputs = std::make_shared<std::vector<output_rect_t>>();
        for (const auto &monitor : ipc->monitors(false)) {
          if (!monitor.disabled) {
            outputs->emplace_back(logical_rect(monitor.name, monitor.x, monitor.y, monitor.width, monitor.height, monitor.scale, monitor.transform));
          }
        }
        if (outputs->empty()) {
          drop(ipc);
          return nullptr;
        }

        std::lock_guard lock {mutex_};
        outputs_ = outputs;
        outputs_time_ = now;
        return outputs_;
      }

      bool bind_to_output(const std::string &evdev_name, const std::string &output) override {
        return set_all(evdev_name, {{"output", output}});
      }

      bool set_flat_accel(const std::string &evdev_name) override {
        return set_all(evdev_name, {{"accel_profile", "flat"}, {"sensitivity", "0"}});
      }

      std::uint64_t generation() const override {
        return generation_.load(std::memory_order_acquire);
      }

    private:
      std::shared_ptr<hyprland::ipc_t> connection() {
        std::lock_guard lock {mutex_};
        if (ipc_) {
          return ipc_;
        }
        auto now = std::chrono::steady_clock::now();
        if (now < next_attempt_) {
          return nullptr;
        }
        next_attempt_ = now + reconnect_interval;

        std::shared_ptr<hyprland::ipc_t> ipc = hyprland::ipc_t::connect();
        if (!ipc) {
          return nullptr;
        }
        ipc_ = ipc;
        generation_.fetch_add(1, std::memory_order_acq_rel);

        if (!listener_) {
          listener_ = std::make_unique<hyprland::event_listener_t>();
          listener_->subscribe([this](std::string_view event, std::string_view) {
            if (event == "configreloaded"sv) {
              // Per-device settings made with hl.device() do not survive a config reload.
              generation_.fetch_add(1, std::memory_order_acq_rel);
              outputs_dirty_.store(true, std::memory_order_release);
            } else if (event.starts_with("monitoradded"sv) || event.starts_with("monitorremoved"sv)) {
              outputs_dirty_.store(true, std::memory_order_release);
            }
          });
          listener_->start();
        }
        return ipc;
      }

      void drop(const std::shared_ptr<hyprland::ipc_t> &ipc) {
        std::lock_guard lock {mutex_};
        if (ipc_ == ipc) {
          ipc_.reset();
        }
      }

      /**
       * @brief Set options on every compositor device created from this evdev name.
       * @details Covers the names Hyprland is known to give it (`name`, and `name-1` for the touch half of a touch
       * screen) even before the device shows up, since options set early are applied when it appears, plus any
       * other `name-N` it is listed under.
       */
      bool set_all(const std::string &evdev_name, const std::vector<std::pair<std::string, std::string>> &options) {
        auto ipc = connection();
        if (!ipc) {
          return false;
        }
        auto devices = ipc->request_json("j/devices");
        if (!devices || !devices->is_object()) {
          drop(ipc);
          return false;
        }

        auto base = compositor_device_name(evdev_name);
        std::vector<std::string> names {base, base + "-1"};
        for (const auto &[kind, list] : devices->items()) {
          if (!list.is_array()) {
            continue;
          }
          for (const auto &device : list) {
            if (!device.is_object() || !device.contains("name") || !device["name"].is_string()) {
              continue;
            }
            auto name = device["name"].get<std::string>();
            if (is_instance_of(name, base) && std::ranges::find(names, name) == names.end()) {
              names.emplace_back(std::move(name));
            }
          }
        }

        bool ok = true;
        for (const auto &name : names) {
          for (const auto &[key, value] : options) {
            ok = ipc->set_device_option(name, key, value) && ok;
          }
        }
        return ok;
      }

      std::mutex mutex_;
      std::shared_ptr<hyprland::ipc_t> ipc_;
      std::chrono::steady_clock::time_point next_attempt_ {};
      std::unique_ptr<hyprland::event_listener_t> listener_;
      std::atomic<std::uint64_t> generation_ {0};
      std::atomic<bool> outputs_dirty_ {false};
      outputs_t outputs_;
      std::chrono::steady_clock::time_point outputs_time_ {};
    };
  }  // namespace

  std::shared_ptr<compositor_t> hyprland_compositor() {
    // Never destroyed: its event listener thread may still be running at exit.
    static auto *instance = new std::shared_ptr<compositor_t> {std::make_shared<hyprland_compositor_t>()};
    return *instance;
  }
#else
  std::shared_ptr<compositor_t> hyprland_compositor() {
    return nullptr;
  }
#endif
}  // namespace platf::abs_input
