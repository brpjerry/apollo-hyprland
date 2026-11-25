/**
 * @file src/platform/linux/hyprland/vdisplay.cpp
 * @brief Definitions for the Hyprland headless-output virtual display backend, and platf::vdisplay on Linux.
 *
 * Behaviour verified against Hyprland 0.57.2 (Lua config):
 * - `output create headless NAME` adds an output at 1920x1080@60 placed by `auto` to the right of the layout, and
 *   emits `monitoraddedv2>>ID,NAME,`. `output remove NAME` emits `monitorremovedv2>>ID,NAME,`.
 * - A mode such as `2560x1600@119.880` is accepted for headless outputs as is; `j/monitors` reports
 *   `refreshRate: 119.88` and the new size, typically before the next request is answered. A mode within about
 *   1 Hz of one the output lists is snapped to it: a headless output lists only 1920x1080@60, so
 *   `1920x1080@59.940` ends up at 60 Hz. Rules are still
 *   applied asynchronously (on the next monitor reload), so the backend polls until the mode shows up.
 * - `hl.monitor({...})` merges into an existing rule for the same output. Rules cannot be deleted short of a config
 *   reload, so the rule for a removed `APOLLO-xxxxxxxx` output stays behind. That is harmless: it only applies to
 *   an output of that exact name, which only Apollo creates, and the next session for that identity overwrites it.
 *   (HYP-12 may turn this into a feature by persisting per-identity settings.)
 * - A config reload (`hyprctl reload`, or saving the config) rebuilds the rules from the config file and drops the
 *   runtime rule, so the output would fall back to 1920x1080@60. The backend re-applies its rules on
 *   `configreloaded`.
 */
// this include
#include "vdisplay.h"

// standard includes
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>

// platform includes
#include <signal.h>
#include <unistd.h>

// local includes
#include "src/logging.h"
#include "src/platform/linux/wayland.h"

using namespace std::literals;

namespace hyprland {
  namespace {
    using platf::vdisplay::mode_t;
    using platf::vdisplay::status_e;
    using clock_t_ = std::chrono::steady_clock;

    constexpr std::string_view name_prefix = "APOLLO-"sv;
    constexpr std::string_view default_position = "auto-right"sv;
    // Hyprland snaps a requested mode to one in the output's mode list when the refresh rate is within about
    // 1 Hz: a headless output lists 1920x1080@60, so `1920x1080@59.940` gives 60 Hz while `2560x1440@59.940`
    // gives 59.94 Hz.
    constexpr double refresh_tolerance_hz = 1.0;

    bool is_lower_hex(char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    }

    /**
     * @brief Parse a numeric position `XxY` (as produced by isolated_position()).
     */
    std::optional<std::pair<int, int>> parse_position(const std::optional<std::string> &position) {
      if (!position) {
        return std::nullopt;
      }
      int x = 0;
      int y = 0;
      char sep = 0;
      char extra = 0;
      if (std::sscanf(position->c_str(), "%d%c%d%c", &x, &sep, &y, &extra) != 3 || sep != 'x') {
        return std::nullopt;
      }
      return std::pair {x, y};
    }

    bool same_rule(const monitor_rule_t &a, const monitor_rule_t &b) {
      return a.output == b.output && a.mode == b.mode && a.position == b.position && a.scale == b.scale &&
             a.transform == b.transform && a.bitdepth == b.bitdepth && a.disabled == b.disabled;
    }

    std::string describe(const monitor_info_t &m) {
      return std::format("{}x{}@{:.3f} at {},{} scale {}", m.width, m.height, m.refresh_hz, m.x, m.y, m.scale);
    }

    bool process_alive(int pid) {
      if (pid <= 0) {
        return false;
      }
      return ::kill(pid, 0) == 0 || errno == EPERM;
    }
  }  // namespace

  std::string vdisplay_output_name(const uuid_util::uuid_t &identity) {
    // uuid_t::parse() reads the first group of the text form into b32[0] as a number, so formatting that number
    // gives back the first 8 hex digits as the user sees them (client UUID in the web UI, app UUID in apps.json).
    // uuid_t::string() is not the inverse of parse(): it prints the bytes in memory order, which swaps that group.
    return std::format("{}{:08x}", name_prefix, identity.b32[0]);
  }

