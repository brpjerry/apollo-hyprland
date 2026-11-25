/**
 * @file src/platform/linux/input/inputtino_touch.cpp
 * @brief Definitions for inputtino touch input handling.
 */
// lib includes
#include <boost/locale.hpp>
#include <inputtino/input.hpp>
#include <libevdev/libevdev.h>

// local includes
#include "inputtino_abs.h"
#include "inputtino_common.h"
#include "inputtino_touch.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/utility.h"

using namespace std::literals;

namespace platf::touch {
  void update(client_input_raw_t *raw, const touch_port_t &touch_port, const touch_input_t &touch) {
    if (raw->touch) {
      switch (touch.eventType) {
        case LI_TOUCH_EVENT_HOVER:
        case LI_TOUCH_EVENT_DOWN:
        case LI_TOUCH_EVENT_MOVE:
          {
            // Convert our 0..360 range to -90..90 relative to Y axis
            int adjusted_angle = touch.rotation;

            if (adjusted_angle > 90 && adjusted_angle < 270) {
              // Lower hemisphere
              adjusted_angle = 180 - adjusted_angle;
            }

            // Wrap the value if it's out of range
            if (adjusted_angle > 90) {
              adjusted_angle -= 360;
            } else if (adjusted_angle < -90) {
              adjusted_angle += 360;
            }
            // touch.x/y are display-local positions divided by the layout size (touch_port.width/height).
            auto t = raw->touch_mapper.map(touch_port, double(touch.x) * touch_port.width, double(touch.y) * touch_port.height);
            float fx = abs_input::libinput_fraction(t.at.x, t.min.x, t.max.x, abs_input::touch_max_x);
            float fy = abs_input::libinput_fraction(t.at.y, t.min.y, t.max.y, abs_input::touch_max_y);
            (*raw->touch).place_finger(touch.pointerId, fx, fy, touch.pressureOrDistance, adjusted_angle);
            break;
          }
        case LI_TOUCH_EVENT_CANCEL:
        case LI_TOUCH_EVENT_UP:
        case LI_TOUCH_EVENT_HOVER_LEAVE:
          {
            (*raw->touch).release_finger(touch.pointerId);
            break;
          }
          // TODO: LI_TOUCH_EVENT_CANCEL_ALL
      }
    }
  }
}  // namespace platf::touch
