/**
 * @file src/platform/linux/hyprland/settings_manager.cpp
 * @brief Definitions for the display-device settings manager on Hyprland.
 *
 * Behaviour verified against Hyprland 0.57.2 (Lua config) in a private headless instance:
 * - `hl.monitor({ output = X, disabled = true })` and `disabled = false` take effect asynchronously: j/monitors may
 *   still show the old state right after the call, so every change is followed by polling until it shows.
 * - Disabling a monitor moves its workspaces (those with windows; empty ones are destroyed) to a remaining monitor,
 *   FALLBACK if none is left; enabling it again moves them back by itself. Focus can move to another monitor while
 *   monitors are being disabled, so the target is focused again afterwards and its workspace shown again.
 * - Outputs positioned `auto` move when another output changes mode; positions are restored after modes.
 * - A headless output takes any mode, but snaps to a listed mode when the request is close to it.
 * - `cm = "hdr"` and `bitdepth = 10` do nothing on a headless output (it reports `hardwareDetails.hdr = false`).
 */
// this include
#include "settings_manager.h"

// standard includes
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <format>
#include <set>
#include <thread>

// lib includes
#include <display_device/file_settings_persistence.h>

// local includes
#include "display_settings.h"
#include "src/logging.h"
#include "src/platform/vdisplay.h"
#include "vdisplay.h"

using namespace std::literals;

namespace hyprland::dd {
  using display_device::SettingsManagerInterface;
  using ApplyResult = SettingsManagerInterface::ApplyResult;
  using RevertResult = SettingsManagerInterface::RevertResult;
  using DevicePreparation = display_device::SingleDisplayConfiguration::DevicePreparation;

  namespace {
    constexpr int state_version = 1;
    /// Refresh rates closer than this are the same mode when checking what Hyprland reports.
    constexpr double refresh_tolerance_hz = 1.0;
    /// Refresh rates closer than this count as the requested one when choosing among an output's modes.
    constexpr double refresh_match_hz = 0.5;

    template<class T>
    T json_get(const nlohmann::json &obj, const char *key, T fallback) {
      if (!obj.is_object()) {
        return fallback;
      }
      auto it = obj.find(key);
      if (it == obj.end() || it->is_null()) {
        return fallback;
      }
      try {
        return it->get<T>();
      } catch (const nlohmann::json::exception &) {
        return fallback;
      }
    }

    bool same_mode(const output_mode_t &a, const output_mode_t &b) {
      return a.width == b.width && a.height == b.height && std::abs(a.refresh_hz - b.refresh_hz) < refresh_tolerance_hz;
    }

    std::string position_string(int x, int y) {
      return std::format("{}x{}", x, y);
    }

    double to_hz(const display_device::FloatingPoint &value) {
      if (const auto *d = std::get_if<double>(&value)) {
        return *d;
      }
      const auto &r = std::get<display_device::Rational>(value);
      return r.m_denominator == 0 ? 0.0 : static_cast<double>(r.m_numerator) / r.m_denominator;
    }

    saved_output_t save(const output_t &output) {
      saved_output_t saved;
      saved.name = output.name;
      saved.description = output.description;
      saved.enabled = output.enabled;
      saved.mode = output.mode;
      saved.x = output.x;
      saved.y = output.y;
      saved.cm = output.cm;
      saved.bitdepth = output.bitdepth;
      saved.active_workspace = output.active_workspace;
      saved.is_virtual = output.is_virtual;
      return saved;
    }

    /// Fill `d` with the initial values of the fields `t` says were changed.
    void restore_fields(desired_t &d, const saved_output_t &s, const touched_t &t) {
      if (t.enabled) {
        d.enabled = s.enabled;
      }
      if (t.mode) {
        d.mode = s.mode;
      }
      if (t.position) {
        d.position = std::pair {s.x, s.y};
      }
      if (t.hdr) {
        if (s.is_virtual) {
          d.virtual_hdr = s.virtual_hdr;
        } else {
          d.cm = s.cm.empty() ? "srgb"s : s.cm;
          d.bitdepth = s.bitdepth;
        }
      }
    }

    RevertResult revert_result(failure_e failure) {
      switch (failure) {
        case failure_e::unreachable:
          return RevertResult::ApiTemporarilyUnavailable;
        case failure_e::mode:
          return RevertResult::RevertingDisplayModesFailed;
        case failure_e::hdr:
          return RevertResult::RevertingHdrStatesFailed;
        case failure_e::position:
          return RevertResult::RevertingPrimaryDeviceFailed;
        default:
          return RevertResult::SwitchingTopologyFailed;
      }
    }

    std::string describe(const display_device::SingleDisplayConfiguration &config) {
      std::string prep;
      switch (config.m_device_prep) {
        case DevicePreparation::VerifyOnly:
          prep = "verify_only";
          break;
        case DevicePreparation::EnsureActive:
          prep = "ensure_active";
          break;
        case DevicePreparation::EnsurePrimary:
          prep = "ensure_primary";
          break;
        case DevicePreparation::EnsureOnlyDisplay:
          prep = "ensure_only_display";
          break;
      }
      auto text = std::format("device \"{}\", {}", config.m_device_id, prep);
      if (config.m_resolution) {
        text += std::format(", resolution {}x{}", config.m_resolution->m_width, config.m_resolution->m_height);
      }
      if (config.m_refresh_rate) {
        text += std::format(", refresh rate {:.3f}", to_hz(*config.m_refresh_rate));
      }
      if (config.m_hdr_state) {
        text += *config.m_hdr_state == display_device::HdrState::Enabled ? ", HDR on" : ", HDR off";
      }
      return text;
    }
  }  // namespace