  bool is_vdisplay_output_name(std::string_view name) {
    if (!name.starts_with(name_prefix)) {
      return false;
    }
    auto rest = name.substr(name_prefix.size());
    return rest.size() == 8 && std::ranges::all_of(rest, is_lower_hex);
  }

  std::string vdisplay_mode_string(const mode_t &mode) {
    const int millihz = mode.refresh_millihz > 0 ? mode.refresh_millihz : 60000;
    return std::format("{}x{}@{}.{:03}", mode.width, mode.height, millihz / 1000, millihz % 1000);
  }

  bool monitor_has_mode(const monitor_info_t &monitor, const mode_t &mode) {
    const int millihz = mode.refresh_millihz > 0 ? mode.refresh_millihz : 60000;
    return monitor.width == mode.width && monitor.height == mode.height &&
           std::abs(monitor.refresh_hz - millihz / 1000.0) <= refresh_tolerance_hz;
  }

  std::string isolated_position(const std::vector<monitor_info_t> &monitors, const std::string &own_name, int gap) {
    bool any = false;
    int max_x = 0;
    int max_y = 0;
    for (const auto &m : monitors) {
      if (m.name == own_name || m.disabled || m.width <= 0 || m.height <= 0) {
        continue;
      }
      const double scale = m.scale > 0 ? m.scale : 1.0;
      int w = static_cast<int>(std::lround(m.width / scale));
      int h = static_cast<int>(std::lround(m.height / scale));
      if (m.transform % 2 == 1) {
        std::swap(w, h);
      }
      const int right = m.x + w;
      const int bottom = m.y + h;
      max_x = any ? std::max(max_x, right) : right;
      max_y = any ? std::max(max_y, bottom) : bottom;
      any = true;
    }
    return std::format("{}x{}", max_x + gap, max_y + gap);
  }

  bool monitor_is_active(const monitor_info_t &monitor) {
    return !monitor.disabled && monitor.dpms_on && monitor.name != "FALLBACK"sv;
  }

  bool any_monitor_active(const std::vector<monitor_info_t> &monitors) {
    return std::ranges::any_of(monitors, monitor_is_active);
  }

  vdisplay_t::options_t vdisplay_t::default_options() {
    options_t options;
    options.connect = []() {
      return ipc_t::connect();
    };
    options.instance_exists = []() {
      return find_instance_signature().has_value();
    };
    options.output_visible = [](const std::string &name) -> std::optional<bool> {
      // Capture connects to $WAYLAND_DISPLAY; without it there is nothing to wait for.
      if (std::getenv("WAYLAND_DISPLAY") == nullptr) {
        return std::nullopt;
      }
      for (const auto &monitor : wl::monitors()) {
        if (monitor->name == name) {
          return true;
        }
      }
      return false;
    };
    options.state_dir = runtime_dir().parent_path() / "apollo-vdisplay";
    return options;
  }

  vdisplay_t::vdisplay_t(options_t options):
      options_ {std::move(options)} {
    reapply_thread_ = std::thread {[this]() {
      reapply_loop();
    }};
  }

  vdisplay_t::~vdisplay_t() {
    {
      std::lock_guard lock {events_mutex_};
      stopping_ = true;
    }
    events_cv_.notify_all();
    reapply_thread_.join();

    std::lock_guard lock {mutex_};
    // The listener's callback only takes events_mutex_, so stopping it here cannot deadlock.
    listener_.reset();
  }

  status_e vdisplay_t::init() {
    std::lock_guard lock {mutex_};
    if (!connect_locked()) {
      return status_;
    }
    if (!cleaned_up_) {
      cleaned_up_ = true;
      cleanup_stale_locked();
    }
    return status_;
  }

  status_e vdisplay_t::status() const {
    return status_;
  }

  void vdisplay_t::cleanup_stale() {
    std::lock_guard lock {mutex_};
    if (!ensure_connected_locked()) {
      return;
    }
    cleanup_stale_locked();
  }

