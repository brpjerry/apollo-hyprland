/**
 * @file src/platform/linux/hyprland/display_settings.cpp
 * @brief Definitions for the per-client settings of Hyprland virtual displays.
 *
 * Behaviour verified against Hyprland 0.57.2 (Lua config):
 * - `hl.monitor()` merges into the output's existing rule, and the rule outlives the output: a headless output
 *   re-created under the same name immediately takes the mode and scale of the last rule.
 * - Hyprland snaps scales it cannot represent exactly: 1.5 on 1280x800 becomes 1.6, 1.2 on 2560x1600 becomes 1.25.
 *   Learning therefore compares what j/monitors reports, never what was asked for.
 * - `bitdepth = 10` leaves a headless output at XRGB8888, so a bit depth cannot be learned from a headless output.
 * - `HL.Monitor:set_workspace("name:x")` creates the workspace on that monitor, or moves it there from another
 *   monitor, and makes it active without moving focus.
 * - `hl.dsp.window.move({ workspace = ..., window = "address:0x...", follow = false })` moves that window (not the
 *   focused one) and leaves focus where it was.
 */
// this include
#include "display_settings.h"

// standard includes
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>
#include <fstream>
#include <sstream>

// platform includes
#include <unistd.h>

// local includes
#include "src/logging.h"
#include "vdisplay.h"

using namespace std::literals;

namespace hyprland {
  namespace {
    constexpr int settings_version = 1;
    // Hyprland reports scales with a few decimals; anything closer than this is the same scale.
    constexpr double scale_epsilon = 1e-3;

    template<class T>
    std::optional<T> get_opt(const nlohmann::json &json, const char *key) {
      auto it = json.find(key);
      if (it == json.end()) {
        return std::nullopt;
      }
      if constexpr (std::is_same_v<T, std::string>) {
        if (it->is_string()) {
          return it->template get<std::string>();
        }
      } else if constexpr (std::is_same_v<T, double>) {
        if (it->is_number()) {
          return it->template get<double>();
        }
      } else {
        if (it->is_number_integer()) {
          return it->template get<T>();
        }
      }
      return std::nullopt;
    }

    bool all_digits(std::string_view s) {
      return !s.empty() && std::ranges::all_of(s, [](char c) {
        return c >= '0' && c <= '9';
      });
    }

    /// Round a scale for the settings file, so it does not show binary noise such as 1.6000000000000001.
    double round_scale(double scale) {
      return std::round(scale * 1e6) / 1e6;
    }
  }  // namespace

  bool display_settings_t::empty() const {
    return !scale && !scale_percent && !transform && !bitdepth && !position && !workspace;
  }

  nlohmann::json settings_to_json(const display_settings_t &s) {
    nlohmann::json json = nlohmann::json::object();
    if (!s.client_name.empty()) {
      json["client"] = s.client_name;
    }
    if (s.scale) {
      json["scale"] = round_scale(*s.scale);
    }
    if (s.scale_percent) {
      json["scale_factor"] = *s.scale_percent;
    }
    if (s.transform) {
      json["transform"] = *s.transform;
    }
    if (s.bitdepth) {
      json["bitdepth"] = *s.bitdepth;
    }
    if (s.position) {
      json["position"] = *s.position;
    }
    if (s.workspace) {
      json["workspace"] = *s.workspace;
    }
    return json;
  }

  display_settings_t settings_from_json(const nlohmann::json &json) {
    display_settings_t s;
    if (!json.is_object()) {
      return s;
    }
    s.client_name = get_opt<std::string>(json, "client").value_or("");
    if (auto v = get_opt<double>(json, "scale"); v && std::isfinite(*v) && *v > 0.0 && *v <= 10.0) {
      s.scale = *v;
    }
    if (auto v = get_opt<int>(json, "scale_factor"); v && *v > 0) {
      s.scale_percent = *v;
    }
    if (auto v = get_opt<int>(json, "transform"); v && *v >= 0 && *v <= 7) {
      s.transform = *v;
    }
    if (auto v = get_opt<int>(json, "bitdepth"); v && (*v == 8 || *v == 10)) {
      s.bitdepth = *v;
    }
    if (auto v = get_opt<std::string>(json, "position"); v && !v->empty()) {
      s.position = *v;
    }
    if (auto v = get_opt<std::string>(json, "workspace"); v && !v->empty()) {
      s.workspace = *v;
    }
    return s;
  }