  const output_t *layout_t::find(std::string_view name) const {
    auto it = std::ranges::find(outputs, name, &output_t::name);
    return it == outputs.end() ? nullptr : &*it;
  }

  const workspace_t *layout_t::find_workspace(std::string_view name) const {
    auto it = std::ranges::find(workspaces, name, &workspace_t::name);
    return it == workspaces.end() ? nullptr : &*it;
  }

  const saved_output_t *state_t::find(std::string_view name) const {
    auto it = std::ranges::find(initial, name, &saved_output_t::name);
    return it == initial.end() ? nullptr : &*it;
  }

  bool touched_t::any() const {
    return enabled || mode || position || hdr;
  }

  touched_t &touched_t::operator|=(const touched_t &other) {
    enabled |= other.enabled;
    mode |= other.mode;
    position |= other.position;
    hdr |= other.hdr;
    return *this;
  }

  std::optional<output_mode_t> parse_mode_string(std::string_view str) {
    if (str.ends_with("Hz"sv)) {
      str.remove_suffix(2);
    }
    const auto x = str.find('x');
    const auto at = str.find('@');
    if (x == std::string_view::npos || at == std::string_view::npos || at < x) {
      return std::nullopt;
    }
    output_mode_t mode;
    const auto w = str.substr(0, x);
    const auto h = str.substr(x + 1, at - x - 1);
    const auto r = str.substr(at + 1);
    if (std::from_chars(w.data(), w.data() + w.size(), mode.width).ec != std::errc {} ||
        std::from_chars(h.data(), h.data() + h.size(), mode.height).ec != std::errc {} ||
        std::from_chars(r.data(), r.data() + r.size(), mode.refresh_hz).ec != std::errc {}) {
      return std::nullopt;
    }
    if (mode.width <= 0 || mode.height <= 0 || mode.refresh_hz <= 0) {
      return std::nullopt;
    }
    return mode;
  }

  std::string mode_string(const output_mode_t &mode) {
    return std::format("{}x{}@{:.3f}", mode.width, mode.height, mode.refresh_hz);
  }

  std::vector<output_t> parse_outputs(const nlohmann::json &monitors) {
    std::vector<output_t> result;
    if (!monitors.is_array()) {
      return result;
    }
    const auto base = parse_monitors(monitors);
    std::size_t index = 0;
    for (const auto &obj : monitors) {
      if (!obj.is_object()) {
        continue;
      }
      const auto &m = base.at(index++);
      if (m.name.empty() || m.name == "FALLBACK"sv) {
        continue;
      }
      output_t out;
      out.name = m.name;
      out.description = m.description;
      out.id = m.id;
      out.enabled = !m.disabled;
      out.dpms_on = m.dpms_on;
      out.mode = {m.width, m.height, m.refresh_hz};
      out.x = m.x;
      out.y = m.y;
      out.scale = m.scale;
      out.transform = m.transform;
      out.focused = m.focused;
      out.active_workspace = m.active_workspace;
      out.bitdepth = bitdepth_of_format(m.current_format);
      out.cm = json_get<std::string>(obj, "colorManagementPreset", "");
      const auto details = obj.find("hardwareDetails");
      if (details != obj.end()) {
        out.hdr_capable = json_get<bool>(*details, "hdr", false);
        out.any_mode = json_get<std::string>(*details, "backend", "") == "headless"sv;
      }
      if (const auto modes = obj.find("availableModes"); modes != obj.end() && modes->is_array()) {
        for (const auto &mode : *modes) {
          if (mode.is_string()) {
            if (auto parsed = parse_mode_string(mode.get<std::string>())) {
              out.modes.push_back(*parsed);
            }
          }
        }
      }
      out.is_virtual = is_vdisplay_output_name(out.name);
      result.push_back(std::move(out));
    }
    return result;
  }

  std::vector<workspace_t> parse_workspaces(const nlohmann::json &workspaces) {
    std::vector<workspace_t> result;
    if (!workspaces.is_array()) {
      return result;
    }
    for (const auto &obj : workspaces) {
      auto name = json_get<std::string>(obj, "name", "");
      auto monitor = json_get<std::string>(obj, "monitor", "");
      // Special workspaces follow the focused monitor; they are not moved around. They are told apart by their
      // reserved name: named workspaces have negative ids as well.
      if (name.empty() || name == "special"sv || name.starts_with("special:"sv)) {
        continue;
      }
      result.push_back({std::move(name), std::move(monitor)});
    }
    return result;
  }

  bool is_hdr_preset(std::string_view cm) {
    return cm == "hdr"sv || cm == "hdredid"sv;
  }

  std::optional<output_mode_t> choose_mode(const output_t &output, int width, int height, double refresh_hz) {
    if (output.any_mode || output.is_virtual || output.modes.empty()) {
      return output_mode_t {width, height, refresh_hz};
    }
    std::vector<output_mode_t> sized;
    std::ranges::copy_if(output.modes, std::back_inserter(sized), [&](const output_mode_t &m) {
      return m.width == width && m.height == height;
    });
    if (sized.empty()) {
      return std::nullopt;
    }
    std::ranges::sort(sized, {}, &output_mode_t::refresh_hz);
    const auto closest = std::ranges::min_element(sized, {}, [&](const output_mode_t &m) {
      return std::abs(m.refresh_hz - refresh_hz);
    });
    if (std::abs(closest->refresh_hz - refresh_hz) < refresh_match_hz) {
      return *closest;
    }
    const auto above = std::ranges::find_if(sized, [&](const output_mode_t &m) {
      return m.refresh_hz > refresh_hz;
    });
    return above != sized.end() ? *above : sized.back();
  }

  // hyprland_compositor_t

