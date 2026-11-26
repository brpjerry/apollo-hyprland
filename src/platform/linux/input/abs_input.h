/**
 * @file src/platform/linux/input/abs_input.h
 * @brief Absolute mouse, touch and pen input on Hyprland: the live layout and binding devices to the streamed output.
 *
 * libvirtualhid creates the devices and Sunshine's touch port (video::make_port(), input::client_to_touchport())
 * places points on the streamed output. What Hyprland 0.57.2 adds (measured with `hyprctl cursorpos`, a `wev` window
 * on a headless output for touch, and read in the v0.57.2 sources: src/pointer/PointerManager.cpp warpAbsolute(),
 * src/managers/input/Touch.cpp onTouchDown(), src/managers/input/InputManager.cpp set*Configs()):
 *
 * - Absolute mouse (libvirtualhid's "<mouse> (Absolute)" device): mapped onto the bounding box of all monitors'
 *   logical rectangles, including a negative origin (0,-560 on the development machine).
 *   `hl.device({ name = ..., output = ... })` is accepted but has no effect: Hyprland never binds libinput pointers to
 *   an output. So points are normalised against the layout box, read live from Hyprland (the touch port's copy goes
 *   stale when another output, e.g. another client's virtual display, is added).
 * - Touch screen: libinput reports it twice, as a pointer that gets the plain name and as a touch device that gets
 *   the name with `-1` appended (Hyprland de-duplicates names across all devices). Unbound, a touch lands on the
 *   *focused* monitor (its position + fraction * logical size), never on the layout box.
 *   `hl.device({ name = "<touch name>", output = "NAME" })` binds it to that output and takes effect at once.
 *   libvirtualhid can bind devices to an output only on KDE, so binding stays here.
 * - Pen tablet: unbound it is mapped onto the layout box like the mouse (unless the user set `input:tablet:output` or
 *   a region for all tablets); `output` binds it to that output's logical box.
 * - Options set with `hl.device()` before the device exists are applied when it appears. They are not kept across a
 *   config reload, hence compositor_t::generation().
 * - Scale and rotation: everything is in logical pixels. A bound device is mapped onto the output's logical box,
 *   which is already rotated (HDMI-A-1, 3840x2160 at scale 1.5 with transform 1, is 1440x2560 at 2560,-560).
 *   Hyprland's screencopy delivers rotated outputs as upright frames (2160x3840), so the touch port carries no
 *   rotation and input::client_to_touchport() yields coordinates in that same rotated logical space.
 * - libinput normalises an axis as value / (max - min + 1), so the raw value max/2 lands just short of the middle,
 *   and the top-left edge of an output can land on its neighbour; libvirtualhid's rounding to the nearest unit adds
 *   up to half a unit more. libinput_units() compensates: for the mouse always, for touch and pen once they are bound
 *   to the output (unbound, they keep Sunshine's coordinates).
 * - Relative motion goes through the user's `input:sensitivity` and accel profile (with `sensitivity = -0.5`, 10 units
 *   moved the pointer 5 px). `accel_profile = "flat"` with `sensitivity = 0` on the device makes it 1:1.
 *
 * Without Hyprland (other compositors, X11) none of this applies and Sunshine's own mapping is used unchanged.
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
   * @brief Axis maximum of libvirtualhid's absolute mouse (both axes) on Linux.
   */
  constexpr int mouse_axis_max = 65535;

  /**
   * @brief Horizontal axis maximum of libvirtualhid's touch screen and pen tablet on Linux.
   */
  constexpr int touch_axis_max_x = 19200;

  /**
   * @brief Vertical axis maximum of libvirtualhid's touch screen and pen tablet on Linux.
   */
  constexpr int touch_axis_max_y = 10800;

  /**
   * @brief A point in fractions of a box.
   */
  struct point_t {
    double x;  ///< Horizontal fraction.
    double y;  ///< Vertical fraction.
  };

  /**
   * @brief A point as fractions of a box, with the part of the box the point must stay in (the streamed output).
   */
  struct target_t {
    point_t at;  ///< The point.
    point_t min {0, 0};  ///< Top-left of the streamed output.
    point_t max {1, 1};  ///< Bottom-right of the streamed output.
  };

  /**
   * @brief A rectangle in logical layout coordinates; for outputs, `name` is the connector name.
   */
  struct output_rect_t {
    std::string name;  ///< Connector name, empty for a bounding box.
    int x;  ///< Left edge.
    int y;  ///< Top edge.
    int width;  ///< Width.
    int height;  ///< Height.
  };

  /**
   * @brief Fraction of the layout bounding box for an absolute mouse point, from the port alone.
   * @param port The platform port input.cpp hands to platf::abs_mouse(): offset_x/offset_y is the streamed output's
   *             top-left, width/height the size of the layout box, env_offset_x/env_offset_y its top-left and
   *             logical_width/logical_height the size of the output (optional, limits the target to the output).
   * @param x Layout x (output offset + display-local x), as passed to platf::abs_mouse().
   * @param y Layout y.
   * @return The target.
   */
  target_t layout_target(const touch_port_t &port, double x, double y);

  /**
   * @brief layout_target() against a live layout: `output` is where the streamed output is now and `layout` the
   * bounding box of all outputs now. Display-local coordinates are rescaled if the output's size changed since `port`.
   * @param port Port of the stream, as for layout_target(const touch_port_t &, double, double).
   * @param output The streamed output in the live layout.
   * @param layout Bounding box of the live layout.
   * @param x Layout x as passed to platf::abs_mouse().
   * @param y Layout y.
   * @return The target.
   */
  target_t layout_target(const touch_port_t &port, const output_rect_t &output, const output_rect_t &layout, double x, double y);

  /**
   * @brief Device value for one axis of a target: corrects for libinput dividing by (max + 1), rounds up to a whole
   * unit and keeps the result inside [min, max) of the target, i.e. on the streamed output.
   * @details libinput then puts it less than one unit past `at`, and never before it.
   * @param at Target fraction.
   * @param min Lowest fraction allowed.
   * @param max Fraction where the next output starts.
   * @param axis_max Axis maximum of the device.
   * @return Device value.
   */
  int libinput_units(double at, double min, double max, int axis_max);

  /**
   * @brief libinput_units() for the whole box.
   * @param fraction Target fraction.
   * @param axis_max Axis maximum of the device.
   * @return Device value.
   */
  int libinput_units(double fraction, int axis_max);

  /**
   * @brief libinput_units() as a fraction of @p axis_max, for libvirtualhid devices that take [0, 1] and round it to
   *        the nearest unit (the touch screen and pen tablet).
   * @param fraction Target fraction of the output.
   * @param axis_max Axis maximum of the device.
   * @return The fraction to hand the device.
   */
  float libinput_fraction(double fraction, int axis_max);

  /**
   * @brief The name Hyprland gives a device: lower case, spaces and newlines replaced by `-`.
   * @param evdev_name Name the device was created with.
   * @return Compositor device name.
   */
  std::string compositor_device_name(std::string_view evdev_name);

  /**
   * @brief Whether a compositor device name belongs to a device created as `base`, i.e. is `base` or `base-N`
   * (Hyprland appends `-N` when the name is taken, e.g. by the pointer half of a touch screen).
   * @param name Compositor device name.
   * @param base compositor_device_name() of the evdev name.
   * @return Whether it is an instance.
   */
  bool is_instance_of(std::string_view name, std::string_view base);

  /**
   * @brief The logical rectangle of an output from its mode: mode size divided by scale, swapped for 90 and 270 degree
   * transforms (odd wl_output_transform values), at its layout position.
   * @param name Connector name.
   * @param x Layout x.
   * @param y Layout y.
   * @param mode_width Mode width.
   * @param mode_height Mode height.
   * @param scale Output scale.
   * @param transform wl_output transform.
   * @return The logical rectangle.
   */
  output_rect_t logical_rect(std::string name, int x, int y, int mode_width, int mode_height, double scale, int transform);

  /**
   * @brief Find the output a port describes: by its output_name, else the one whose top-left is the port's offset
   * (preferring the one of the port's logical size if several share that position, e.g. mirrors).
   * @param outputs Live outputs.
   * @param port Mouse port (logical_width/height is the output size) or touch port (width/height is).
   * @return The output, or nullopt.
   */
  std::optional<output_rect_t> find_output(const std::vector<output_rect_t> &outputs, const touch_port_t &port);

  /**
   * @brief Find an output by name.
   * @param outputs Live outputs.
   * @param name Connector name.
   * @return The output, or nullopt.
   */
  std::optional<output_rect_t> find_output(const std::vector<output_rect_t> &outputs, std::string_view name);

  /**
   * @brief Bounding box of all outputs (named "").
   * @param outputs Live outputs.
   * @return The box, or nullopt if there are no outputs.
   */
  std::optional<output_rect_t> bounding_box(const std::vector<output_rect_t> &outputs);

  /**
   * @brief Shared list of outputs.
   */
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
     * @param evdev_name Name the device was created with.
     * @param output Connector name; empty removes the binding.
     * @return Whether the compositor took it.
     */
    virtual bool bind_to_output(const std::string &evdev_name, const std::string &output) = 0;

    /**
     * @brief Make relative motion of the device 1:1 (flat acceleration, no sensitivity change).
     * @param evdev_name Name the device was created with.
     * @return Whether the compositor took it.
     */
    virtual bool set_flat_accel(const std::string &evdev_name) = 0;

    /**
     * @brief Changes whenever per-device settings made earlier may have been lost (reconnect, config reload).
     * @return The generation.
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

#ifdef SUNSHINE_TESTS
  /**
   * @brief What the Linux input entry points may reach in tests, for input created afterwards.
   * @details Unit tests can run inside a desktop session, so by default hyprland_compositor() is nullptr there and
   *          text is not typed through the session's Wayland virtual keyboard.
   * @param compositor What hyprland_compositor() returns: a fake, real_hyprland_compositor() or nullptr.
   * @param virtual_keyboard Whether text may go through the Wayland virtual keyboard.
   */
  void set_compositor_for_testing(std::shared_ptr<compositor_t> compositor, bool virtual_keyboard = false);

  /**
   * @brief Whether a test allowed the Wayland virtual keyboard (set_compositor_for_testing()).
   * @return The setting.
   */
  bool virtual_keyboard_for_testing();

  /**
   * @brief The real Hyprland client, for live tests.
   * @return What hyprland_compositor() returns outside tests.
   */
  std::shared_ptr<compositor_t> real_hyprland_compositor();