  std::string vdisplay_t::create(const uuid_util::uuid_t &identity, const std::string &client_name, const mode_t &mode) {
    const auto name = vdisplay_output_name(identity);
    const monitor_rule_t rule {
      .output = name,
      .mode = vdisplay_mode_string(mode),
      .position = std::string {default_position},
      // HYP-12 will remember a per-identity scale; until then every session starts at 1.
      .scale = 1.0,
    };

    std::lock_guard lock {mutex_};
    if (!ensure_connected_locked()) {
      BOOST_LOG(error) << "Hyprland virtual display: not connected to Hyprland"sv;
      return {};
    }

    auto monitors = monitors_locked();
    if (!monitors) {
      BOOST_LOG(error) << "Hyprland virtual display: cannot list monitors"sv;
      return {};
    }
    const bool exists = std::ranges::any_of(*monitors, [&](const auto &m) {
      return m.name == name;
    });

    if (exists) {
      if (auto it = owned_.find(name); it != owned_.end()) {
        // Created by this process and still there (e.g. a second launch for the same identity): reuse it.
        BOOST_LOG(info) << "Hyprland virtual display: reusing "sv << name << " for "sv << client_name;
        it->second.identity = identity;
        if (!apply_rule_locked(it->second, rule, mode)) {
          BOOST_LOG(error) << "Hyprland virtual display: "sv << name << " did not take mode "sv << *rule.mode;
          return {};
        }
        return name;
      }

      // Left over from a crash, or created by someone else under our naming scheme: start from a fresh output.
      BOOST_LOG(info) << "Hyprland virtual display: removing leftover output "sv << name << " before re-creating it"sv;
      if (!remove_output_locked(name)) {
        BOOST_LOG(error) << "Hyprland virtual display: cannot remove leftover output "sv << name;
        return {};
      }
      remove_owner_file(name);
    }

    const auto seen = added_count(name);
    if (!ipc_->create_headless(name)) {
      BOOST_LOG(error) << "Hyprland virtual display: cannot create headless output "sv << name;
      return {};
    }
    auto &entry = owned_[name];
    entry.identity = identity;
    entry.rule = {};
    write_owner_file(name);

    const auto fail = [&](std::string_view why) -> std::string {
      BOOST_LOG(error) << "Hyprland virtual display: "sv << name << ' ' << why << "; removing it"sv;
      remove_output_locked(name);
      owned_.erase(name);
      remove_owner_file(name);
      return {};
    };

    if (!wait_for_output_event_locked(name, true, seen)) {
      return fail("did not appear"sv);
    }

    // Capture looks the output up in the Wayland registry by name, so wait until the wl_output global is there.
    const auto deadline = clock_t_::now() + options_.timeout;
    while (true) {
      const auto visible = options_.output_visible ? options_.output_visible(name) : std::nullopt;
      if (!visible) {
        BOOST_LOG(debug) << "Hyprland virtual display: no Wayland connection, not waiting for the wl_output of "sv << name;
        break;
      }
      if (*visible) {
        break;
      }
      if (clock_t_::now() >= deadline) {
        return fail("never showed up as a Wayland output"sv);
      }
      std::this_thread::sleep_for(options_.poll_interval);
    }

    if (!apply_rule_locked(entry, rule, mode)) {
      return fail(std::format("did not take mode {}", *rule.mode));
    }

    BOOST_LOG(info) << "Hyprland virtual display: created "sv << name << " at "sv << *rule.mode << " for "sv << client_name;
    return name;
  }

  bool vdisplay_t::set_mode(const std::string &name, const mode_t &mode, bool isolated) {
    std::lock_guard lock {mutex_};
    auto it = owned_.find(name);
    if (it == owned_.end()) {
      BOOST_LOG(warning) << "Hyprland virtual display: set_mode on "sv << name << ", which Apollo did not create"sv;
      return false;
    }
    if (!ensure_connected_locked()) {
      return false;
    }

    monitor_rule_t rule {
      .output = name,
      .mode = vdisplay_mode_string(mode),
      .position = std::string {default_position},
      .scale = it->second.rule.scale.value_or(1.0),
    };

    if (isolated) {
      // Windows (changeDisplaySettings2 / rearrangeVirtualDisplayForLowerRight) moves the virtual display to the
      // lower-right corner of the layout, touching the other displays only at a corner, and makes it primary, so
      // the cursor practically stays on it. The cheap Hyprland equivalent: put the output below and to the right
      // of everything else with a gap. Hyprland clamps pointer motion to the closest point on some monitor, so a
      // pointer on the virtual display cannot cross the gap to a physical monitor (or the other way round).
      // Hyprland re-places outputs whose position is `auto` whenever the layout changes, so they move to the right
      // of the isolated output while it exists (seen live: two auto-placed test outputs went from x=4000 to
      // x=10200 and came back after removal). They stay at y=0 and the isolated output starts below the lowest
      // bottom edge, so they still do not touch it.
      // HYP-11 should improve on this: focus the output and give it a workspace, optionally disable the physical
      // monitors for the session, and move it again when the physical layout changes during the session.
      auto monitors = monitors_locked();
      if (!monitors) {
        return false;
      }
      rule.position = isolated_position(*monitors, name, options_.isolation_gap);
      BOOST_LOG(info) << "Hyprland virtual display: isolating "sv << name << " at "sv << *rule.position;
    }

    if (same_rule(rule, it->second.rule)) {
      if (auto m = find_monitor_locked(name); m && monitor_has_mode(*m, mode)) {
        return true;
      }
    }
    return apply_rule_locked(it->second, rule, mode);
  }

