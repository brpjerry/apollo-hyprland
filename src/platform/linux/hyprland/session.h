/**
 * @file src/platform/linux/hyprland/session.h
 * @brief Hyprland helpers for a streaming session: waking the streamed output.
 */
#pragma once

// standard includes
#include <string>

// local includes
#include "ipc.h"

namespace hyprland {
  enum class wake_result_e {
    already_on,  ///< The output was on; nothing was sent.
    woken,  ///< DPMS was off and has been switched on.
    not_found,  ///< No output has this exact name (empty names and legacy indexes included).
    failed,  ///< Hyprland refused the dpms dispatch.
  };

  /**
   * @brief Switch DPMS on for one output if it is off. Other outputs are never touched.
   *
   * A DPMS-off output is still advertised to Wayland clients, but Hyprland stops rendering it, so it captures as
   * black (e.g. after hypridle switched the screens off before the stream started).
   * @param output Exact connector name, e.g. `APOLLO-1a2b3c4d`.
   */
  wake_result_e wake_output(const ipc_t &ipc, const std::string &output);
}  // namespace hyprland