#endif

  /**
   * @brief Maps absolute mouse points onto the live layout box.
   *
   * While the compositor answers, the streamed output is looked up when the port changes and then followed by name
   * through the compositor's live layout; if it is not found, the port's own layout box is used.
   */
  class layout_mapper_t {
  public:
    /**
     * @brief Construct a mapper.
     * @param compositor The compositor, or nullptr.
     */
    explicit layout_mapper_t(std::shared_ptr<compositor_t> compositor);

    /**
     * @brief Map a point.
     * @param port Port as for layout_target().
     * @param x Layout x as passed to platf::abs_mouse().
     * @param y Layout y.
     * @return The target, or nullopt when no compositor answers (then Sunshine's own mapping applies).
     */
    std::optional<target_t> map(const touch_port_t &port, double x, double y);

  private:
    std::shared_ptr<compositor_t> compositor_;
    std::optional<touch_port_t> port_;
    std::string output_;
  };

  /**
   * @brief Keeps one absolute device (a client's touch screen or pen) bound to the output being streamed.
   *
   * The device is bound to the output the port names the first time, and again whenever the port or the compositor's
   * generation changes. While binding fails it is retried every retry_interval.
   */
  class output_binder_t {
  public:
    /**
     * @brief How long to wait before trying to bind again after a failure.
     */
    static constexpr std::chrono::seconds retry_interval {2};

    /**
     * @brief Construct a binder.
     * @param evdev_name Name the device was created with.
     * @param compositor The compositor, or nullptr.
     */
    output_binder_t(std::string evdev_name, std::shared_ptr<compositor_t> compositor);

    /**
     * @brief Bind the device to the output of @p port if that has not happened yet.
     * @param port Touch port of the event (monitor-local, as input.cpp builds it for touch and pen).
     * @return Whether the device is bound to the port's output.
     */
    bool update(const touch_port_t &port);

    /**
     * @brief Remove the binding, if there is one.
     */
    void unbind();

    /**
     * @brief The output the device is bound to.
     * @return Connector name, empty if it is not bound.
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
    /**
     * @brief Construct the helper.
     * @param compositor The compositor, or nullptr.
     */
    explicit flat_accel_t(std::shared_ptr<compositor_t> compositor);

    /**
     * @brief Call before each relative move; talks to the compositor only when needed.
     * @param evdev_name Name of the relative device; a new name is set up again.
     */
    void ensure(const std::string &evdev_name);

  private:
    std::shared_ptr<compositor_t> compositor_;
    std::string evdev_name_;
    std::optional<std::uint64_t> applied_generation_;
    std::chrono::steady_clock::time_point retry_at_ {};
  };
}  // namespace platf::abs_input