  hyprland_compositor_t::hyprland_compositor_t(options_t options):
      options_ {std::move(options)} {
    if (!options_.get_virtual_hdr) {
      options_.get_virtual_hdr = [](const std::string &name) {
        return platf::vdisplay::get_hdr(name);
      };
    }
    if (!options_.set_virtual_hdr) {
      options_.set_virtual_hdr = [](const std::string &name, bool enable) {
        return platf::vdisplay::set_hdr(name, enable);
      };
    }
  }

  const ipc_t *hyprland_compositor_t::connected() {
    if (ipc_ && ipc_->request("/version"sv)) {
      return ipc_.get();
    }
    ipc_.reset();
    if (options_.signature) {
      ipc_ = ipc_t::connect(options_.hypr_dir.empty() ? runtime_dir() : options_.hypr_dir, *options_.signature);
    } else if (options_.hypr_dir.empty()) {
      ipc_ = ipc_t::connect();
    } else if (auto sig = find_instance_signature(options_.hypr_dir, nullptr, nullptr)) {
      ipc_ = ipc_t::connect(options_.hypr_dir, *sig);
    }
    return ipc_.get();
  }

  std::string hyprland_compositor_t::signature() const {
    return ipc_ ? ipc_->signature() : std::string {};
  }

  std::optional<layout_t> hyprland_compositor_t::layout() {
    const auto *ipc = connected();
    if (!ipc) {
      return std::nullopt;
    }
    auto monitors = ipc->request_json("monitors all"sv);
    auto workspaces = ipc->request_json("workspaces"sv);
    if (!monitors || !monitors->is_array()) {
      return std::nullopt;
    }
    layout_t layout;
    layout.outputs = parse_outputs(*monitors);
    if (workspaces) {
      layout.workspaces = parse_workspaces(*workspaces);
    }
    return layout;
  }

  bool hyprland_compositor_t::merges_rules() const {
    return !ipc_ || ipc_->dialect() == config_dialect_e::lua;
  }

  bool hyprland_compositor_t::allowed(const std::string &output) {
    if (!options_.may_change) {
      return true;
    }
    auto current = layout();
    const auto *out = current ? current->find(output) : nullptr;
    if (!out || !options_.may_change(*out)) {
      BOOST_LOG(error) << "Hyprland display device: refusing to change output "sv << output << " on instance "sv << signature()
                       << " (not allowed by the safety check)"sv;
      return false;
    }
    return true;
  }

  bool hyprland_compositor_t::set_monitor(const monitor_rule_t &rule) {
    const auto *ipc = connected();
    if (!ipc || !allowed(rule.output)) {
      return false;
    }
    return ipc->set_monitor(rule);
  }

  bool hyprland_compositor_t::focus_monitor(const std::string &name) {
    const auto *ipc = connected();
    if (!ipc) {
      return false;
    }
    if (ipc->dialect() == config_dialect_e::lua) {
      return ipc->dispatch_lua(lua_focus_monitor(name));
    }
    return ipc->dispatch_legacy("focusmonitor", name);
  }

  bool hyprland_compositor_t::show_workspace(const std::string &monitor, const std::string &workspace) {
    const auto *ipc = connected();
    if (!ipc || !allowed(monitor)) {
      return false;
    }
    if (ipc->dialect() == config_dialect_e::lua) {
      const auto reply = ipc->eval(lua_show_workspace(monitor, workspace));
      return reply && *reply == "ok"sv;
    }
    return ipc->dispatch_legacy("moveworkspacetomonitor", workspace_selector(workspace) + " " + monitor);
  }

  std::optional<bool> hyprland_compositor_t::virtual_hdr(const std::string &name) {
    return options_.get_virtual_hdr(name);
  }

  bool hyprland_compositor_t::set_virtual_hdr(const std::string &name, bool enable) {
    return options_.set_virtual_hdr(name, enable);
  }

  // state

  nlohmann::json state_to_json(const state_t &state) {
    nlohmann::json json;
    json["version"] = state_version;
    auto &initial = json["initial"] = nlohmann::json::array();
    for (const auto &s : state.initial) {
      nlohmann::json o {
        {"name", s.name},
        {"description", s.description},
        {"enabled", s.enabled},
        {"mode", mode_string(s.mode)},
        {"x", s.x},
        {"y", s.y},
        {"cm", s.cm},
        {"active_workspace", s.active_workspace},
        {"virtual", s.is_virtual},
      };
      if (s.bitdepth) {
        o["bitdepth"] = *s.bitdepth;
      }
      if (s.virtual_hdr) {
        o["virtual_hdr"] = *s.virtual_hdr;
      }
      initial.push_back(std::move(o));
    }
    auto &touched = json["touched"] = nlohmann::json::object();
    for (const auto &[name, t] : state.touched) {
      touched[name] = {{"enabled", t.enabled}, {"mode", t.mode}, {"position", t.position}, {"hdr", t.hdr}};
    }
    auto &workspaces = json["workspaces"] = nlohmann::json::array();
    for (const auto &w : state.workspaces) {
      workspaces.push_back({{"name", w.name}, {"monitor", w.monitor}});
    }
    json["focused"] = state.focused;
    json["focus_changed"] = state.focus_changed;
    return json;
  }

