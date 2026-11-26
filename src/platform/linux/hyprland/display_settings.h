/**
 * @file src/platform/linux/hyprland/display_settings.h
 * @brief Declarations for the per-client settings of Hyprland virtual displays, and the pure logic around them.
 *
 * Windows remembers scale, layout and HDR per monitor identity. Hyprland forgets runtime monitor rules (a config
 * reload drops them, a restart loses them), so Apollo stores what it should re-apply for each `APOLLO-xxxxxxxx`
 * output itself (vdisplay_t, HYP-12). Everything here except display_settings_store_t's file access is pure, so the
 * unit tests cover it without a compositor.
 * Only compiled when Wayland support is enabled (`SUNSHINE_BUILD_WAYLAND`).
 */
#pragma once

// standard includes
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "ipc.h"
#include "src/platform/vdisplay.h"

namespace hyprland {
  /**
   * @brief What Apollo remembers about one virtual display. Unset fields are not applied.
   */
  struct display_settings_t {
    std::string client_name;  ///< For people reading the file; not used otherwise.
    std::optional<double> scale;  ///< Output scale the user chose.
    /// The app/client scale factor (percent) in effect when `scale` was learned. `scale` is only re-applied while the
    /// scale factor stays the same; a different factor replaces it with factor / 100.
    std::optional<int> scale_percent;
    std::optional<int> transform;  ///< wl_output_transform, 0-7.
    std::optional<int> bitdepth;  ///< 8 or 10.
    std::optional<std::string> position;  ///< `XxY`; only used when the display is not isolated.
    std::optional<std::string> workspace;  ///< Workspace name; unset means default_workspace_name().

    bool operator==(const display_settings_t &) const = default;

    /**
     * @brief Whether nothing but the client name is set.
     */
    bool empty() const;
  };

  nlohmann::json settings_to_json(const display_settings_t &settings);

  /**
   * @brief Parse one entry of the settings file. Fields with the wrong type or out of range are ignored.
   */
  display_settings_t settings_from_json(const nlohmann::json &json);

  /**
   * @brief The settings file: `{"version": 1, "displays": {"APOLLO-xxxxxxxx": {...}}}`.
   *
   * Every call reads (and save() rewrites) the whole file, so edits made by hand between sessions are kept.
   * Writes go to a temporary file that is renamed over the old one. Not thread-safe; vdisplay_t serialises calls.
   */
  class display_settings_store_t {
  public:
    /**
     * @param path The file. Empty disables persistence: load() returns nothing and save() does nothing.
     */
    explicit display_settings_store_t(std::filesystem::path path);

    /**
     * @brief The settings for an output, or default settings if there are none (or the file is unreadable).
     */
    display_settings_t load(const std::string &output) const;

    /**
     * @brief Store the settings for an output, replacing what was there. Empty settings remove the entry.
     * @return true if the file was written.
     */
    bool save(const std::string &output, const display_settings_t &settings) const;

    const std::filesystem::path &path() const;

  private:
    std::map<std::string, nlohmann::json> read_all() const;

    std::filesystem::path path_;
  };

  /**
   * @brief The workspace a virtual display gets by default: the output name in lowercase, e.g. `apollo-1a2b3c4d`.
   */
  std::string default_workspace_name(const std::string &output);

  /**
   * @brief The Hyprland selector for a workspace name: the number itself for numbered workspaces (`3`),
   * `name:<name>` for named ones.
   */
  std::string workspace_selector(const std::string &workspace);

  /**
   * @brief Bit depth of a DRM format name: 10 for the `*2101010` formats, 8 for `*8888`, nullopt otherwise.
   * @note Hyprland 0.57 keeps headless outputs at XRGB8888 whatever `bitdepth` says, so this only tells something
   *       on outputs that really switch formats.
   */
  std::optional<int> bitdepth_of_format(std::string_view format);

  /**
   * @brief The scale to apply when creating a virtual display.
   * @param settings Remembered settings.
   * @param scale_percent The app/client scale factor of this session (100 = none).
   * @return The remembered scale if it was learned under the same factor; otherwise factor / 100 if the factor is not
   *         100; otherwise nullopt (build_monitor_rule() then uses 1).
   */
  std::optional<double> choose_scale(const display_settings_t &settings, int scale_percent);

  /**
   * @brief Fields Apollo has put into the runtime rule of an output since this process started.
   *
   * `hl.monitor()` merges into the output's existing rule, and Hyprland keeps that rule after the output is removed,
   * so a field set in one session sticks to later outputs of the same name. When a later session has no value for
   * such a field, Apollo resets it to Hyprland's default rather than leaving the old value.
   */
  struct emitted_fields_t {
    bool transform = false;
    bool bitdepth = false;
  };

