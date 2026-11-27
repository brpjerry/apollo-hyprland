/**
 * @file src/platform/linux/hyprland/session.h
 * @brief Hyprland helpers for a streaming session: waking the streamed output, closing an app's windows.
 */
#pragma once

// standard includes
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// local includes
#include "display_settings.h"
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
   * @param output Exact connector name, e.g. `APOLLO-1a2b3c4d`, or a legacy numeric index that capture accepts.
   * @param capture_names The Wayland output names in the order capture indexes them; called only for an index
   *        that is no connector's name. Without it, an index is not found.
   */
  wake_result_e wake_output(const ipc_t &ipc, const std::string &output, const std::function<std::vector<std::string>()> &capture_names = {});

  /**
   * @brief Which windows belong to a launched app, for closing them when the app is stopped.
   */
  struct app_windows_query_t {
    int pgid = 0;  ///< Process group of the launched app.
    /// Monitor id of the app's own (virtual) display, or -1. Windows there whose process started after the app
    /// count too: that catches games started through an already running launcher (Steam, Proton, Lutris), whose
    /// windows belong to processes outside the group.
    int stream_monitor = -1;
  };

  /**
   * @brief The addresses of the app's windows, in the order Hyprland lists them.
   *
   * A window belongs to the app if its process is in the group or descends from it (process_belongs_to_group()),
   * or if it is on `stream_monitor` and its process started at or after the group leader (by `/proc` start time).
   * @param read_stat Looks up a process (read_proc_stat() outside tests).
   */
  std::vector<std::string> app_window_addresses(
    const std::vector<client_info_t> &clients,
    const app_windows_query_t &query,
    const std::function<std::optional<proc_stat_t>(int)> &read_stat
  );

  /**
   * @brief Lua dispatcher call asking a window to close, as its close button would:
   * `hl.dsp.window.close({ window = "address:0x..." })`.
   */
  std::string lua_close_window(const std::string &address);

  /**
   * @brief Ask the app's windows to close (the app can still show its "save before exit?" prompt).
   * @param stream_output The app's virtual display, or "" to close only windows of the group's processes.
   * @return How many windows were asked to close; 0 if none were found or Hyprland is unreachable.
   */
  int close_app_windows(
    const ipc_t &ipc,
    int pgid,
    const std::string &stream_output,
    const std::function<std::optional<proc_stat_t>(int)> &read_stat = read_proc_stat
  );
}  // namespace hyprland