  std::optional<state_t> state_from_json(const nlohmann::json &json) {
    if (!json.is_object() || json_get<int>(json, "version", 0) != state_version) {
      return std::nullopt;
    }
    state_t state;
    const auto initial = json.find("initial");
    if (initial == json.end() || !initial->is_array()) {
      return std::nullopt;
    }
    for (const auto &o : *initial) {
      saved_output_t s;
      s.name = json_get<std::string>(o, "name", "");
      auto mode = parse_mode_string(json_get<std::string>(o, "mode", ""));
      if (s.name.empty() || !mode) {
        return std::nullopt;
      }
      s.mode = *mode;
      s.description = json_get<std::string>(o, "description", "");
      s.enabled = json_get<bool>(o, "enabled", true);
      s.x = json_get<int>(o, "x", 0);
      s.y = json_get<int>(o, "y", 0);
      s.cm = json_get<std::string>(o, "cm", "");
      s.active_workspace = json_get<std::string>(o, "active_workspace", "");
      s.is_virtual = json_get<bool>(o, "virtual", false);
      if (o.contains("bitdepth")) {
        s.bitdepth = json_get<int>(o, "bitdepth", 8);
      }
      if (o.contains("virtual_hdr")) {
        s.virtual_hdr = json_get<bool>(o, "virtual_hdr", false);
      }
      state.initial.push_back(std::move(s));
    }
    if (const auto touched = json.find("touched"); touched != json.end() && touched->is_object()) {
      for (const auto &[name, t] : touched->items()) {
        state.touched[name] = {
          json_get<bool>(t, "enabled", false),
          json_get<bool>(t, "mode", false),
          json_get<bool>(t, "position", false),
          json_get<bool>(t, "hdr", false),
        };
      }
    }
    if (const auto workspaces = json.find("workspaces"); workspaces != json.end() && workspaces->is_array()) {
      for (const auto &w : *workspaces) {
        state.workspaces.push_back({json_get<std::string>(w, "name", ""), json_get<std::string>(w, "monitor", "")});
      }
    }
    state.focused = json_get<std::string>(json, "focused", "");
    state.focus_changed = json_get<bool>(json, "focus_changed", false);
    return state;
  }

  // planning

  std::optional<std::string> resolve_device(const state_t &state, const layout_t &current, const std::string &device_id) {
    if (device_id.empty()) {
      // Wayland has no primary display; Apollo treats the physical output at the origin of the original layout as
      // primary (ensure_primary puts the target there), else the focused one, else the first enabled one.
      std::vector<const saved_output_t *> candidates;
      for (const auto &s : state.initial) {
        if (s.enabled && !s.is_virtual && current.find(s.name)) {
          candidates.push_back(&s);
        }
      }
      for (const auto *s : candidates) {
        if (s->x == 0 && s->y == 0) {
          return s->name;
        }
      }
      for (const auto *s : candidates) {
        if (s->name == state.focused) {
          return s->name;
        }
      }
      if (!candidates.empty()) {
        return candidates.front()->name;
      }
      return std::nullopt;
    }
    if (state.find(device_id) || current.find(device_id)) {
      return device_id;
    }
    std::string_view desc {device_id};
    const bool prefixed = desc.starts_with("desc:"sv);
    if (prefixed) {
      desc.remove_prefix(5);
    }
    for (const auto &s : state.initial) {
      if (!s.description.empty() && (s.description == desc || (prefixed && s.description.starts_with(desc)))) {
        return s.name;
      }
    }
    return std::nullopt;
  }

  std::variant<apply_plan_t, ApplyResult> plan_apply(
    const state_t &state,
    const layout_t &current,
    const display_device::SingleDisplayConfiguration &config
  ) {
    apply_plan_t result;
    result.touched = state.touched;

    // Whatever earlier applies changed goes back first; this apply then overrides what it needs.
    for (const auto &[name, t] : state.touched) {
      const auto *s = state.find(name);
      if (s && current.find(name)) {
        restore_fields(result.plan[name], *s, t);
      }
    }

    const auto target_name = resolve_device(state, current, config.m_device_id);
    const auto *target = target_name ? state.find(*target_name) : nullptr;
    const auto *target_now = target_name ? current.find(*target_name) : nullptr;
    if (!target || !target_now) {
      BOOST_LOG(error) << "Hyprland display device: no output matches \""sv << config.m_device_id << '"';
      return ApplyResult::DevicePrepFailed;
    }
    result.target = target->name;
    auto &d = result.plan[target->name];
    auto &touched = result.touched[target->name];

    const auto set_enabled = [&](const saved_output_t &s, bool enabled) {
      result.plan[s.name].enabled = enabled;
      if (enabled != s.enabled) {
        result.touched[s.name].enabled = true;
      }
    };

    switch (config.m_device_prep) {
      case DevicePreparation::VerifyOnly:
        if (!target_now->enabled) {
          BOOST_LOG(error) << "Hyprland display device: "sv << target->name << " is disabled (dd_configuration_option is verify_only)"sv;
          return ApplyResult::DevicePrepFailed;
        }
        break;
      case DevicePreparation::EnsureActive:
        set_enabled(*target, true);
        break;
      case DevicePreparation::EnsurePrimary:
        {
          // No primary display on Wayland: move the target to the origin and shift the rest of the layout with it.
          // A virtual display stays where vdisplay_t put it (vdisplay_t::remove() would remember the move as the
          // client's own choice); it only gets focus.
          set_enabled(*target, true);
          const int dx = target->is_virtual ? 0 : target->x;
          const int dy = target->is_virtual ? 0 : target->y;
          for (const auto &s : state.initial) {
            if (!current.find(s.name) || s.is_virtual || (dx == 0 && dy == 0)) {
              continue;
            }
            const auto &p = result.plan[s.name];
            const bool enabled = p.enabled.value_or(current.find(s.name)->enabled);
            if (!enabled) {
              continue;
            }
            result.plan[s.name].position = std::pair {s.x - dx, s.y - dy};
            result.touched[s.name].position = true;
          }
          result.focus_target = true;
          break;
        }
      case DevicePreparation::EnsureOnlyDisplay:
        set_enabled(*target, true);
        for (const auto &s : state.initial) {
          const auto *now = current.find(s.name);
          // Other virtual displays belong to other clients' streams; leave them alone.
          if (s.name == target->name || s.is_virtual || !now) {
            continue;
          }
          if (s.enabled || now->enabled) {
            result.plan[s.name].enabled = false;
            result.touched[s.name].enabled = true;
          }
        }
        result.focus_target = true;
        break;
    }

    if (config.m_resolution || config.m_refresh_rate) {
      const int width = config.m_resolution ? static_cast<int>(config.m_resolution->m_width) : target->mode.width;
      const int height = config.m_resolution ? static_cast<int>(config.m_resolution->m_height) : target->mode.height;
      const double hz = config.m_refresh_rate ? to_hz(*config.m_refresh_rate) : target->mode.refresh_hz;
      if (width <= 0 || height <= 0 || hz <= 0) {
        BOOST_LOG(error) << "Hyprland display device: invalid mode "sv << width << 'x' << height << '@' << hz;
        return ApplyResult::DisplayModePrepFailed;
      }
      const auto mode = choose_mode(*target_now, width, height, hz);
      if (!mode) {
        BOOST_LOG(error) << "Hyprland display device: "sv << target->name << " has no "sv << width << 'x' << height << " mode"sv;
        return ApplyResult::DisplayModePrepFailed;
      }
      if (!same_mode(*mode, output_mode_t {width, height, hz})) {
        BOOST_LOG(info) << "Hyprland display device: "sv << target->name << " has no "sv << mode_string({width, height, hz})
                        << " mode, using "sv << mode_string(*mode);
      }
      d.mode = *mode;
      if (!same_mode(*mode, target->mode)) {
        touched.mode = true;
      }
    }

    if (config.m_hdr_state) {
      const bool enable = *config.m_hdr_state == display_device::HdrState::Enabled;
      if (target->is_virtual) {
        if (target->virtual_hdr) {
          d.virtual_hdr = enable;
          if (enable != *target->virtual_hdr) {
            touched.hdr = true;
          }
        } else {
          BOOST_LOG(info) << "Hyprland display device: cannot read the HDR state of "sv << target->name << ", leaving HDR alone"sv;
        }
      } else if (target_now->hdr_capable) {
        const bool initially = is_hdr_preset(target->cm);
        // Already in the requested state: keep the user's preset (hdredid, say) and bit depth. Only a change is
        // recorded for revert, so anything changed here would not come back.
        if (enable != initially) {
          d.cm = enable ? "hdr"s : "srgb"s;
          d.bitdepth = enable ? 10 : 8;
          touched.hdr = true;
        }
      } else {
        BOOST_LOG(info) << "Hyprland display device: "sv << target->name << " does not support HDR, leaving it alone"sv;
      }
    }

    // Drop entries that change nothing.
    std::erase_if(result.touched, [](const auto &entry) {
      return !entry.second.any();
    });
    return result;
  }