  bool vdisplay_t::remove(const uuid_util::uuid_t &identity) {
    const auto name = vdisplay_output_name(identity);

    std::lock_guard lock {mutex_};
    if (!ensure_connected_locked()) {
      return false;
    }

    const bool present = find_monitor_locked(name).has_value();
    bool removed = false;
    if (present) {
      removed = remove_output_locked(name);
      if (removed) {
        BOOST_LOG(info) << "Hyprland virtual display: removed "sv << name;
      } else {
        BOOST_LOG(warning) << "Hyprland virtual display: removing "sv << name << " failed"sv;
      }
    }
    if (!present || removed) {
      owned_.erase(name);
      remove_owner_file(name);
    }
    return removed;
  }

  std::optional<bool> vdisplay_t::any_monitor_active() {
    std::lock_guard lock {mutex_};
    if (!ensure_connected_locked()) {
      return std::nullopt;
    }
    auto monitors = monitors_locked();
    if (!monitors) {
      return std::nullopt;
    }
    return hyprland::any_monitor_active(*monitors);
  }

  std::vector<std::string> vdisplay_t::owned_outputs() const {
    std::lock_guard lock {mutex_};
    std::vector<std::string> names;
    for (const auto &[name, _] : owned_) {
      names.push_back(name);
    }
    return names;
  }

  void vdisplay_t::on_event(std::string_view event, std::string_view data) {
    if (event == "monitoraddedv2"sv || event == "monitorremovedv2"sv) {
      auto parsed = parse_monitor_event(data);
      if (!parsed || !parsed->name.starts_with(name_prefix)) {
        return;
      }
      {
        std::lock_guard lock {events_mutex_};
        ++(event == "monitoraddedv2"sv ? added_ : removed_)[parsed->name];
      }
      events_cv_.notify_all();
    } else if (event == "configreloaded"sv) {
      {
        std::lock_guard lock {events_mutex_};
        reload_pending_ = true;
      }
      events_cv_.notify_all();
    }
  }

  void vdisplay_t::reapply_loop() {
    while (true) {
      {
        std::unique_lock lock {events_mutex_};
        events_cv_.wait(lock, [this]() {
          return reload_pending_ || stopping_;
        });
        if (stopping_) {
          return;
        }
        reload_pending_ = false;
      }

      // events_mutex_ is released before taking mutex_: an operation holding mutex_ may be waiting on events.
      std::lock_guard lock {mutex_};
      if (!ipc_) {
        continue;
      }
      for (auto &[name, entry] : owned_) {
        if (entry.rule.output.empty()) {
          continue;
        }
        BOOST_LOG(info) << "Hyprland virtual display: config reloaded, re-applying "sv << name << " at "sv << entry.rule.mode.value_or("?"s);
        if (!ipc_->set_monitor(entry.rule)) {
          BOOST_LOG(warning) << "Hyprland virtual display: re-applying the rule for "sv << name << " failed"sv;
        }
      }
    }
  }

  bool vdisplay_t::ensure_connected_locked() {
    if (ipc_ && status_ == status_e::ok) {
      return true;
    }
    return connect_locked();
  }