  /**
   * @brief Inputs of build_monitor_rule().
   */
  struct rule_inputs_t {
    std::string output;
    platf::vdisplay::mode_t mode;
    display_settings_t settings;
    /// Position for an isolated display (from isolated_position()); nullopt when not isolated.
    std::optional<std::string> isolated_position;
    emitted_fields_t emitted;  ///< See emitted_fields_t.
  };

  /**
   * @brief The monitor rule for a virtual display.
   *
   * Mode, position and scale always: the position is the isolated one, else the remembered one, else `auto-right`;
   * the scale comes from choose_scale(), else 1 (without a scale Hyprland picks one by resolution, e.g. 2 for
   * 2560x1600, which would change the stream's desktop size behind the user's back). Transform and bit depth only
   * when remembered, so static monitor rules for them apply; one Apollo set before (`emitted`) is reset to the default
   * (transform 0, bit depth 8) instead.
   */
  monitor_rule_t build_monitor_rule(const rule_inputs_t &inputs);

  /**
   * @brief What the user changed on a virtual display during a session, folded into the remembered settings.
   * @param stored The settings the session started with.
   * @param baseline The output right after Apollo last applied its rule.
   * @param now The output at the end of the session.
   * @param isolated Whether the display was isolated (its position is then Apollo's, not the user's).
   * @param scale_percent The session's scale factor (recorded with a learned scale).
   * @param assigned_workspace The workspace Apollo showed on the display, if it did (activate()); empty otherwise.
   * @return The new settings. Equal to `stored` when nothing changed.
   */
  display_settings_t learn_settings(
    const display_settings_t &stored,
    const monitor_info_t &baseline,
    const monitor_info_t &now,
    bool isolated,
    int scale_percent,
    const std::string &assigned_workspace
  );

  /**
   * @brief `ppid` and process group of a process, from `/proc/<pid>/stat`.
   */
  struct proc_stat_t {
    int ppid = 0;
    int pgid = 0;
    std::uint64_t start_time = 0;  ///< Field 22: clock ticks after boot when the process started; 0 if unknown.
  };

  /**
   * @brief Parse the contents of `/proc/<pid>/stat`. The command name may contain spaces and parentheses.
   */
  std::optional<proc_stat_t> parse_proc_stat(std::string_view stat);

  /**
   * @brief Read `/proc/<pid>/stat`.
   */
  std::optional<proc_stat_t> read_proc_stat(int pid);

  /**
   * @brief Whether a process belongs to a launched app: it, or one of its ancestors, is in the process group `pgid`
   * (or is its leader).
   *
   * Walking up the ancestors also catches children that moved to their own process group or session (e.g. through
   * `setsid`), as long as their parent is still alive.
   * @param read_stat Looks up a process (read_proc_stat() outside tests).
   */
  bool process_belongs_to_group(int pid, int pgid, const std::function<std::optional<proc_stat_t>(int)> &read_stat);

  /**
   * @brief One entry of `j/clients`, reduced to what window placement needs.
   */
  struct client_info_t {
    std::string address;  ///< `0x...`
    int pid = -1;
    std::string workspace;  ///< Workspace name.
    int monitor = -1;  ///< Monitor id.
  };

  /**
   * @brief Parse a `j/clients` reply. Entries that are not objects are skipped.
   */
  std::vector<client_info_t> parse_clients(const nlohmann::json &json);

  /**
   * @brief The window address in an `openwindow` event (`ADDR,WS,CLASS,TITLE`, ADDR without `0x`), as `0x...`.
   */
  std::optional<std::string> parse_openwindow_address(std::string_view data);

  /**
   * @brief Lua that shows a workspace on a monitor without moving focus, creating the workspace if needed and
   * moving it over if it is on another monitor. Returns `"ok"`, or `"no monitor"` if the monitor does not exist.
   */
  std::string lua_show_workspace(const std::string &monitor, const std::string &workspace);

  /**
   * @brief Lua dispatcher call focusing a monitor, e.g. `hl.dsp.focus({ monitor = "APOLLO-1a2b3c4d" })`.
   */
  std::string lua_focus_monitor(const std::string &monitor);

  /**
   * @brief Lua dispatcher call focusing a workspace (switching to its monitor).
   */
  std::string lua_focus_workspace(const std::string &workspace);

  /**
   * @brief Lua dispatcher call moving a window to a workspace without following it.
   * @param address Window address, `0x...`.
   */
  std::string lua_move_window(const std::string &address, const std::string &workspace);
}  // namespace hyprland