  plan_t plan_revert(const state_t &state, const layout_t &current, std::map<std::string, touched_t> &pending) {
    plan_t plan;
    for (const auto &[name, t] : state.touched) {
      const auto *s = state.find(name);
      if (!s) {
        continue;
      }
      if (!current.find(name)) {
        // A virtual display is gone with its session: nothing to restore. A monitor may come back (unplugged, or
        // switched off at the wall); keep it for the next attempt.
        if (!s->is_virtual) {
          pending[name] = t;
        }
        continue;
      }
      restore_fields(plan[name], *s, t);
    }
    return plan;
  }

  std::vector<monitor_rule_t> rules_for(const plan_t &plan, const layout_t &current, bool merges) {
    std::vector<monitor_rule_t> enables;
    std::vector<monitor_rule_t> changes;
    std::vector<monitor_rule_t> disables;
    for (const auto &[name, d] : plan) {
      const auto *now = current.find(name);
      if (!now) {
        continue;
      }
      monitor_rule_t rule;
      rule.output = name;
      const bool enabled_after = d.enabled.value_or(now->enabled);
      if (!enabled_after) {
        if (now->enabled) {
          rule.disabled = true;
          disables.push_back(std::move(rule));
        }
        continue;
      }
      const bool enabling = !now->enabled;
      bool changed = enabling;
      if (d.mode && (enabling || !same_mode(*d.mode, now->mode))) {
        rule.mode = mode_string(*d.mode);
        changed = true;
      }
      if (d.position && (enabling || d.position != std::pair {now->x, now->y})) {
        rule.position = position_string(d.position->first, d.position->second);
        changed = true;
      }
      if (d.cm && (*d.cm != now->cm || (d.bitdepth && d.bitdepth != now->bitdepth))) {
        rule.cm = d.cm;
        rule.bitdepth = d.bitdepth;
        changed = true;
      }
      if (!changed) {
        continue;
      }
      if (enabling) {
        rule.disabled = false;
      }
      if (!merges) {
        // A hyprlang rule replaces the previous one: carry everything over.
        if (!rule.mode) {
          rule.mode = mode_string(now->mode);
        }
        if (!rule.position) {
          rule.position = position_string(now->x, now->y);
        }
        rule.scale = now->scale;
        rule.transform = now->transform;
        if (!rule.bitdepth && now->bitdepth) {
          rule.bitdepth = now->bitdepth;
        }
        if (!rule.cm && !now->cm.empty()) {
          rule.cm = now->cm;
        }
      }
      (enabling ? enables : changes).push_back(std::move(rule));
    }
    // Enable before disabling anything, so the session never runs out of monitors in between.
    std::vector<monitor_rule_t> rules;
    rules.insert(rules.end(), enables.begin(), enables.end());
    rules.insert(rules.end(), changes.begin(), changes.end());
    rules.insert(rules.end(), disables.begin(), disables.end());
    return rules;
  }

