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
    if (!raw->touch) {
      return;
    }
    touch_pen::contact_t contact;
    if (touch.eventType == LI_TOUCH_EVENT_DOWN || touch.eventType == LI_TOUCH_EVENT_MOVE) {
      // touch.x/y are display-local positions divided by the layout size (touch_port.width/height).
      auto t = raw->touch_mapper.map(touch_port, double(touch.x) * touch_port.width, double(touch.y) * touch_port.height);
      contact.x = abs_input::libinput_units(t.at.x, t.min.x, t.max.x, abs_input::touch_max_x);
      contact.y = abs_input::libinput_units(t.at.y, t.min.y, t.max.y, abs_input::touch_max_y);
      contact.pressure = touch_pen::touch_pressure(touch.pressureOrDistance);
      contact.orientation = touch_pen::touch_orientation(touch.rotation);
      // The contact area is in touch-port units, like the position before it is divided by the port size.
      auto box = raw->touch_mapper.box_size(touch_port);
      std::tie(contact.major, contact.minor) = touch_pen::contact_area(touch.contactAreaMajor, touch.contactAreaMinor, touch.rotation, abs_input::touch_max_x / box.x, abs_input::touch_max_y / box.y);
    }
    raw->touch->write(raw->touch_state.handle(touch.eventType, touch.pointerId, contact));
  }
}  // namespace platf::touch
