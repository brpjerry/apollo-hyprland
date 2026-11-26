/**
 * @file src/platform/linux/input/touch_pen.h
 * @brief What the per-client libvirtualhid touch screen and pen tablet get for a client event on Linux (HYP-24).
 * @details libvirtualhid's Linux touch screen and pen tablet take Sunshine's events as they are, which libinput gets
 *          wrong in a few cases. These functions adjust the events before they reach the devices:
 *          - touch hover is not a touch (Linux touch screens have no hover): a hover over nothing is dropped, and a
 *            hover over a contact that is down lifts it;
 *          - a contact with unknown pressure (0) gets half pressure;
 *          - a pen with an unknown tool keeps the tool it had (a pen at first), so it still comes into proximity;
 *          - a pen contact with unknown pressure still puts the tip down (libinput decides contact by pressure);
 *          - events without a position (leaving, cancels) keep the last position instead of jumping to 0,0;
 *          - a button-only event only changes buttons (see pen_buttons_only()).
 *          libvirtualhid cannot report touch cancels as palms, contact areas or pen rotation, and never takes the pen
 *          out of proximity; those are left as they are.
 */
#pragma once

// standard includes
#include <cstdint>
#include <optional>
#include <string>

// local includes
#include "src/platform/common.h"

namespace platf::touch_pen {
  /**
   * @brief The touch event for the device.
   * @param touch Client event.
   * @param contact_down Whether the event's pointer has a contact down on the device.
   * @return The event to submit, or nullopt to drop it.
   */
  std::optional<touch_input_t> touch_event(const touch_input_t &touch, bool contact_down);

  /**
   * @brief What the pen tablet remembers between events.
   */
  struct pen_state_t {
    std::uint8_t tool = LI_TOOL_TYPE_PEN;  ///< Tool of the last event that named one.
    float x = 0;  ///< Last known position.
    float y = 0;  ///< Last known position.
    bool in_proximity = false;  ///< Whether the last event left the tool over the device.
  };

  /**
   * @brief Whether a pen event only changes buttons: the device must not move, lift or land the tool for it.
   * @param pen Client event.
   * @return True for LI_TOUCH_EVENT_BUTTON_ONLY.
   */
  bool pen_buttons_only(const pen_input_t &pen);

  /**
   * @brief The pen event for the device.
   * @param pen Client event (not a button-only one).
   * @param state Updated with the event's tool and position.
   * @return The event to submit.
   */
  pen_input_t pen_event(const pen_input_t &pen, pen_state_t &state);

  /**
   * @brief Suffix for a client's device names, unique on the host, e.g. " (client 4242-3)".
   * @details Hyprland binds devices to an output by name, so clients must not share names, not even with the clients
   * of another Apollo process on the same compositor: the process id keeps them apart.
   * @param process_id Id of this process.
   * @param client_id Client number within the process.
   * @return The suffix.
   */
  std::string client_device_suffix(int process_id, int client_id);
}  // namespace platf::touch_pen