  display_settings_store_t::display_settings_store_t(std::filesystem::path path):
      path_ {std::move(path)} {
  }

  const std::filesystem::path &display_settings_store_t::path() const {
    return path_;
  }

  std::map<std::string, nlohmann::json> display_settings_store_t::read_all() const {
    std::map<std::string, nlohmann::json> result;
    if (path_.empty()) {
      return result;
    }
    std::ifstream in {path_};
    if (!in) {
      return result;
    }
    auto json = nlohmann::json::parse(in, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
      BOOST_LOG(warning) << "Hyprland virtual display: ignoring unreadable settings file "sv << path_.string();
      return result;
    }
    auto displays = json.find("displays");
    if (displays == json.end() || !displays->is_object()) {
      return result;
    }
    for (auto it = displays->begin(); it != displays->end(); ++it) {
      result[it.key()] = it.value();
    }
    return result;
  }

  display_settings_t display_settings_store_t::load(const std::string &output) const {
    auto all = read_all();
    auto it = all.find(output);
    return it == all.end() ? display_settings_t {} : settings_from_json(it->second);
  }

  bool display_settings_store_t::save(const std::string &output, const display_settings_t &settings) const {
    if (path_.empty()) {
      return false;
    }
    auto all = read_all();
    if (settings.empty()) {
      all.erase(output);
    } else {
      all[output] = settings_to_json(settings);
    }

    nlohmann::json displays = nlohmann::json::object();
    for (auto &[name, entry] : all) {
      displays[name] = entry;
    }
    const nlohmann::json json {{"version", settings_version}, {"displays", displays}};

    std::error_code ec;
    if (path_.has_parent_path()) {
      std::filesystem::create_directories(path_.parent_path(), ec);
    }
    auto tmp = path_;
    tmp += std::format(".{}.tmp", ::getpid());
    {
      std::ofstream out {tmp, std::ios::trunc};
      out << json.dump(2) << '\n';
      if (!out) {
        BOOST_LOG(warning) << "Hyprland virtual display: cannot write "sv << tmp.string();
        std::filesystem::remove(tmp, ec);
        return false;
      }
    }
    std::filesystem::rename(tmp, path_, ec);
    if (ec) {
      BOOST_LOG(warning) << "Hyprland virtual display: cannot replace "sv << path_.string() << ": "sv << ec.message();
      std::filesystem::remove(tmp, ec);
      return false;
    }
    return true;
  }