  bool vdisplay_t::connect_locked() {
    auto ipc = options_.connect ? options_.connect() : nullptr;
    if (!ipc) {
      const bool exists = options_.instance_exists && options_.instance_exists();
      status_ = exists ? status_e::failed : status_e::unavailable;
      if (exists) {
        BOOST_LOG(warning) << "Hyprland virtual display: a Hyprland instance exists but does not answer"sv;
      } else {
        BOOST_LOG(debug) << "Hyprland virtual display: not running under Hyprland"sv;
      }
      return false;
    }

    // Headless outputs (`output create headless`) and monitoraddedv2 have been around far longer than the
    // `j/status` request ipc_t relies on, so any version that answers here supports them.
    auto version = ipc->request_json("version"sv);
    if (!version || !version->is_object()) {
      BOOST_LOG(warning) << "Hyprland virtual display: Hyprland did not answer the version request"sv;
      status_ = status_e::failed;
      return false;
    }
    BOOST_LOG(info) << "Hyprland virtual display: using Hyprland "sv << version->value("tag", "(unknown version)"s)
                    << " (instance "sv << ipc->signature() << ')';

    listener_.reset();
    listener_ = std::make_unique<event_listener_t>(ipc->event_socket_path());
    listener_->subscribe([this](std::string_view event, std::string_view data) {
      on_event(event, data);
    });
    if (!listener_->start()) {
      BOOST_LOG(warning) << "Hyprland virtual display: cannot connect to the event socket yet; polling monitors instead"sv;
    }

    ipc_ = std::move(ipc);
    status_ = status_e::ok;
    return true;
  }

  std::optional<std::vector<monitor_info_t>> vdisplay_t::monitors_locked() {
    for (int attempt = 0; attempt < 2; ++attempt) {
      if (ipc_) {
        if (auto json = ipc_->request_json("monitors all"sv)) {
          return parse_monitors(*json);
        }
      }
      // The instance may have gone away (e.g. Hyprland restarted with a new signature): reconnect once.
      if (attempt == 0 && !connect_locked()) {
        break;
      }
    }
    return std::nullopt;
  }

  std::optional<monitor_info_t> vdisplay_t::find_monitor_locked(const std::string &name) {
    auto monitors = monitors_locked();
    if (!monitors) {
      return std::nullopt;
    }
    for (auto &m : *monitors) {
      if (m.name == name) {
        return m;
      }
    }
    return std::nullopt;
  }

  bool vdisplay_t::apply_rule_locked(owned_t &entry, monitor_rule_t rule, const mode_t &mode) {
    const auto position = parse_position(rule.position);
    // Remember the rule before applying it, so a config reload in between re-applies the new one.
    entry.rule = rule;
    if (!ipc_->set_monitor(rule)) {
      return false;
    }
    return wait_for_mode_locked(rule.output, mode, position);
  }

  bool vdisplay_t::wait_for_mode_locked(const std::string &name, const mode_t &mode, const std::optional<std::pair<int, int>> &position) {
    const auto deadline = clock_t_::now() + options_.timeout;
    std::optional<monitor_info_t> last;
    while (true) {
      last = find_monitor_locked(name);
      if (last && monitor_has_mode(*last, mode) && (!position || (last->x == position->first && last->y == position->second))) {
        return true;
      }
      if (clock_t_::now() >= deadline) {
        BOOST_LOG(warning) << "Hyprland virtual display: "sv << name << " wanted "sv << vdisplay_mode_string(mode)
                           << (position ? std::format(" at {},{}", position->first, position->second) : ""s)
                           << ", has "sv << (last ? describe(*last) : "no monitor"s);
        return false;
      }
      std::this_thread::sleep_for(options_.poll_interval);
    }
  }

  bool vdisplay_t::remove_output_locked(const std::string &name) {
    const auto seen = removed_count(name);
    if (!ipc_->remove_output(name)) {
      return false;
    }
    if (!wait_for_output_event_locked(name, false, seen)) {
      BOOST_LOG(warning) << "Hyprland virtual display: no monitorremovedv2 for "sv << name;
      return !find_monitor_locked(name).has_value();
    }
    return true;
  }

  void vdisplay_t::cleanup_stale_locked() {
    auto monitors = monitors_locked();
    if (!monitors) {
      return;
    }
    for (const auto &m : *monitors) {
      if (!is_vdisplay_output_name(m.name) || owned_.contains(m.name)) {
        continue;
      }
      if (auto pid = read_owner_pid(m.name); pid && *pid != ::getpid() && process_alive(*pid)) {
        BOOST_LOG(info) << "Hyprland virtual display: leaving "sv << m.name << " alone, it belongs to running process "sv << *pid;
        continue;
      }
      BOOST_LOG(info) << "Hyprland virtual display: removing stale output "sv << m.name << " left by a previous run"sv;
      if (remove_output_locked(m.name)) {
        remove_owner_file(m.name);
      }
    }
  }

