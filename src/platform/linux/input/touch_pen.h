/**
 * @file src/platform/linux/input/touch_pen.h
 * @brief Per-client virtual touch screen and pen tablet (uinput), with the event logic kept testable (HYP-24).
 * @details inputtino's touch screen and pen tablet cannot report contact area or pen rotation, reuse multi-touch
 *          slots, never take the pen out of proximity and have no touch cancel. These devices keep inputtino's
 *          names, IDs and axis ranges (so compositor bindings and the mapping in inputtino_abs.h stay the same) and
 *          add:
 *          - touch hover is not a touch (Linux touch screens have no hover), and hovering over an active contact
 *            lifts it;
 *          - LI_TOUCH_EVENT_CANCEL / CANCEL_ALL mark the contacts as palms before lifting them, which libinput
 *            turns into a touch cancel (wl_touch.cancel) instead of a completed tap;
 *          - ABS_MT_TOUCH_MAJOR/MINOR contact area and ABS_MT_ORIENTATION;
 *          - pen proximity in/out, a tip that libinput detects even when the client does not know the pressure,
 *            tilt in degrees and rotation on ABS_Z.
 *          The state machines only produce events, so tests can check them without uinput.
 */
#pragma once

// standard includes
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// lib includes
#include <linux/input.h>  // event codes and MT_TOOL_*

// local includes
#include "inputtino_abs.h"

struct libevdev_uinput;

namespace platf::touch_pen {
  /**
   * @brief One evdev event.
   */
  struct event_t {
    std::uint16_t type;
    std::uint16_t code;
    std::int32_t value;

    bool operator==(const event_t &) const = default;
  };

  using events_t = std::vector<event_t>;

  namespace touch_axes {
    inline constexpr int max_x = abs_input::touch_max_x;
    inline constexpr int max_y = abs_input::touch_max_y;
    inline constexpr int slots = 16;
    inline constexpr int pressure_max = 253;
    inline constexpr int major_max = abs_input::touch_max_x;  ///< Contact axes use the position units
  }  // namespace touch_axes

  namespace pen_axes {
    inline constexpr int max_x = abs_input::pen_max_x;
    inline constexpr int max_y = abs_input::pen_max_y;
    inline constexpr int resolution = 28;  ///< Units per mm on X and Y
    inline constexpr int pressure_max = 4096;
    inline constexpr int distance_max = 1024;
    inline constexpr int tilt_resolution = 57;  ///< Units per radian: one unit is about one degree
    inline constexpr int rotation_max = 359;  ///< ABS_Z, one unit per degree
  }  // namespace pen_axes

  /**
   * @brief A touch contact in device units.
   */
  struct contact_t {
    int x = 0;
    int y = 0;
    int pressure = 0;  ///< 0..touch_axes::pressure_max
    int major = 0;  ///< Contact ellipse axes, 0 when unknown
    int minor = 0;
    int orientation = 0;  ///< -90..90 degrees from vertical
  };

  /**
   * @brief ABS_MT_ORIENTATION for a Moonlight rotation (degrees clockwise from vertical, 0..359): the ellipse's
   *        major axis direction folded into -90..90. Unknown rotation is 0 (vertical).
   */
  int touch_orientation(std::uint16_t rotation);

  /**
   * @brief Touch pressure in device units; contacts with unknown pressure (0) get half pressure.
   */
  int touch_pressure(float pressure);

  /**
   * @brief Contact ellipse axes in device units.
   * @param major Major axis in the units of the touch port (input.cpp scales the client's value to it).
   * @param minor Minor axis, same units.
   * @param rotation Direction of the major axis (Moonlight rotation) or LI_ROT_UNKNOWN; it matters when the device
   *        has different scales on X and Y. Unknown is treated as 45 degrees, as on Windows.
   * @param units_x Device units per touch-port unit on X.
   * @param units_y Device units per touch-port unit on Y.
   * @return {major, minor}, both 0 when the client sent no contact area.
   */
  std::pair<int, int> contact_area(float major, float minor, std::uint16_t rotation, double units_x, double units_y);