  std::string default_workspace_name(const std::string &output) {
    std::string name = output;
    std::ranges::transform(name, name.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    return name;
  }

  std::string workspace_selector(const std::string &workspace) {
    return all_digits(workspace) ? workspace : "name:" + workspace;
  }

  std::optional<int> bitdepth_of_format(std::string_view format) {
    if (format.ends_with("2101010"sv)) {
      return 10;
    }
    if (format.ends_with("8888"sv)) {
      return 8;
    }
    return std::nullopt;
  }

  std::optional<double> choose_scale(const display_settings_t &settings, int scale_percent) {
    if (scale_percent <= 0) {
      scale_percent = 100;
    }
    if (settings.scale && settings.scale_percent.value_or(100) == scale_percent) {
      return settings.scale;
    }
    if (scale_percent != 100) {
      return scale_percent / 100.0;
    }
    return std::nullopt;
  }

  monitor_rule_t build_monitor_rule(const rule_inputs_t &in) {
    monitor_rule_t rule {
      .output = in.output,
      .mode = vdisplay_mode_string(in.mode),
      .position = in.isolated_position ? *in.isolated_position : in.settings.position.value_or("auto-right"),
      .scale = choose_scale(in.settings, in.mode.scale_percent).value_or(1.0),
      .transform = in.settings.transform,
      .bitdepth = in.settings.bitdepth,
    };
    if (!rule.transform && in.emitted.transform) {
      rule.transform = 0;
    }
    if (!rule.bitdepth && in.emitted.bitdepth) {
      rule.bitdepth = 8;
    }
    return rule;
  }

  display_settings_t learn_settings(
    const display_settings_t &stored,
    const monitor_info_t &baseline,
    const monitor_info_t &now,
    bool isolated,
    int scale_percent,
    const std::string &assigned_workspace
  ) {
    auto learned = stored;
    if (std::abs(now.scale - baseline.scale) > scale_epsilon) {
      learned.scale = now.scale;
      learned.scale_percent = scale_percent > 0 ? scale_percent : 100;
    }
    if (now.transform != baseline.transform) {
      learned.transform = now.transform;
    }
    const auto depth_before = bitdepth_of_format(baseline.current_format);
    const auto depth_now = bitdepth_of_format(now.current_format);
    if (depth_before && depth_now && depth_before != depth_now) {
      learned.bitdepth = depth_now;
    }
    if (!isolated && (now.x != baseline.x || now.y != baseline.y)) {
      learned.position = std::format("{}x{}", now.x, now.y);
    }
    if (!assigned_workspace.empty() && !now.active_workspace.empty() && now.active_workspace != assigned_workspace) {
      // The user left another workspace showing on the display: use that one next time. Going back to the default
      // workspace clears the entry again.
      if (now.active_workspace == default_workspace_name(now.name)) {
        learned.workspace.reset();
      } else {
        learned.workspace = now.active_workspace;
      }
    }
    return learned;
  }

  std::optional<proc_stat_t> parse_proc_stat(std::string_view stat) {
    // "pid (comm) state ppid pgrp ...": comm may contain anything, so parse after the last ')'.
    auto close = stat.rfind(')');
    if (close == std::string_view::npos) {
      return std::nullopt;
    }
    std::istringstream rest {std::string {stat.substr(close + 1)}};
    std::string state;
    proc_stat_t result;
    if (!(rest >> state >> result.ppid >> result.pgid)) {
      return std::nullopt;
    }
    return result;
  }

  std::optional<proc_stat_t> read_proc_stat(int pid) {
    if (pid <= 0) {
      return std::nullopt;
    }
    std::ifstream in {std::format("/proc/{}/stat", pid)};
    std::string content;
    if (!std::getline(in, content)) {
      return std::nullopt;
    }
    return parse_proc_stat(content);
  }

  bool process_belongs_to_group(int pid, int pgid, const std::function<std::optional<proc_stat_t>(int)> &read_stat) {
    if (pid <= 0 || pgid <= 0) {
      return false;
    }
    // Bounded, in case of a pid loop in a fake or a racing /proc.
    for (int depth = 0; depth < 64 && pid > 1; ++depth) {
      if (pid == pgid) {
        return true;
      }
      auto stat = read_stat(pid);
      if (!stat) {
        return false;
      }
      if (stat->pgid == pgid) {
        return true;
      }
      pid = stat->ppid;
    }
    return false;
  }

  std::vector<client_info_t> parse_clients(const nlohmann::json &json) {
    std::vector<client_info_t> result;
    if (!json.is_array()) {
      return result;
    }
    for (const auto &obj : json) {
      if (!obj.is_object()) {
        continue;
      }
      client_info_t c;
      c.address = get_opt<std::string>(obj, "address").value_or("");
      c.pid = get_opt<int>(obj, "pid").value_or(-1);
      c.monitor = get_opt<int>(obj, "monitor").value_or(-1);
      if (auto ws = obj.find("workspace"); ws != obj.end() && ws->is_object()) {
        c.workspace = get_opt<std::string>(*ws, "name").value_or("");
      }
      result.push_back(std::move(c));
    }
    return result;
  }

  std::optional<std::string> parse_openwindow_address(std::string_view data) {
    auto address = data.substr(0, data.find(','));
    if (address.starts_with("0x"sv)) {
      address.remove_prefix(2);
    }
    if (address.empty() || !std::ranges::all_of(address, [](char c) {
          return std::isxdigit(static_cast<unsigned char>(c)) != 0;
        })) {
      return std::nullopt;
    }
    return "0x" + std::string {address};
  }

  std::string lua_show_workspace(const std::string &monitor, const std::string &workspace) {
    return std::format(
      R"(local m = hl.get_monitor({}); if not m then return "no monitor" end; m:set_workspace({}); return "ok")",
      lua_quote(monitor),
      lua_quote(workspace_selector(workspace))
    );
  }

  std::string lua_focus_monitor(const std::string &monitor) {
    return std::format("hl.dsp.focus({{ monitor = {} }})", lua_quote(monitor));
  }

  std::string lua_focus_workspace(const std::string &workspace) {
    return std::format("hl.dsp.focus({{ workspace = {} }})", lua_quote(workspace_selector(workspace)));
  }

  std::string lua_move_window(const std::string &address, const std::string &workspace) {
    return std::format(
      "hl.dsp.window.move({{ workspace = {}, window = {}, follow = false }})",
      lua_quote(workspace_selector(workspace)),
      lua_quote("address:" + address)
    );
  }
}  // namespace hyprland