  bool plan_satisfied(const plan_t &plan, const layout_t &current, std::string *failed, failure_e *kind) {
    const auto fail = [&](failure_e k, std::string what) {
      if (failed) {
        *failed = std::move(what);
      }
      if (kind) {
        *kind = k;
      }
      return false;
    };
    for (const auto &[name, d] : plan) {
      const auto *now = current.find(name);
      if (!now) {
        return fail(failure_e::topology, name + " is gone");
      }
      if (d.enabled && *d.enabled != now->enabled) {
        return fail(failure_e::topology, std::format("{} is still {}", name, now->enabled ? "enabled" : "disabled"));
      }
      if (!now->enabled) {
        continue;
      }
      if (d.mode && !same_mode(*d.mode, now->mode)) {
        return fail(failure_e::mode, std::format("{} runs {} instead of {}", name, mode_string(now->mode), mode_string(*d.mode)));
      }
      if (d.position && *d.position != std::pair {now->x, now->y}) {
        return fail(failure_e::position, std::format("{} is at {}x{} instead of {}x{}", name, now->x, now->y, d.position->first, d.position->second));
      }
      if (d.cm && is_hdr_preset(*d.cm) != is_hdr_preset(now->cm)) {
        return fail(failure_e::hdr, std::format("{} has colour preset {} instead of {}", name, now->cm, *d.cm));
      }
    }
    return true;
  }

  // settings_manager_t

  settings_manager_t::settings_manager_t(
    std::shared_ptr<compositor_t> compositor,
    std::shared_ptr<display_device::SettingsPersistenceInterface> persistence,
    options_t options
  ):
      compositor_ {std::move(compositor)},
      persistence_ {std::move(persistence)},
      options_ {std::move(options)} {
    if (!options_.sleep) {
      options_.sleep = [](std::chrono::milliseconds d) {
        std::this_thread::sleep_for(d);
      };
    }
    if (const auto data = persistence_->load(); data && !data->empty()) {
      const auto json = nlohmann::json::parse(data->begin(), data->end(), nullptr, false);
      state_ = state_from_json(json);
      if (state_) {
        BOOST_LOG(info) << "Hyprland display device: found settings to restore from a previous run"sv;
      } else {
        BOOST_LOG(error) << "Hyprland display device: the saved display state is not valid and is ignored"sv;
      }
    }
  }

  settings_manager_t::settings_manager_t(std::shared_ptr<compositor_t> compositor, std::shared_ptr<display_device::SettingsPersistenceInterface> persistence):
      settings_manager_t(std::move(compositor), std::move(persistence), options_t {}) {
  }

  const std::optional<state_t> &settings_manager_t::state() const {
    return state_;
  }

  bool settings_manager_t::save_state(const std::optional<state_t> &state) {
    bool ok;
    if (state) {
      const auto text = state_to_json(*state).dump(2);
      ok = persistence_->store(std::vector<std::uint8_t> {text.begin(), text.end()});
    } else {
      ok = persistence_->clear();
    }
    if (!ok) {
      BOOST_LOG(error) << "Hyprland display device: cannot save the display state"sv;
      return false;
    }
    state_ = state;
    return true;
  }

  display_device::EnumeratedDeviceList settings_manager_t::enumAvailableDevices() const {
    display_device::EnumeratedDeviceList devices;
    const auto current = compositor_->layout();
    if (!current) {
      return devices;
    }
    for (const auto &o : current->outputs) {
      display_device::EnumeratedDevice device;
      device.m_device_id = o.name;
      device.m_display_name = o.enabled ? o.name : "";
      device.m_friendly_name = o.description.empty() ? o.name : o.description;
      // Active means capturable: enabled and not switched off (hyprland::monitor_is_active()).
      if (o.enabled && o.dpms_on) {
        std::optional<display_device::HdrState> hdr;
        if (o.is_virtual) {
          if (auto on = compositor_->virtual_hdr(o.name)) {
            hdr = *on ? display_device::HdrState::Enabled : display_device::HdrState::Disabled;
          }
        } else if (o.hdr_capable) {
          hdr = is_hdr_preset(o.cm) ? display_device::HdrState::Enabled : display_device::HdrState::Disabled;
        }
        device.m_info = display_device::EnumeratedDevice::Info {
          {static_cast<unsigned int>(std::max(o.mode.width, 0)), static_cast<unsigned int>(std::max(o.mode.height, 0))},
          o.scale,
          o.mode.refresh_hz,
          o.x == 0 && o.y == 0,
          {o.x, o.y},
          hdr,
        };
      }
      devices.push_back(std::move(device));
    }
    return devices;
  }

  std::string settings_manager_t::getDisplayName(const std::string &device_id) const {
    const auto current = compositor_->layout();
    if (!current) {
      // Capture may still know the name; don't hide it.
      return device_id;
    }
    state_t lookup;
    for (const auto &o : current->outputs) {
      lookup.initial.push_back(save(o));
      if (o.focused) {
        lookup.focused = o.name;
      }
    }
    // An empty id means the primary display, which applySettings() picked from the layout it saved. Capture must
    // pick the same output: on its own it would take the first one Wayland lists.
    const auto &selection = device_id.empty() && state_ ? *state_ : lookup;
    const auto name = resolve_device(selection, *current, device_id);
    if (!name) {
      // Not an output Hyprland reports (e.g. the legacy display index capture also accepts): pass it on.
      return device_id;
    }
    const auto *out = current->find(*name);
    return out && out->enabled ? *name : std::string {};
  }

  std::optional<layout_t> settings_manager_t::wait_for(const plan_t &plan, std::string &failure, failure_e &kind) {
    const auto deadline = std::chrono::steady_clock::now() + options_.settle_timeout;
    while (true) {
      auto current = compositor_->layout();
      if (current && plan_satisfied(plan, *current, &failure, &kind)) {
        return current;
      }
      if (!current) {
        failure = "Hyprland stopped answering";
        kind = failure_e::unreachable;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return std::nullopt;
      }
      options_.sleep(options_.poll_interval);
    }
  }

