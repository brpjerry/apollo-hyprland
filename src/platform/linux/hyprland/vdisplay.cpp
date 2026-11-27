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
 *   HYP-12 builds on this: settings the user changes are remembered per output name (display_settings.h).
 * - A config reload (`hyprctl reload`, or saving the config) rebuilds the rules from the config file and drops the
 *   runtime rule, so the output would fall back to 1920x1080@60. The backend re-applies its rules on
 *   `configreloaded`.
 * - HDR (HYP-17): `cm = "hdr"` only takes on a headless output together with `supports_hdr = 1` (there is no EDID);
 *   `colorManagementPreset` in `j/monitors` then reads `hdr`, and wp-color-management describes the output as PQ /
 *   BT.2020 with the luminances of the rule (80 cd/m² max without them). `bitdepth = 10` gives an XBGR2101010
 *   output on the DRM-backed desktop session (and stays XRGB8888 in the test harness's render-node-only instance).
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
#include <cstring>
#include <format>
#include <fstream>

// platform includes
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <unistd.h>

// local includes
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/platform/linux/wayland.h"

using namespace std::literals;

namespace hyprland {
  namespace {
    using platf::vdisplay::mode_t;
    using platf::vdisplay::status_e;
    using clock_t_ = std::chrono::steady_clock;

    constexpr std::string_view name_prefix = "APOLLO-"sv;
    // Hyprland snaps a requested mode to one in the output's mode list when the refresh rate is within about
    // 1 Hz: a headless output lists 1920x1080@60, so `1920x1080@59.940` gives 60 Hz while `2560x1440@59.940`
    // gives 59.94 Hz.
    constexpr double refresh_tolerance_hz = 1.0;
    // Hyprland rounds a scale to one that divides the mode into whole logical pixels (1.5 on 1280x800 gives 1.6).
    constexpr double scale_tolerance = 0.25;

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
      return a == b;
    }

    std::string describe(const monitor_info_t &m) {
      return std::format("{}x{}@{:.3f} at {},{} scale {}", m.width, m.height, m.refresh_hz, m.x, m.y, m.scale);
    }

    /**
     * @brief The start time of a process (clock ticks after boot), or nullopt if it cannot be read.
     */
    std::optional<std::uint64_t> process_start_time(int pid) {
      auto stat = read_proc_stat(pid);
      if (!stat || stat->start_time == 0) {
        return std::nullopt;
      }
      return stat->start_time;
    }

    /**
     * @brief An exclusive flock() on a file, released on destruction. An empty path holds nothing and counts as
     *        acquired (no state directory: nothing to coordinate).
     */
    class file_lock_t {
    public:
      explicit file_lock_t(const std::filesystem::path &path):
          fd_ {path.empty() ? -1 : ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600)},
          acquired_ {path.empty()} {
        if (fd_ >= 0) {
          int rc;
          do {
            rc = ::flock(fd_, LOCK_EX);
          } while (rc != 0 && errno == EINTR);
          acquired_ = rc == 0;
        }
        if (!acquired_) {
          BOOST_LOG(error) << "Hyprland virtual display: cannot lock "sv << path.string() << ": "sv << std::strerror(errno);
        }
      }

      /**
       * @brief Whether the lock is held (or none was needed).
       */
      bool acquired() const {
        return acquired_;
      }

      ~file_lock_t() {
        if (fd_ >= 0) {
          ::close(fd_);
        }
      }

      file_lock_t(const file_lock_t &) = delete;
      file_lock_t &operator=(const file_lock_t &) = delete;

    private:
      int fd_;
      bool acquired_;
    };
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

  void apply_hdr_fields(monitor_rule_t &rule, bool hdr) {
    if (hdr) {
      rule.cm = "hdr";
      rule.bitdepth = 10;
      rule.supports_hdr = 1;
      rule.supports_wide_color = 1;
      rule.max_luminance = 1000;
      rule.min_luminance = 0.005;
      rule.max_avg_luminance = 400;
    } else {
      // The bit depth is left to the caller: the remembered one, or 8 (see build_monitor_rule()).
      rule.cm = "srgb";
    }
  }

  bool monitor_is_hdr(const monitor_info_t &monitor) {
    return monitor.cm_preset == "hdr"sv || monitor.cm_preset == "hdredid"sv;
  }

  vdisplay_t::options_t vdisplay_t::default_options() {
    options_t options;
    options.connect = []() {
      return ipc_t::connect();
    };
    options.instance_exists = []() {
      // A running Hyprland whose control socket stays silent is still Hyprland. Report the backend as failed rather
      // than unavailable, so a launch is refused instead of capturing the physical desktop. A signature left in the
      // environment by a Hyprland session that has since exited does not count.
      return instance_running() || find_instance_signature().has_value();
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
    // Apollo's state directory: where sunshine_state.json lives (the config dir unless `file_state` moves it).
    auto state_parent = std::filesystem::path {config::nvhttp.file_state}.parent_path();
    options.settings_file = (state_parent.empty() ? platf::appdata() : state_parent) / "vdisplay_settings.json";
    options.read_stat = read_proc_stat;
    return options;
  }

  vdisplay_t::vdisplay_t(options_t options):
      options_ {std::move(options)},
      store_ {options_.settings_file} {
    if (!options_.read_stat) {
      options_.read_stat = read_proc_stat;
    }
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

  void vdisplay_t::cleanup_stale(bool only_if_pending) {
    std::lock_guard lock {mutex_};
    if (only_if_pending && std::ranges::none_of(owned_, [](const auto &entry) {
          return entry.second.ended;
        })) {
      return;
    }
    if (!ensure_connected_locked()) {
      return;
    }
    cleanup_stale_locked();
  }

  std::string vdisplay_t::create(const uuid_util::uuid_t &identity, const std::string &client_name, const mode_t &mode) {
    const auto name = vdisplay_output_name(identity);

    std::lock_guard lock {mutex_};
    if (!ensure_connected_locked()) {
      BOOST_LOG(error) << "Hyprland virtual display: not connected to Hyprland"sv;
      return {};
    }

    // A fresh session state for the entry: remembered settings, this session's scale factor, not isolated yet.
    const auto start_session = [&](owned_t &entry) {
      entry.identity = identity;
      entry.settings = store_.load(name);
      entry.settings.client_name = client_name;
      entry.scale_percent = mode.scale_percent > 0 ? mode.scale_percent : 100;
      entry.isolated = false;
      entry.ended = false;
      entry.baseline.reset();
      if (!entry.settings.empty()) {
        BOOST_LOG(info) << "Hyprland virtual display: remembered settings for "sv << name << ": "sv << settings_to_json(entry.settings).dump();
      }
    };

    auto monitors = monitors_locked();
    if (!monitors) {
      BOOST_LOG(error) << "Hyprland virtual display: cannot list monitors"sv;
      return {};
    }
    const bool exists = std::ranges::any_of(*monitors, [&](const auto &m) {
      return m.name == name;
    });

    if (auto it = owned_.find(name); exists && it != owned_.end()) {
      // Created by this process and still there (e.g. a second launch for the same identity): reuse it.
      BOOST_LOG(info) << "Hyprland virtual display: reusing "sv << name << " for "sv << client_name;
      start_session(it->second);
      const auto rule = rule_for_locked(name, it->second, mode, std::nullopt);
      if (!apply_rule_locked(it->second, rule, mode)) {
        BOOST_LOG(error) << "Hyprland virtual display: "sv << name << " did not take mode "sv << *rule.mode;
        return {};
      }
      return name;
    }

    // Claim the name before touching an output this process does not own, and keep the claim through removing a
    // leftover and creating the new one: another instance's stale cleanup takes an Apollo output without a live
    // owner for a leftover, and another instance replacing or creating the same output has claimed it first. Another
    // running Apollo's output (cleanup_stale_locked() leaves those alone too) is not ours to take over.
    if (!claim_owner_file(name)) {
      return {};
    }
    if (exists) {
      // Left over from a crash, or created under our naming scheme without an owner: start from a fresh output.
      BOOST_LOG(info) << "Hyprland virtual display: removing leftover output "sv << name << " before re-creating it"sv;
      if (!remove_output_locked(name)) {
        BOOST_LOG(error) << "Hyprland virtual display: cannot remove leftover output "sv << name;
        release_owner_file(name);
        return {};
      }
    }

    const auto seen = added_count(name);
    if (!ipc_->create_headless(name)) {
      BOOST_LOG(error) << "Hyprland virtual display: cannot create headless output "sv << name;
      release_owner_file(name);
      return {};
    }
    auto &entry = owned_[name];
    entry = {};
    start_session(entry);
    const auto rule = rule_for_locked(name, entry, mode, std::nullopt);

    const auto fail = [&](std::string_view why) -> std::string {
      BOOST_LOG(error) << "Hyprland virtual display: "sv << name << ' ' << why << "; removing it"sv;
      // If that fails too, keep it owned: the caller's remove() (terminate() after a failed launch) tries again.
      if (remove_output_locked(name)) {
        owned_.erase(name);
        release_owner_file(name);
      }
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

    std::optional<std::string> position;
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
      // activate() (HYP-11) then focuses the output, which also warps the pointer onto it. Disabling the physical
      // monitors for the session is display_device's job (HYP-18). Not done: moving the output again when the
      // physical layout changes during the session.
      auto monitors = monitors_locked();
      if (!monitors) {
        return false;
      }
      position = isolated_position(*monitors, name, options_.isolation_gap);
      BOOST_LOG(info) << "Hyprland virtual display: isolating "sv << name << " at "sv << *position;
    }
    it->second.isolated = isolated;
    const auto rule = rule_for_locked(name, it->second, mode, position);

    if (same_rule(rule, it->second.rule)) {
      if (auto m = find_monitor_locked(name); m && monitor_has_mode(*m, mode)) {
        return true;
      }
    }
    return apply_rule_locked(it->second, rule, mode);
  }

  std::optional<bool> vdisplay_t::get_hdr(const std::string &name) {
    std::lock_guard lock {mutex_};
    if (!owned_.contains(name) || !ensure_connected_locked()) {
      return std::nullopt;
    }
    auto monitor = find_monitor_locked(name);
    if (!monitor) {
      return std::nullopt;
    }
    return monitor_is_hdr(*monitor);
  }

  bool vdisplay_t::set_hdr(const std::string &name, bool enable) {
    std::lock_guard lock {mutex_};
    auto it = owned_.find(name);
    if (it == owned_.end()) {
      BOOST_LOG(warning) << "Hyprland virtual display: set_hdr on "sv << name << ", which Apollo did not create"sv;
      return false;
    }
    if (!ensure_connected_locked()) {
      return false;
    }

    // Through the output's rule, so the new state is part of the baseline: remove() must not learn HDR's bit depth
    // as a setting the user chose.
    auto &entry = it->second;
    // The rule below is built from the stored settings. Take in what the user changed during the session first
    // (terminate() restores the HDR state just before remove()), or re-applying it would undo those changes.
    if (const auto current = find_monitor_locked(name)) {
      learn_locked(name, entry, *current);
    }
    const auto previous = entry.hdr;
    entry.hdr = enable;
    const auto rule = rule_for_locked(name, entry, entry.mode, entry.isolated ? entry.rule.position : std::nullopt);
    if (!apply_rule_locked(entry, rule, entry.mode) || !wait_for_cm_locked(name, enable)) {
      entry.hdr = previous;
      return false;
    }
    // The preset may lag the mode the rule waited for; take the baseline again with it applied.
    entry.baseline = find_monitor_locked(name);
    BOOST_LOG(info) << "Hyprland virtual display: "sv << name << (enable ? " is in HDR mode"sv : " is in SDR mode"sv);
    return true;
  }

  bool vdisplay_t::remove(const uuid_util::uuid_t &identity) {
    const auto name = vdisplay_output_name(identity);

    std::lock_guard lock {mutex_};
    // Only what this backend created: after a failed create() the output may belong to another instance.
    const auto entry = owned_.find(name);
    if (entry == owned_.end()) {
      return false;
    }
    // The session is over whatever happens below: its app's new windows no longer go to the output, and an output
    // that cannot be removed now is removed by the next stale cleanup (the caller may not try again).
    entry->second.pgid = 0;
    entry->second.ended = true;
    if (!ensure_connected_locked()) {
      return false;
    }

    // A monitor list that cannot be read says nothing about the output: keep it owned and let the caller retry.
    const auto monitors = monitors_locked();
    if (!monitors) {
      BOOST_LOG(warning) << "Hyprland virtual display: cannot list monitors to remove "sv << name;
      return false;
    }
    const auto found = std::ranges::find(*monitors, name, &monitor_info_t::name);
    return finish_remove_locked(name, found == monitors->end() ? nullptr : &*found);
  }

  bool vdisplay_t::finish_remove_locked(const std::string &name, const monitor_info_t *current) {
    const auto entry = owned_.find(name);
    if (entry == owned_.end()) {
      return false;
    }
    std::optional<focus_t> restore_focus;
    if (current) {
      learn_locked(name, entry->second, *current);
      if (current->focused) {
        restore_focus = entry->second.previous_focus;
      }
    }

    bool removed = false;
    if (current) {
      removed = remove_output_locked(name);
      if (removed) {
        BOOST_LOG(info) << "Hyprland virtual display: removed "sv << name;
      } else {
        BOOST_LOG(warning) << "Hyprland virtual display: removing "sv << name << " failed"sv;
      }
    }
    if (!current || removed) {
      owned_.erase(name);
      release_owner_file(name);
    }
    // Hyprland moves focus to some remaining monitor; put it back where it was before the stream.
    if (removed && restore_focus) {
      restore_focus_locked(*restore_focus);
    }
    return removed;
  }

  bool vdisplay_t::activate(const std::string &name) {
    std::lock_guard lock {mutex_};
    auto it = owned_.find(name);
    if (it == owned_.end()) {
      BOOST_LOG(warning) << "Hyprland virtual display: activate on "sv << name << ", which Apollo did not create"sv;
      return false;
    }
    if (!ensure_connected_locked()) {
      return false;
    }
    auto monitors = monitors_locked();
    if (!monitors) {
      return false;
    }
    auto &entry = it->second;

    // Remember where focus was, unless it is already on this display (activated twice in one session).
    for (const auto &m : *monitors) {
      if (m.focused && m.name != name) {
        entry.previous_focus = focus_t {m.name, m.active_workspace};
      }
    }

    const auto workspace = entry.settings.workspace.value_or(default_workspace_name(name));
    bool ok = false;
    if (ipc_->dialect() == config_dialect_e::lua) {
      const auto reply = ipc_->eval(lua_show_workspace(name, workspace));
      ok = reply && *reply == "ok"sv;
      if (!ok) {
        BOOST_LOG(warning) << "Hyprland virtual display: cannot show workspace "sv << workspace << " on "sv << name << ": "sv << reply.value_or("error"s);
      }
      ok = focus_monitor_locked(name) && ok;
    } else {
      // focusworkspaceoncurrentmonitor creates the workspace, or pulls it over from another monitor.
      ok = focus_monitor_locked(name) && ipc_->dispatch_legacy("focusworkspaceoncurrentmonitor", workspace_selector(workspace));
    }
    entry.workspace = workspace;
    if (ok) {
      BOOST_LOG(info) << "Hyprland virtual display: focused "sv << name << " on workspace "sv << workspace
                      << (entry.previous_focus ? std::format(" (was {} on {})", entry.previous_focus->workspace, entry.previous_focus->monitor) : ""s);
    }
    return ok;
  }

  void vdisplay_t::track_process_group(const std::string &name, int pgid) {
    std::lock_guard lock {mutex_};
    auto it = owned_.find(name);
    if (it == owned_.end() || pgid <= 0) {
      return;
    }
    it->second.pgid = pgid;
    BOOST_LOG(debug) << "Hyprland virtual display: windows of process group "sv << pgid << " go to "sv << name;
    if (ensure_connected_locked()) {
      // Windows opened before tracking started (the app may be quick, or already running in this group).
      adopt_windows_locked({});
    }
  }

  monitor_rule_t vdisplay_t::rule_for_locked(const std::string &name, const owned_t &entry, const mode_t &mode, std::optional<std::string> position) {
    auto session_mode = mode;
    session_mode.scale_percent = entry.scale_percent;
    auto rule = build_monitor_rule({
      .output = name,
      .mode = session_mode,
      .settings = entry.settings,
      .isolated_position = std::move(position),
      .emitted = emitted_[name],
    });
    // HDR (set_hdr()) is part of every rule while it is on, so mode changes and config reloads keep it, and the
    // baseline that remove() learns user changes against includes it.
    if (entry.hdr) {
      apply_hdr_fields(rule, *entry.hdr);
    }
    return rule;
  }

  void vdisplay_t::learn_locked(const std::string &name, owned_t &entry, const monitor_info_t &now) {
    if (!entry.baseline) {
      return;
    }
    const auto learned = learn_settings(entry.settings, *entry.baseline, now, entry.isolated, entry.scale_percent, entry.workspace);
    if (learned == entry.settings) {
      return;
    }
    BOOST_LOG(info) << "Hyprland virtual display: remembering "sv << name << " settings "sv << settings_to_json(learned).dump()
                    << " (was "sv << settings_to_json(entry.settings).dump() << ')';
    if (!store_.save(name, learned)) {
      BOOST_LOG(warning) << "Hyprland virtual display: cannot save settings to "sv << store_.path().string();
    }
    // Rules built later in this session (set_hdr) must carry the user's changes rather than undo them.
    entry.settings = learned;
  }

  bool vdisplay_t::focus_monitor_locked(const std::string &monitor) {
    if (ipc_->dialect() == config_dialect_e::lua) {
      return ipc_->dispatch_lua(lua_focus_monitor(monitor));
    }
    return ipc_->dispatch_legacy("focusmonitor", monitor);
  }

  void vdisplay_t::restore_focus_locked(const focus_t &focus) {
    // Focusing the workspace also focuses its monitor (wherever it is now). If the workspace is gone, at least go
    // back to the monitor.
    bool workspace_exists = false;
    if (!focus.workspace.empty()) {
      if (auto json = ipc_->request_json("workspaces"sv); json && json->is_array()) {
        workspace_exists = std::ranges::any_of(*json, [&](const nlohmann::json &ws) {
          return ws.is_object() && ws.value("name", ""s) == focus.workspace;
        });
      }
    }
    bool ok;
    if (workspace_exists) {
      ok = ipc_->dialect() == config_dialect_e::lua ?
             ipc_->dispatch_lua(lua_focus_workspace(focus.workspace)) :
             ipc_->dispatch_legacy("workspace", workspace_selector(focus.workspace));
    } else {
      ok = focus_monitor_locked(focus.monitor);
    }
    if (ok) {
      BOOST_LOG(info) << "Hyprland virtual display: focus back on "sv << (workspace_exists ? "workspace "s + focus.workspace : focus.monitor);
    }
  }

  void vdisplay_t::adopt_windows_locked(const std::vector<std::string> &addresses) {
    const bool any_tracking = std::ranges::any_of(owned_, [](const auto &kv) {
      return kv.second.pgid > 0 && !kv.second.workspace.empty();
    });
    if (!any_tracking) {
      return;
    }
    auto clients_json = ipc_->request_json("clients"sv);
    auto monitors = monitors_locked();
    if (!clients_json || !monitors) {
      return;
    }
    for (const auto &client : parse_clients(*clients_json)) {
      if (!addresses.empty() && std::ranges::find(addresses, client.address) == addresses.end()) {
        continue;
      }
      for (const auto &[name, entry] : owned_) {
        if (entry.pgid <= 0 || entry.workspace.empty() || !process_belongs_to_group(client.pid, entry.pgid, options_.read_stat)) {
          continue;
        }
        const auto vd = std::ranges::find_if(*monitors, [&](const monitor_info_t &m) {
          return m.name == name;
        });
        if (vd == monitors->end() || client.monitor == vd->id) {
          break;  // already on the display (on any of its workspaces)
        }
        BOOST_LOG(info) << "Hyprland virtual display: moving window "sv << client.address << " (pid "sv << client.pid
                        << ") from workspace "sv << client.workspace << " to "sv << entry.workspace << " on "sv << name;
        if (ipc_->dialect() == config_dialect_e::lua) {
          ipc_->dispatch_lua(lua_move_window(client.address, entry.workspace));
        } else {
          ipc_->dispatch_legacy("movetoworkspacesilent", std::format("{},address:{}", workspace_selector(entry.workspace), client.address));
        }
        break;
      }
    }
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
    } else if (event == "openwindow"sv) {
      if (auto address = parse_openwindow_address(data)) {
        {
          std::lock_guard lock {events_mutex_};
          opened_windows_.push_back(std::move(*address));
        }
        events_cv_.notify_all();
      }
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
      bool reload = false;
      std::vector<std::string> windows;
      {
        std::unique_lock lock {events_mutex_};
        events_cv_.wait(lock, [this]() {
          return reload_pending_ || !opened_windows_.empty() || stopping_;
        });
        if (stopping_) {
          return;
        }
        reload = std::exchange(reload_pending_, false);
        windows.assign(opened_windows_.begin(), opened_windows_.end());
        opened_windows_.clear();
      }

      // events_mutex_ is released before taking mutex_: an operation holding mutex_ may be waiting on events.
      std::lock_guard lock {mutex_};
      if (!ipc_) {
        continue;
      }
      if (!windows.empty()) {
        adopt_windows_locked(windows);
      }
      if (!reload) {
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
        BOOST_LOG(warning) << "Hyprland virtual display: a Hyprland instance was detected, but its control socket did not answer"sv;
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
        if (auto json = ipc_->request_json("monitors all"sv); json && json->is_array()) {
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

  std::optional<bool> vdisplay_t::monitor_listed_locked(const std::string &name) {
    const auto monitors = monitors_locked();
    if (!monitors) {
      return std::nullopt;
    }
    return std::ranges::find(*monitors, name, &monitor_info_t::name) != monitors->end();
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
    // Only an isolated position is waited for: Hyprland may adjust a remembered one, which is no reason to fail.
    const auto position = entry.isolated ? parse_position(rule.position) : std::nullopt;
    // Remember the rule before applying it, so a config reload in between re-applies the new one.
    entry.rule = rule;
    entry.mode = mode;
    if (!ipc_->set_monitor(rule)) {
      return false;
    }
    auto &emitted = emitted_[rule.output];
    emitted.transform = emitted.transform || rule.transform.has_value();
    emitted.bitdepth = emitted.bitdepth || rule.bitdepth.has_value();
    if (!wait_for_mode_locked(rule, mode, position)) {
      return false;
    }
    // What the output looks like with Apollo's rule (and the user's static rules) applied: the user's changes during
    // the session are the difference to this.
    entry.baseline = find_monitor_locked(rule.output);
    return true;
  }

  bool vdisplay_t::wait_for_mode_locked(const monitor_rule_t &rule, const mode_t &mode, const std::optional<std::pair<int, int>> &position) {
    const auto &name = rule.output;
    const auto deadline = clock_t_::now() + options_.timeout;
    std::optional<monitor_info_t> last;
    while (true) {
      last = find_monitor_locked(name);
      const bool core = last && monitor_has_mode(*last, mode) && (!position || (last->x == position->first && last->y == position->second));
      // The mode may already be right (a re-created output starts with the last rule's mode) while the new scale is
      // still pending, so wait for the scale too. Hyprland snaps scales it cannot represent (1.5 on 1280x800 is 1.6),
      // hence the tolerance.
      const bool extras = last && (!rule.scale || std::abs(last->scale - *rule.scale) <= scale_tolerance) &&
                          (!rule.transform || last->transform == *rule.transform);
      if (core && extras) {
        return true;
      }
      if (clock_t_::now() >= deadline) {
        if (core) {
          BOOST_LOG(warning) << "Hyprland virtual display: "sv << name << " has scale "sv << last->scale << " and transform "sv
                             << last->transform << " instead of "sv << rule.scale.value_or(last->scale) << " and "sv
                             << rule.transform.value_or(last->transform);
          return true;
        }
        BOOST_LOG(warning) << "Hyprland virtual display: "sv << name << " wanted "sv << vdisplay_mode_string(mode)
                           << (position ? std::format(" at {},{}", position->first, position->second) : ""s)
                           << ", has "sv << (last ? describe(*last) : "no monitor"s);
        return false;
      }
      std::this_thread::sleep_for(options_.poll_interval);
    }
  }

  bool vdisplay_t::wait_for_cm_locked(const std::string &name, bool hdr) {
    const auto deadline = clock_t_::now() + options_.timeout;
    while (true) {
      auto monitor = find_monitor_locked(name);
      if (monitor && monitor_is_hdr(*monitor) == hdr) {
        return true;
      }
      if (clock_t_::now() >= deadline) {
        BOOST_LOG(warning) << "Hyprland virtual display: "sv << name << " wanted color management preset "sv << (hdr ? "hdr"sv : "srgb"sv)
                           << ", has "sv << (monitor ? (monitor->cm_preset.empty() ? "none"s : monitor->cm_preset) : "no monitor"s);
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
      return monitor_listed_locked(name) == false;
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
      // Under the lock, so an instance claiming the name meanwhile is seen as its owner.
      file_lock_t lock {owner_lock_path()};
      if (!lock.acquired()) {
        return;
      }
      if (auto owner = read_owner(m.name); owner && other_owner_alive(*owner)) {
        BOOST_LOG(info) << "Hyprland virtual display: leaving "sv << m.name << " alone, it belongs to running process "sv << owner->pid;
        continue;
      }
      BOOST_LOG(info) << "Hyprland virtual display: removing stale output "sv << m.name << " left by a previous run"sv;
      if (remove_output_locked(m.name)) {
        remove_owner_file(m.name);
      }
    }
    // Then outputs of ended sessions that remove() could not take away, as remove() would have finished them (saving
    // what the user changed, giving focus back). The snapshot still lists them: the loop above left owned outputs alone.
    std::vector<std::string> ended;
    for (const auto &[name, entry] : owned_) {
      if (entry.ended) {
        ended.push_back(name);
      }
    }
    for (const auto &name : ended) {
      const auto found = std::ranges::find(*monitors, name, &monitor_info_t::name);
      if (finish_remove_locked(name, found == monitors->end() ? nullptr : &*found) || found == monitors->end()) {
        BOOST_LOG(info) << "Hyprland virtual display: removed "sv << name << " of an ended session"sv;
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
        if (monitor_listed_locked(name) == added) {
          return true;
        }
      }
      if (clock_t_::now() >= deadline) {
        return false;
      }
    }
  }

  bool vdisplay_t::write_owner_file(const std::string &name) {
    if (options_.state_dir.empty()) {
      return true;
    }
    std::error_code ec;
    std::filesystem::create_directories(options_.state_dir, ec);
    std::ofstream out {options_.state_dir / name, std::ios::trunc};
    out << ::getpid();
    if (const auto start = process_start_time(::getpid())) {
      out << ' ' << *start;
    }
    out << '\n';
    out.close();
    if (!out) {
      BOOST_LOG(error) << "Hyprland virtual display: cannot write "sv << (options_.state_dir / name).string();
      return false;
    }
    return true;
  }

  std::filesystem::path vdisplay_t::owner_lock_path() const {
    if (options_.state_dir.empty()) {
      return {};
    }
    std::error_code ec;
    std::filesystem::create_directories(options_.state_dir, ec);
    return options_.state_dir / ".lock";
  }

  bool vdisplay_t::claim_owner_file(const std::string &name) {
    if (options_.state_dir.empty()) {
      return true;
    }
    file_lock_t lock {owner_lock_path()};
    if (!lock.acquired()) {
      return false;
    }
    if (auto owner = read_owner(name); owner && other_owner_alive(*owner)) {
      BOOST_LOG(error) << "Hyprland virtual display: "sv << name << " belongs to running process "sv << owner->pid;
      return false;
    }
    if (!write_owner_file(name)) {
      remove_owner_file(name);
      return false;
    }
    return true;
  }

  void vdisplay_t::release_owner_file(const std::string &name) {
    if (options_.state_dir.empty()) {
      return;
    }
    file_lock_t lock {owner_lock_path()};
    if (!lock.acquired()) {
      return;
    }
    if (auto owner = read_owner(name); owner && owner->pid == ::getpid()) {
      remove_owner_file(name);
    }
  }

  void vdisplay_t::remove_owner_file(const std::string &name) {
    if (options_.state_dir.empty()) {
      return;
    }
    std::error_code ec;
    std::filesystem::remove(options_.state_dir / name, ec);
  }

  std::optional<vdisplay_t::owner_t> vdisplay_t::read_owner(const std::string &name) const {
    if (options_.state_dir.empty()) {
      return std::nullopt;
    }
    std::ifstream in {options_.state_dir / name};
    owner_t owner;
    if (!(in >> owner.pid)) {
      return std::nullopt;
    }
    if (std::uint64_t start = 0; in >> start) {
      owner.start_time = start;
    }
    return owner;
  }

  bool vdisplay_t::other_owner_alive(const owner_t &owner) {
    if (owner.pid <= 0 || owner.pid == ::getpid() || (::kill(owner.pid, 0) != 0 && errno != EPERM)) {
      return false;
    }
    // A live PID with another start time is a crashed owner's, reused by the kernel. Without a recorded start time
    // (an owner file from before they were recorded) or a readable one, the PID counts.
    if (owner.start_time) {
      if (const auto now = process_start_time(owner.pid); now && *now != *owner.start_time) {
        return false;
      }
    }
    return true;
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

  void cleanup_stale(bool only_if_pending) {
    // A pending removal is retried whatever the status says: the backend reconnects first.
    if (only_if_pending || backend().status() == status_e::ok) {
      backend().cleanup_stale(only_if_pending);
    }
  }

  std::string create(const uuid_util::uuid_t &identity, const std::string &client_name, const mode_t &mode, const std::string &) {
    return backend().create(identity, client_name, mode);
  }

  bool set_mode(const std::string &display_name, const mode_t &mode, bool isolated) {
    return backend().set_mode(display_name, mode, isolated);
  }

  bool activate(const std::string &display_name) {
    return backend().activate(display_name);
  }

  void track_process_group(const std::string &display_name, int process_group_id) {
    backend().track_process_group(display_name, process_group_id);
  }

  bool remove(const uuid_util::uuid_t &identity) {
    return backend().remove(identity);
  }

  std::optional<bool> get_hdr(const std::string &display_name) {
    // Only Apollo's own outputs; physical monitors are handled by the display device code (HYP-18).
    if (backend().status() != status_e::ok) {
      return std::nullopt;
    }
    return backend().get_hdr(display_name);
  }

  bool set_hdr(const std::string &display_name, bool enable) {
    if (backend().status() != status_e::ok) {
      return false;
    }
    return backend().set_hdr(display_name, enable);
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
