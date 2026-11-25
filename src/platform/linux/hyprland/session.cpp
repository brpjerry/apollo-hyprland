/**
 * @file src/platform/linux/hyprland/session.cpp
 * @brief Hyprland helpers for a streaming session.
 */
// local includes
#include "session.h"

#include "src/logging.h"

using namespace std::literals;

namespace hyprland {
  wake_result_e wake_output(const ipc_t &ipc, const std::string &output) {
    if (output.empty()) {
      return wake_result_e::not_found;
    }
    for (const auto &monitor : ipc.monitors(true)) {
      if (monitor.name != output) {
        continue;
      }
      if (monitor.dpms_on) {
        return wake_result_e::already_on;
      }
      if (!ipc.set_dpms(output, true)) {
        BOOST_LOG(warning) << "Hyprland: could not switch on output "sv << output;
        return wake_result_e::failed;
      }
      BOOST_LOG(info) << "Hyprland: switched on output "sv << output << " (DPMS was off)"sv;
      return wake_result_e::woken;
    }
    return wake_result_e::not_found;
  }
}  // namespace hyprland
