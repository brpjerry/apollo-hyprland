/**
 * @file src/platform/linux/hyprland/session.cpp
 * @brief Hyprland helpers for a streaming session.
 */
// local includes
#include "session.h"

// standard includes
#include <format>

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

  std::vector<std::string> app_window_addresses(
    const std::vector<client_info_t> &clients,
    const app_windows_query_t &query,
    const std::function<std::optional<proc_stat_t>(int)> &read_stat
  ) {
    std::vector<std::string> result;
    if (query.pgid <= 0) {
      return result;
    }

    std::optional<std::uint64_t> app_start;
    if (query.stream_monitor >= 0) {
      if (auto leader = read_stat(query.pgid); leader && leader->start_time > 0) {
        app_start = leader->start_time;
      }
    }

    for (const auto &client : clients) {
      if (client.address.empty() || client.pid <= 0) {
        continue;
      }
      bool belongs = process_belongs_to_group(client.pid, query.pgid, read_stat);
      if (!belongs && app_start && client.monitor == query.stream_monitor) {
        auto stat = read_stat(client.pid);
        belongs = stat && stat->start_time >= *app_start;
      }
      if (belongs) {
        result.push_back(client.address);
      }
    }
    return result;
  }

  std::string lua_close_window(const std::string &address) {
    return std::format("hl.dsp.window.close({{ window = {} }})", lua_quote("address:" + address));
  }

  int close_app_windows(
    const ipc_t &ipc,
    int pgid,
    const std::string &stream_output,
    const std::function<std::optional<proc_stat_t>(int)> &read_stat
  ) {
    auto clients_json = ipc.request_json("clients");
    if (!clients_json) {
      return 0;
    }
    app_windows_query_t query {.pgid = pgid};
    if (!stream_output.empty()) {
      for (const auto &monitor : ipc.monitors(true)) {
        if (monitor.name == stream_output) {
          query.stream_monitor = monitor.id;
        }
      }
    }

    int closed = 0;
    for (const auto &address : app_window_addresses(parse_clients(*clients_json), query, read_stat)) {
      bool ok = ipc.dialect() == config_dialect_e::lua ? ipc.dispatch_lua(lua_close_window(address)) :
                                                          ipc.dispatch_legacy("closewindow", "address:" + address);
      if (ok) {
        ++closed;
      }
    }
    return closed;
  }
}  // namespace hyprland