  /**
   * @brief Multi-touch (protocol type B) state for the virtual touch screen.
   */
  class touch_state_t {
  public:
    /**
     * @brief Events for one Moonlight touch event (LI_TOUCH_EVENT_*); @p contact is ignored for lifts and cancels.
     * @return Events ending in SYN_REPORT, or nothing when there is nothing to report (e.g. a hover, or a new
     *         contact while all slots are taken).
     */
    events_t handle(std::uint8_t event_type, std::uint32_t pointer_id, const contact_t &contact);

    /**
     * @brief Number of contacts down.
     */
    int contacts() const;

  private:
    struct slot_t {
      bool active = false;
      std::uint32_t pointer_id = 0;
      bool palm = false;  ///< ABS_MT_TOOL_TYPE of this slot is MT_TOOL_PALM
    };

    events_t place(std::uint32_t pointer_id, const contact_t &contact);
    void lift(events_t &events, int slot, bool cancel);
    void select(events_t &events, int slot);
    int find(std::uint32_t pointer_id) const;

    std::array<slot_t, touch_axes::slots> slots_ {};
    int current_slot_ = 0;
    int next_tracking_id_ = 0;
  };

  /**
   * @brief A pen sample in device units.
   */
  struct pen_sample_t {
    int tool = 0;  ///< BTN_TOOL_PEN or BTN_TOOL_RUBBER; 0 keeps the current tool (pen if there is none)
    int x = 0;
    int y = 0;
    float pressure = 0;  ///< 0..1, 0 when unknown (contacts then get half pressure)
    float distance = 0;  ///< 0..1 for hovers
    int tilt_x = 0;  ///< Degrees, -90..90
    int tilt_y = 0;
    int rotation = -1;  ///< Degrees clockwise, 0..359, or -1 when unknown
    std::uint8_t buttons = 0;  ///< LI_PEN_BUTTON_*
  };

  /**
   * @brief Converts a Moonlight pen event's tilt (0..90 from vertical, or LI_TILT_UNKNOWN) and rotation (0..359, or
   *        LI_ROT_UNKNOWN) into ABS_TILT_X/Y degrees; both are 0 unless both are known.
   */
  std::pair<int, int> pen_tilt(std::uint8_t tilt, std::uint16_t rotation);

  /**
   * @brief Tablet tool state for the virtual pen tablet.
   */
  class pen_state_t {
  public:
    /**
     * @brief Events for one Moonlight pen event (LI_TOUCH_EVENT_*).
     */
    events_t handle(std::uint8_t event_type, const pen_sample_t &sample);

    /**
     * @brief The tool in proximity (BTN_TOOL_*), 0 when none.
     */
    int tool() const;

    /**
     * @brief Whether the tip is down.
     */
    bool touching() const;

  private:
    void proximity_in(events_t &events, int tool);
    void proximity_out(events_t &events);
    void axes(events_t &events, const pen_sample_t &sample);
    void buttons(events_t &events, std::uint8_t buttons);

    int tool_ = 0;
    int last_tool_ = BTN_TOOL_PEN;
    bool touching_ = false;
    std::uint8_t buttons_ = 0;
  };

  /**
   * @brief A uinput device created through libevdev.
   */
  class uinput_device_t {
  public:
    /**
     * @brief The virtual touch screen (same capabilities as inputtino's, plus contact area and palm tool type).
     * @return nullptr with @p error set when uinput is not available.
     */
    static std::unique_ptr<uinput_device_t> touch_screen(const std::string &name, std::string &error);

    /**
     * @brief The virtual pen tablet (same capabilities as inputtino's, plus ABS_Z rotation).
     */
    static std::unique_ptr<uinput_device_t> pen_tablet(const std::string &name, std::string &error);

    ~uinput_device_t();
    uinput_device_t(const uinput_device_t &) = delete;
    uinput_device_t &operator=(const uinput_device_t &) = delete;

    void write(const events_t &events);

    /**
     * @brief The device's /dev/input/event* node.
     */
    std::string devnode() const;

  private:
    explicit uinput_device_t(libevdev_uinput *device);
    libevdev_uinput *device_;
  };

  /**
   * @brief Whether this process can create uinput devices (/dev/uinput is writable).
   */
  bool uinput_available();
}  // namespace platf::touch_pen