  failure_e settings_manager_t::execute(const plan_t &plan, const layout_t &current, std::string &failure) {
    plan_t issued;
    for (const auto &rule : rules_for(plan, current, compositor_->merges_rules())) {
      if (!compositor_->set_monitor(rule)) {
        failure = "Hyprland rejected the monitor rule for " + rule.output;
        // Hyprland applies rules asynchronously. Let the ones already sent land, or the revert that follows would
        // compare against a layout that does not show them yet, send nothing, and they would land afterwards.
        if (!issued.empty()) {
          std::string ignored;
          failure_e ignored_kind;
          wait_for(issued, ignored, ignored_kind);
        }
        // A hyprlang rule carries the current mode and preset too: classify by what it changes.
        const auto *now = current.find(rule.output);
        const bool changes_cm = rule.cm && (!now || *rule.cm != now->cm);
        const bool changes_mode = rule.mode && (!now || *rule.mode != mode_string(now->mode));
        return rule.disabled ? failure_e::topology :
               changes_cm    ? failure_e::hdr :
               changes_mode  ? failure_e::mode :
                               failure_e::position;
      }
      issued[rule.output] = plan.at(rule.output);
    }
    for (const auto &[name, d] : plan) {
      if (d.virtual_hdr && current.find(name)) {
        const auto now = compositor_->virtual_hdr(name);
        if ((!now || *now != *d.virtual_hdr) && !compositor_->set_virtual_hdr(name, *d.virtual_hdr)) {
          failure = std::format("cannot switch HDR {} on {}", *d.virtual_hdr ? "on" : "off", name);
          return failure_e::hdr;
        }
      }
    }
    failure_e kind = failure_e::none;
    return wait_for(plan, failure, kind) ? failure_e::none : kind;
  }

  ApplyResult settings_manager_t::applySettings(const display_device::SingleDisplayConfiguration &config) {
    BOOST_LOG(info) << "Hyprland display device: applying "sv << describe(config);
    const auto current = compositor_->layout();
    if (!current) {
      BOOST_LOG(warning) << "Hyprland display device: Hyprland is not reachable"sv;
      return ApplyResult::ApiTemporarilyUnavailable;
    }

    const bool first = !state_;
    state_t state = state_.value_or(state_t {});
    if (first) {
      state.workspaces = current->workspaces;
      for (const auto &o : current->outputs) {
        if (o.focused) {
          state.focused = o.name;
        }
      }
    }
    // Outputs that appeared since the first apply (e.g. this session's virtual display) start out as they are now.
    for (const auto &o : current->outputs) {
      if (!state.find(o.name)) {
        state.initial.push_back(save(o));
      }
    }

    // The virtual display's HDR state is read through platf::vdisplay before it is changed.
    if (config.m_hdr_state) {
      if (const auto target = resolve_device(state, *current, config.m_device_id)) {
        auto it = std::ranges::find(state.initial, *target, &saved_output_t::name);
        if (it != state.initial.end() && it->is_virtual && !it->virtual_hdr && !state.touched[*target].hdr) {
          it->virtual_hdr = compositor_->virtual_hdr(*target);
        }
      }
    }

    auto planned = plan_apply(state, *current, config);
    if (const auto *failed = std::get_if<ApplyResult>(&planned)) {
      return *failed;
    }
    auto &plan = std::get<apply_plan_t>(planned);
    const auto *target_before = current->find(plan.target);

    state.touched = plan.touched;
    state.focus_changed = state.focus_changed || plan.focus_target;
    // Save before changing anything: if Apollo dies half-way, the next start still restores the layout.
    const bool nothing_to_restore = state.touched.empty() && !state.focus_changed;
    if (!nothing_to_restore && !save_state(state)) {
      return ApplyResult::PersistenceSaveFailed;
    }

    std::string failure;
    if (const auto failed = execute(plan.plan, *current, failure); failed != failure_e::none) {
      BOOST_LOG(error) << "Hyprland display device: applying the settings failed: "sv << failure << "; reverting"sv;
      std::ignore = revert(false);
      switch (failed) {
        case failure_e::unreachable:
          return ApplyResult::ApiTemporarilyUnavailable;
        case failure_e::mode:
          return ApplyResult::DisplayModePrepFailed;
        case failure_e::hdr:
          return ApplyResult::HdrStatePrepFailed;
        case failure_e::position:
          return ApplyResult::PrimaryDevicePrepFailed;
        default:
          return ApplyResult::DevicePrepFailed;
      }
    }
    if (nothing_to_restore && state_ && !save_state(std::nullopt)) {
      BOOST_LOG(warning) << "Hyprland display device: cannot clear the saved display state"sv;
    }

    // Disabling monitors moves their workspaces and can move focus. The target keeps the workspace it showed (the
    // virtual display's own workspace, see vdisplay_t::activate()) and gets focus back.
    if (auto after = compositor_->layout()) {
      const auto *target_after = after->find(plan.target);
      if (target_before && target_after && target_after->enabled && !target_before->active_workspace.empty() &&
          target_after->active_workspace != target_before->active_workspace && after->find_workspace(target_before->active_workspace)) {
        compositor_->show_workspace(plan.target, target_before->active_workspace);
      }
      if (target_after && target_after->enabled && !target_after->focused && (plan.focus_target || (target_before && target_before->focused))) {
        compositor_->focus_monitor(plan.target);
      }
    }

    BOOST_LOG(info) << "Hyprland display device: settings applied to "sv << plan.target;
    return ApplyResult::Ok;
  }

