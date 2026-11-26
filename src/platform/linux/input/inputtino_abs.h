/**
 * @file src/platform/linux/input/inputtino_abs.h
 * @brief Map absolute mouse, touch and pen input onto the streamed output.
 *
 * How Hyprland 0.57.2 maps the inputtino uinput devices (measured with `hyprctl cursorpos`, a `wev` window on a
 * headless output for touch, and read in the v0.57.2 sources: src/pointer/PointerManager.cpp warpAbsolute(),
 * src/managers/input/Touch.cpp onTouchDown(), src/managers/input/InputManager.cpp set*Configs()):
 *
 * - Absolute mouse ("Mouse passthrough (absolute)", Hyprland name `mouse-passthrough-(absolute)`): mapped onto the
 *   bounding box of all monitors' logical rectangles, including a negative origin (0,-560 on the development
 *   machine). `hl.device({ name = ..., output = ... })` is accepted but has no effect: Hyprland never binds libinput
 *   pointers to an output. So it has to be normalised against the layout box (option A).
 * - Touch screen ("Touch passthrough"): libinput reports it twice, as a pointer that gets the plain name and as a
 *   touch device that gets the name with `-1` appended (Hyprland de-duplicates names across all devices). Unbound,
 *   a touch lands on the *focused* monitor (its position + fraction * logical size), never on the layout box, so
 *   option A cannot work for it. `hl.device({ name = "<touch name>", output = "NAME" })` binds it to that output and
 *   takes effect at once (option B).
 * - Pen tablet ("Pen passthrough", a tablet): unbound it is mapped onto the layout box like the mouse (unless the
 *   user set `input:tablet:output` or a region for all tablets); `output` binds it to that output's logical box.
 * - Options set with `hl.device()` before the device exists are applied when it appears. They are not kept across a
 *   config reload, hence compositor_t::generation().
 * - Scale and rotation: everything is in logical pixels. A bound device is mapped onto the output's logical box,
 *   which is already rotated (HDMI-A-1, 3840x2160 at scale 1.5 with transform 1, is 1440x2560 at 2560,-560).
 *   Hyprland's screencopy delivers rotated outputs as upright frames (2160x3840), so input::client_to_touchport()
 *   yields coordinates in that same rotated logical space and no extra rotation is needed.
 * - libinput normalises an axis as value / (max - min + 1), so the raw value max/2 lands just short of the middle,
 *   and the top-left edge of an output can land on its neighbour. libinput_units() compensates.
 * - Relative motion goes through the user's `input:sensitivity` and accel profile (with `sensitivity = -0.5`, 10 units
 *   moved the pointer 5 px). `accel_profile = "flat"` with `sensitivity = 0` on the device makes it 1:1.
 *
 * So the absolute mouse is normalised against the layout box, read live from Hyprland when it runs (the touch port's
 * copy goes stale when another output, e.g. another client's virtual display, is added), and touch and pen are bound
 * to the streamed output. Without Hyprland (X11, other compositors) all three use the touch port's layout box.
 *
 * Precision: one device unit is layout width / 19201 for the mouse (0.31 px for DP-3 + HDMI-A-1 + one 1920 px
 * virtual display) and output width / 19201 for touch and pen (0.1 px for 1920); points land less than a unit past
 * the target, on the target output.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// local includes
#include "src/platform/common.h"

namespace platf::abs_input {
  /**
   * Axis maxima of the inputtino uinput devices (third-party/inputtino/src/uinput/{mouse,touchscreen,pentablet}.cpp).
   */
  constexpr int mouse_max_x = 19200;
  constexpr int mouse_max_y = 12000;
  constexpr int touch_max_x = 19200;
  constexpr int touch_max_y = 10800;
  constexpr int pen_max_x = 19200;
  constexpr int pen_max_y = 10800;

  struct point_t {
    double x;
    double y;
  };

  /**
   * @brief A point as fractions of a box, with the part of the box the point must stay in (the streamed output).
   */
  struct target_t {
    point_t at;
    point_t min {0, 0};
    point_t max {1, 1};
  };

  /**
   * @brief A rectangle in logical layout coordinates; for outputs, `name` is the connector name.
   */
  struct output_rect_t {
    std::string name;
    int x;
    int y;
    int width;
    int height;
  };

  /**
   * @brief Fraction of the layout bounding box for a display-local point: (offset + local - layout origin) / size.
   * @param port offset_x/offset_y: top-left of the streamed output; width/height: size of the layout box;
   *             layout_x/layout_y: its top-left; output_width/output_height: size of the streamed output (optional,
   *             limits the target to the output).
   * @param local_x Display-local x, as returned by input::client_to_touchport().
   * @param local_y Display-local y.
   */
  target_t layout_target(const touch_port_t &port, double local_x, double local_y);

  /**
   * @brief layout_target() against a live layout: `output` is where the streamed output is now and `layout` the
   * bounding box of all outputs now. Local coordinates are rescaled if the output's size changed since `port`.
   */
  target_t layout_target(const touch_port_t &port, const output_rect_t &output, const output_rect_t &layout, double local_x, double local_y);

  /**
   * @brief Fraction of the streamed output for a display-local point: local / output size.
   * @return nullopt if the port does not carry the output size.
   */
  std::optional<target_t> output_target(const touch_port_t &port, double local_x, double local_y);

  /**
   * @brief Device value for one axis of a target: corrects for libinput dividing by (max + 1), rounds up to a whole
   * unit and keeps the result inside [min, max) of the target, i.e. on the streamed output.
   * @details libinput then puts it less than one unit past `at`, and never before it.
   */
  int libinput_units(double at, double min, double max, int axis_max);

  /**
   * @brief libinput_units() for the whole box.
   */
  int libinput_units(double fraction, int axis_max);

  /**
   * @brief libinput_units() as a fraction of axis_max, for inputtino APIs that take [0, 1].
   */
  float libinput_fraction(double at, double min, double max, int axis_max);

  /**
   * @brief The name Hyprland gives a device: lower case, spaces and newlines replaced by `-`.
   */
  std::string compositor_device_name(std::string_view evdev_name);

  /**
   * @brief Whether a compositor device name belongs to a device created as `base`, i.e. is `base` or `base-N`
   * (Hyprland appends `-N` when the name is taken, e.g. by the pointer half of a touch screen).
   */
  bool is_instance_of(std::string_view name, std::string_view base);

  /**
   * @brief The logical rectangle of an output from its mode: mode size divided by scale, swapped for 90 and 270 degree
   * transforms (odd wl_output_transform values), at its layout position.
   */
  output_rect_t logical_rect(std::string name, int x, int y, int mode_width, int mode_height, double scale, int transform);

  /**
   * @brief Find the output a touch port describes: the one whose logical top-left is the port's offset.
   * If several share that position (mirrors), prefer the one with the port's output size.
   */
  std::optional<output_rect_t> find_output(const std::vector<output_rect_t> &outputs, const touch_port_t &port);

  /**
   * @brief Find an output by name.
   */
  std::optional<output_rect_t> find_output(const std::vector<output_rect_t> &outputs, std::string_view name);

  /**
   * @brief Bounding box of all outputs (named ""), or nullopt if there are none.
   */
  std::optional<output_rect_t> bounding_box(const std::vector<output_rect_t> &outputs);

  using outputs_t = std::shared_ptr<const std::vector<output_rect_t>>;

  /**
   * @brief The compositor's view of outputs and per-device settings. Implemented for Hyprland; tests use a fake.
   */
  class compositor_t {
  public:
    virtual ~compositor_t() = default;

    /**
     * @brief The enabled outputs in logical layout coordinates.
     * @param refresh Ask the compositor now instead of using a recent copy.
     * @return nullptr if the compositor cannot be reached.
     */
    virtual outputs_t outputs(bool refresh) = 0;

    /**
     * @brief Map the device created with this evdev name (all its compositor devices) onto `output`.
     * An empty output removes the binding.
     */
    virtual bool bind_to_output(const std::string &evdev_name, const std::string &output) = 0;

    /**
     * @brief Make relative motion of the device 1:1 (flat acceleration, no sensitivity change).
     */
    virtual bool set_flat_accel(const std::string &evdev_name) = 0;

    /**
     * @brief Changes whenever per-device settings made earlier may have been lost (reconnect, config reload).
     */
    virtual std::uint64_t generation() const = 0;
  };

  /**
   * @brief The process-wide Hyprland client used by the input devices.
   * @details Connects lazily; while no Hyprland instance answers, its calls fail and the connection is retried at
   * most every few seconds. Outputs are cached and re-read after monitor events or once a second.
   * @return nullptr if Wayland support (and with it the Hyprland IPC client) is not compiled in.
   */
  std::shared_ptr<compositor_t> hyprland_compositor();

  /**
   * @brief Maps absolute mouse points onto the layout box.
   *
   * With a compositor, the streamed output is looked up when the port changes and then followed by name through the
   * compositor's live layout. Without one, or if the output is not found, the port's own layout box is used.
   */
  class layout_mapper_t {
  public:
    explicit layout_mapper_t(std::shared_ptr<compositor_t> compositor);

    target_t map(const touch_port_t &port, double local_x, double local_y);

  private:
    std::shared_ptr<compositor_t> compositor_;
    std::optional<touch_port_t> port_;
    std::string output_;
  };

  /**
   * @brief Maps points for one absolute device (a client's touch screen or pen) and keeps it bound to the output
   * being streamed.
   *
   * With a compositor, the device is bound to the output the touch port describes the first time a point is mapped,
   * and again whenever the port or the compositor's generation changes; points are then fractions of that output.
   * Without a compositor, or while binding fails (retried every retry_interval), points are fractions of the
   * port's layout box.
   */
  class output_mapper_t {
  public:
    static constexpr std::chrono::seconds retry_interval {2};

    output_mapper_t(std::string evdev_name, std::shared_ptr<compositor_t> compositor);

    target_t map(const touch_port_t &port, double local_x, double local_y);

    /**
     * @brief Size, in display-local units, of the box the device's axes span for the last map() with @p port:
     * the streamed output while the device is bound to it, otherwise the port's layout box. Used to convert
     * lengths such as the touch contact area to device units.
     */
    point_t box_size(const touch_port_t &port) const;

    /**
     * @brief The output the device is bound to, empty if it is not.
     */
    const std::string &bound_output() const;

  private:
    void rebind(const touch_port_t &port);

    std::string evdev_name_;
    std::shared_ptr<compositor_t> compositor_;
    std::optional<touch_port_t> port_;
    std::uint64_t generation_ = 0;
    std::string bound_output_;
    std::chrono::steady_clock::time_point retry_at_ {};
  };

  /**
   * @brief Applies set_flat_accel() to a relative device once per compositor generation.
   */
  class flat_accel_t {
  public:
    flat_accel_t(std::string evdev_name, std::shared_ptr<compositor_t> compositor);

    /**
     * @brief Call before each relative move; talks to the compositor only when needed.
     */
    void ensure();

  private:
    std::string evdev_name_;
    std::shared_ptr<compositor_t> compositor_;
    std::optional<std::uint64_t> applied_generation_;
    std::chrono::steady_clock::time_point retry_at_ {};
  };
}  // namespace platf::abs_input
