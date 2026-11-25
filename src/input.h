/**
 * @file src/input.h
 * @brief Declarations for gamepad, keyboard, and mouse input handling.
 */
#pragma once

// standard includes
#include <functional>

// local includes
#include "platform/common.h"
#include "thread_safe.h"
#include "crypto.h"

namespace input {
  struct input_t;

  void print(void *input);
  void reset(std::shared_ptr<input_t> &input);
  void passthrough(std::shared_ptr<input_t> &input, std::vector<std::uint8_t> &&input_data, const crypto::PERM& permission);

  [[nodiscard]] std::unique_ptr<platf::deinit_t> init();

  bool probe_gamepads();

  std::shared_ptr<input_t> alloc(safe::mail_t mail);

  /**
   * @brief Mapping from client stream coordinates to host desktop coordinates.
   * @details Built by video::make_port() from the capturing platf::display_t.
   *   - offset_x/offset_y: top-left of the streamed display in the host desktop space. On Wayland this is the
   *     output's logical position in the compositor layout (can be negative).
   *   - width/height: the client's stream size (config.width/height), used to scale client surface coordinates.
   *   - env_width/env_height: size of the bounding box of all displays in the desktop space.
   *   - env_x/env_y: top-left of that bounding box (0 unless a display has negative coordinates).
   *   - client_offsetX/client_offsetY: letterbox offsets of the frame inside the stream, in stream pixels.
   *   - scalar_inv: multiply letterbox-corrected stream pixels by this to get display-local desktop units, i.e.
   *     values in [0, logical width] x [0, logical height] measured from offset_x/offset_y.
   * Absolute desktop position = (offset_x + local_x, offset_y + local_y); normalised against the whole desktop it is
   * ((offset_x + local_x - env_x) / env_width, (offset_y + local_y - env_y) / env_height).
   */
  struct touch_port_t: public platf::touch_port_t {
    int env_width, env_height;

    // Offset x and y coordinates of the client
    float client_offsetX, client_offsetY;

    float scalar_inv;

    // Top-left corner of the env_width x env_height bounding box (see above).
    int env_x {0}, env_y {0};

    explicit operator bool() const {
      return width != 0 && height != 0 && env_width != 0 && env_height != 0;
    }
  };

  /**
   * @brief Scale the ellipse axes according to the provided size.
   * @param val The major and minor axis pair.
   * @param rotation The rotation value from the touch/pen event.
   * @param scalar The scalar cartesian coordinate pair.
   * @return The major and minor axis pair.
   */
  std::pair<float, float> scale_client_contact_area(const std::pair<float, float> &val, uint16_t rotation, const std::pair<float, float> &scalar);
}  // namespace input