  RevertResult settings_manager_t::revert(bool log_nothing_to_do) {
    if (!state_) {
      if (log_nothing_to_do) {
        BOOST_LOG(debug) << "Hyprland display device: nothing to revert"sv;
      }
      return RevertResult::Ok;
    }
    const auto current = compositor_->layout();
    if (!current) {
      BOOST_LOG(warning) << "Hyprland display device: Hyprland is not reachable, cannot revert yet"sv;
      return RevertResult::ApiTemporarilyUnavailable;
    }
    const state_t state = *state_;
    BOOST_LOG(info) << "Hyprland display device: reverting the display settings"sv;

    std::map<std::string, touched_t> pending;
    const auto plan = plan_revert(state, *current, pending);
    std::string failure;
    if (const auto failed = execute(plan, *current, failure); failed != failure_e::none) {
      BOOST_LOG(error) << "Hyprland display device: reverting failed: "sv << failure;
      return revert_result(failed);
    }

    auto after = compositor_->layout();
    if (!after) {
      return RevertResult::ApiTemporarilyUnavailable;
    }

    // Outputs positioned `auto` (or overlapping) may have moved while modes and monitors changed: put every originally
    // enabled monitor back where it was, once the modes are back.
    const bool layout_changed = std::ranges::any_of(state.touched, [](const auto &entry) {
      return entry.second.enabled || entry.second.mode || entry.second.position;
    });
    if (layout_changed) {
      plan_t positions;
      for (const auto &s : state.initial) {
        const auto *now = after->find(s.name);
        if (s.enabled && !s.is_virtual && now && now->enabled && (now->x != s.x || now->y != s.y)) {
          positions[s.name].position = std::pair {s.x, s.y};
        }
      }
      if (!positions.empty()) {
        // Keep the saved state on failure: the layout is not back yet.
        if (const auto failed = execute(positions, *after, failure); failed != failure_e::none) {
          BOOST_LOG(error) << "Hyprland display device: cannot restore the monitor positions: "sv << failure;
          return revert_result(failed);
        }
      }
      if (auto again = compositor_->layout()) {
        after = std::move(again);
      }
    }

    // Workspaces of monitors that were disabled return to them (Hyprland usually does this itself), and each shows
    // the workspace it showed before. A command that fails keeps the saved state, so the retry finishes the job; it
    // only sends what is still out of place.
    const auto dispatch_failed = [](std::string_view what) {
      BOOST_LOG(warning) << "Hyprland display device: cannot "sv << what << "; will retry"sv;
      return RevertResult::ApiTemporarilyUnavailable;
    };
    for (const auto &[name, t] : state.touched) {
      const auto *s = state.find(name);
      const auto *now = after->find(name);
      if (!t.enabled || !s || !s->enabled || !now || !now->enabled) {
        continue;
      }
      // Moving a workspace to the monitor also shows it there.
      bool moved = false;
      for (const auto &w : state.workspaces) {
        const auto *ws = after->find_workspace(w.name);
        if (w.monitor == name && w.name != s->active_workspace && ws && ws->monitor != name) {
          if (!compositor_->show_workspace(name, w.name)) {
            return dispatch_failed(std::format("move workspace {} back to {}", w.name, name));
          }
          moved = true;
        }
      }
      if (!s->active_workspace.empty() && after->find_workspace(s->active_workspace) &&
          (moved || now->active_workspace != s->active_workspace || after->find_workspace(s->active_workspace)->monitor != name)) {
        if (!compositor_->show_workspace(name, s->active_workspace)) {
          return dispatch_failed(std::format("show workspace {} on {}", s->active_workspace, name));
        }
      }
    }
    if (state.focus_changed && !state.focused.empty()) {
      const auto *f = after->find(state.focused);
      if (f && f->enabled && !f->focused && !compositor_->focus_monitor(state.focused)) {
        return dispatch_failed("focus " + state.focused);
      }
    }

    if (!pending.empty()) {
      state_t rest = state;
      rest.touched = std::move(pending);
      save_state(rest);
      std::string names;
      for (const auto &[name, _] : rest.touched) {
        names += (names.empty() ? "" : ", ") + name;
      }
      BOOST_LOG(warning) << "Hyprland display device: cannot restore "sv << names << " yet: not connected"sv;
      return RevertResult::TopologyIsInvalid;
    }
    if (!save_state(std::nullopt)) {
      return RevertResult::PersistenceSaveFailed;
    }
    BOOST_LOG(info) << "Hyprland display device: display settings reverted"sv;
    return RevertResult::Ok;
  }

  RevertResult settings_manager_t::revertSettings() {
    return revert(true);
  }

  bool settings_manager_t::resetPersistence() {
    BOOST_LOG(info) << "Hyprland display device: forgetting the saved display settings"sv;
    if (!state_ && persistence_->load().value_or(std::vector<std::uint8_t> {}).empty()) {
      return true;
    }
    return save_state(std::nullopt);
  }

  std::unique_ptr<SettingsManagerInterface> make_settings_manager(const std::filesystem::path &persistence_filepath) {
    auto persistence = std::make_shared<display_device::FileSettingsPersistence>(persistence_filepath);
    std::error_code ec;
    const bool has_state = !persistence_filepath.empty() && std::filesystem::exists(persistence_filepath, ec);
    const bool in_hyprland = std::getenv("HYPRLAND_INSTANCE_SIGNATURE") != nullptr || find_instance_signature().has_value();
    if (!has_state && !in_hyprland) {
      return nullptr;
    }
    return std::make_unique<settings_manager_t>(
      std::make_shared<hyprland_compositor_t>(hyprland_compositor_t::options_t {}),
      std::move(persistence)
    );
  }
}  // namespace hyprland::dd