  std::uint64_t vdisplay_t::added_count(const std::string &name) {
    std::lock_guard lock {events_mutex_};
    auto it = added_.find(name);
    return it == added_.end() ? 0 : it->second;
  }

  std::uint64_t vdisplay_t::removed_count(const std::string &name) {
    std::lock_guard lock {events_mutex_};
    auto it = removed_.find(name);
    return it == removed_.end() ? 0 : it->second;
  }

  bool vdisplay_t::wait_for_output_event_locked(const std::string &name, bool added, std::uint64_t seen) {
    const auto deadline = clock_t_::now() + options_.timeout;
    while (true) {
      {
        std::unique_lock lock {events_mutex_};
        const auto &counts = added ? added_ : removed_;
        const bool got = events_cv_.wait_for(lock, options_.poll_interval * 4, [&]() {
          auto it = counts.find(name);
          return it != counts.end() && it->second > seen;
        });
        if (got) {
          return true;
        }
      }
      // Without the event socket, fall back to polling the monitor list.
      if (!listener_ || !listener_->connected()) {
        if (find_monitor_locked(name).has_value() == added) {
          return true;
        }
      }
      if (clock_t_::now() >= deadline) {
        return false;
      }
    }
  }

  void vdisplay_t::write_owner_file(const std::string &name) {
    if (options_.state_dir.empty()) {
      return;
    }
    std::error_code ec;
    std::filesystem::create_directories(options_.state_dir, ec);
    std::ofstream out {options_.state_dir / name, std::ios::trunc};
    out << ::getpid() << '\n';
    if (!out) {
      BOOST_LOG(debug) << "Hyprland virtual display: cannot write "sv << (options_.state_dir / name).string();
    }
  }

  void vdisplay_t::remove_owner_file(const std::string &name) {
    if (options_.state_dir.empty()) {
      return;
    }
    std::error_code ec;
    std::filesystem::remove(options_.state_dir / name, ec);
  }

  std::optional<int> vdisplay_t::read_owner_pid(const std::string &name) {
    if (options_.state_dir.empty()) {
      return std::nullopt;
    }
    std::ifstream in {options_.state_dir / name};
    int pid = 0;
    if (!(in >> pid)) {
      return std::nullopt;
    }
    return pid;
  }
}  // namespace hyprland

namespace platf::vdisplay {
  namespace {
    hyprland::vdisplay_t &backend() {
      // Never destroyed: its threads may still be running while static objects are torn down at exit.
      static auto *instance = new hyprland::vdisplay_t {hyprland::vdisplay_t::default_options()};
      return *instance;
    }
  }  // namespace

  status_e init() {
    return backend().init();
  }

  status_e status() {
    return backend().status();
  }

  void cleanup_stale() {
    if (backend().status() == status_e::ok) {
      backend().cleanup_stale();
    }
  }

  std::string create(const uuid_util::uuid_t &identity, const std::string &client_name, const mode_t &mode, const std::string &) {
    return backend().create(identity, client_name, mode);
  }

  bool set_mode(const std::string &display_name, const mode_t &mode, bool isolated) {
    return backend().set_mode(display_name, mode, isolated);
  }

  bool remove(const uuid_util::uuid_t &identity) {
    return backend().remove(identity);
  }

  std::optional<bool> get_hdr(const std::string &) {
    // HDR on virtual displays is HYP-17. Returning nullopt also keeps process.cpp from running its HDR off/on
    // toggle on a display that cannot do HDR yet.
    return std::nullopt;
  }

  bool set_hdr(const std::string &, bool) {
    // See get_hdr(): HYP-17.
    return false;
  }

  void set_render_adapter(const std::string &) {
    // Hyprland renders headless outputs on its own GPU; there is nothing to choose.
  }

  std::string output_name_for(const std::string &display_name) {
    // Wayland capture selects outputs by connector name (HYP-04).
    return display_name;
  }

  std::optional<bool> any_display_active() {
    if (backend().status() != status_e::ok) {
      return std::nullopt;
    }
    return backend().any_monitor_active();
  }
}  // namespace platf::vdisplay
