/**
 * @file src/platform/linux/input/touch_pen.cpp
 * @brief Definitions for adjusting touch and pen events for libvirtualhid's Linux devices.
 */
// local includes
#include "touch_pen.h"

// standard includes
#include <format>

namespace platf::touch_pen {
  namespace {
    /**
     * @brief Half pressure for a contact whose pressure the client does not know.
     */
    float contact_pressure(float pressure) {
      return pressure > 0.0f ? pressure : 0.5f;
    }
  }  // namespace

  std::optional<touch_input_t> touch_event(const touch_input_t &touch, bool contact_down) {
    auto event = touch;
    switch (touch.eventType) {
      case LI_TOUCH_EVENT_HOVER:
        if (!contact_down) {
          return std::nullopt;
        }
        // The finger hovers again: it lifted.
        event.eventType = LI_TOUCH_EVENT_UP;
        return event;
      case LI_TOUCH_EVENT_DOWN:
      case LI_TOUCH_EVENT_MOVE:
        event.pressureOrDistance = contact_pressure(touch.pressureOrDistance);
        return event;
      default:
        return event;
    }
  }

  bool pen_buttons_only(const pen_input_t &pen) {
    return pen.eventType == LI_TOUCH_EVENT_BUTTON_ONLY;
  }

  pen_input_t pen_event(const pen_input_t &pen, pen_state_t &state) {
    auto event = pen;
    if (pen.toolType == LI_TOOL_TYPE_PEN || pen.toolType == LI_TOOL_TYPE_ERASER) {
      state.tool = pen.toolType;
    } else {
      event.toolType = state.tool;
    }

    switch (pen.eventType) {
      case LI_TOUCH_EVENT_DOWN:
      case LI_TOUCH_EVENT_MOVE:
        event.pressureOrDistance = contact_pressure(pen.pressureOrDistance);
        [[fallthrough]];
      case LI_TOUCH_EVENT_HOVER:
      case LI_TOUCH_EVENT_UP:
        state.x = pen.x;
        state.y = pen.y;
        state.in_proximity = true;
        break;
      default:
        event.x = state.x;
        event.y = state.y;
        state.in_proximity = false;
        break;
    }
    return event;
  }

  std::string client_device_suffix(int process_id, int client_id) {
    return std::format(" (client {}-{})", process_id, client_id);
  }
}  // namespace platf::touch_pen
