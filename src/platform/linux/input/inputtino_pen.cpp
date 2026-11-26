/**
 * @file src/platform/linux/input/inputtino_pen.cpp
 * @brief Definitions for inputtino pen input handling.
 */
// lib includes
#include <boost/locale.hpp>
#include <inputtino/input.hpp>
#include <libevdev/libevdev.h>

// local includes
#include "inputtino_abs.h"
#include "inputtino_common.h"
#include "inputtino_pen.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/utility.h"

using namespace std::literals;

namespace platf::pen {
  void update(client_input_raw_t *raw, const touch_port_t &touch_port, const pen_input_t &pen) {
    if (!raw->pen) {
      return;
    }
    touch_pen::pen_sample_t sample;
    switch (pen.toolType) {
      case LI_TOOL_TYPE_PEN:
        sample.tool = BTN_TOOL_PEN;
        break;
      case LI_TOOL_TYPE_ERASER:
        sample.tool = BTN_TOOL_RUBBER;
        break;
      default:
        sample.tool = 0;  // Same as before
        break;
    }
    sample.buttons = pen.penButtons;
    bool positioned = pen.eventType == LI_TOUCH_EVENT_HOVER || pen.eventType == LI_TOUCH_EVENT_DOWN || pen.eventType == LI_TOUCH_EVENT_MOVE || pen.eventType == LI_TOUCH_EVENT_UP;
    if (positioned) {
      // pen.x/y are display-local positions divided by the layout size (touch_port.width/height).
      auto t = raw->pen_mapper.map(touch_port, double(pen.x) * touch_port.width, double(pen.y) * touch_port.height);
      sample.x = abs_input::libinput_units(t.at.x, t.min.x, t.max.x, abs_input::pen_max_x);
      sample.y = abs_input::libinput_units(t.at.y, t.min.y, t.max.y, abs_input::pen_max_y);
      std::tie(sample.tilt_x, sample.tilt_y) = touch_pen::pen_tilt(pen.tilt, pen.rotation);
      sample.rotation = pen.rotation == LI_ROT_UNKNOWN ? -1 : pen.rotation % 360;
      if (pen.eventType == LI_TOUCH_EVENT_HOVER) {
        sample.distance = pen.pressureOrDistance;
      } else {
        sample.pressure = pen.pressureOrDistance;
      }
    }
    raw->pen->write(raw->pen_state.handle(pen.eventType, sample));
  }
}  // namespace platf::pen
