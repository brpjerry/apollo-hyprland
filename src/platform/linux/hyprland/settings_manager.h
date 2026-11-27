/**
 * @file src/platform/linux/hyprland/settings_manager.h
 * @brief Declarations for the display-device settings manager on Hyprland (dd_* options for monitors).
 *
 * libdisplaydevice only has a Windows backend. Its common part (SettingsManagerInterface, persistence, retry
 * scheduler, JSON for its types) is platform neutral, so this file implements SettingsManagerInterface for Hyprland
 * and src/display_device.cpp drives it exactly as it drives the Windows SettingsManager: apply on stream start, revert
 * on stop / after dd_config_revert_delay / on disconnect, retry while Hyprland is unreachable, revert at startup after a
 * crash, reset from the tray and the web UI.
 *
 * Outputs are identified by connector name (`DP-3`), which is also what Wayland capture uses as the display name.
 * The compositor is reached through compositor_t so the logic can be tested without Hyprland.
 * Only compiled when Wayland support is enabled (`SUNSHINE_BUILD_WAYLAND`).
 */
#pragma once

// standard includes
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// lib includes
#include <display_device/settings_manager_interface.h>
#include <display_device/settings_persistence_interface.h>
#include <nlohmann/json.hpp>

// local includes
#include "ipc.h"

namespace hyprland::dd {
  /**
   * @brief A display mode.
   */
  struct output_mode_t {
    int width = 0;
    int height = 0;
    double refresh_hz = 0.0;

    bool operator==(const output_mode_t &) const = default;
  };

  /**
   * @brief One output as reported by `j/monitors all`, with the fields the settings manager needs.
   */
  struct output_t {
    std::string name;  ///< Connector name, the device id.
    std::string description;
    int id = -1;
    bool enabled = true;
    bool dpms_on = true;
    output_mode_t mode;
    int x = 0;
    int y = 0;
    double scale = 1.0;
    int transform = 0;
    std::string cm;  ///< `colorManagementPreset`, e.g. `srgb` or `hdr`.
    std::optional<int> bitdepth;  ///< From `currentFormat`; nullopt if unknown.
    bool hdr_capable = false;  ///< `hardwareDetails.hdr`.
    bool any_mode = false;  ///< Takes any mode (headless backend), so `modes` does not limit the choice.
    std::vector<output_mode_t> modes;  ///< `availableModes`.
    bool focused = false;
    std::string active_workspace;
    bool is_virtual = false;  ///< An Apollo virtual display (`APOLLO-<8 hex>`).
  };

  /**
   * @brief A workspace and the output it is on.
   */
  struct workspace_t {
    std::string name;
    std::string monitor;

    bool operator==(const workspace_t &) const = default;
  };

  /**
   * @brief The outputs and workspaces of the compositor at one moment.
   */
  struct layout_t {
    std::vector<output_t> outputs;
    std::vector<workspace_t> workspaces;
    /// False when the workspace query failed: `workspaces` is then empty but not known to be. Saving or restoring
    /// the workspaces needs them; listing and naming the outputs does not.
    bool workspaces_listed = true;

    const output_t *find(std::string_view name) const;
    const workspace_t *find_workspace(std::string_view name) const;
  };

  /**
   * @brief Parse an `availableModes` entry such as `1920x1080@60.00Hz`.
   */
  std::optional<output_mode_t> parse_mode_string(std::string_view str);

  /**
   * @brief The mode as a monitor rule value, e.g. `1920x1080@59.940`.
   */
  std::string mode_string(const output_mode_t &mode);

  /**
   * @brief Parse a `j/monitors all` reply. The FALLBACK output Hyprland creates when nothing else is enabled is skipped.
   */
  std::vector<output_t> parse_outputs(const nlohmann::json &monitors);

  /**
   * @brief Parse a `j/workspaces` reply.
   */
  std::vector<workspace_t> parse_workspaces(const nlohmann::json &workspaces);

  /**
   * @brief Whether a colour management preset means HDR (`hdr`, `hdredid`).
   */
  bool is_hdr_preset(std::string_view cm);

  /**
   * @brief Pick the mode to set on an output for a requested size and refresh rate.
   *
   * Outputs that take any mode (headless, virtual) get the request as is. Otherwise the size must be among the
   * output's modes, and the refresh rate is the closest one within 0.5 Hz, else the lowest one above the request,
   * else the highest one.
   * @return nullopt if the output has no mode of that size.
   */
  std::optional<output_mode_t> choose_mode(const output_t &output, int width, int height, double refresh_hz);

  /**
   * @brief Access to the compositor. The production implementation is hyprland_compositor_t; tests use a fake.
   */
  class compositor_t {
  public:
    virtual ~compositor_t() = default;

    /**
     * @return The current layout, or nullopt if the compositor cannot be reached or does not list its outputs.
     */
    virtual std::optional<layout_t> layout() = 0;

    /**
     * @brief Whether a monitor rule merges into the output's existing rule (Lua), so unset fields keep their value.
     * Otherwise (hyprlang) a rule replaces the previous one and must carry every field.
     */
    virtual bool merges_rules() const = 0;

    virtual bool set_monitor(const monitor_rule_t &rule) = 0;
    virtual bool focus_monitor(const std::string &name) = 0;

    /**
     * @brief Move a workspace to a monitor (creating it if needed) and make it the one shown there.
     */
    virtual bool show_workspace(const std::string &monitor, const std::string &workspace) = 0;

    /**
     * @brief HDR state of an Apollo virtual display (platf::vdisplay::get_hdr()).
     */
    virtual std::optional<bool> virtual_hdr(const std::string &name) = 0;

    /**
     * @brief Switch HDR on an Apollo virtual display (platf::vdisplay::set_hdr()).
     */
    virtual bool set_virtual_hdr(const std::string &name, bool enable) = 0;
  };

  /**
   * @brief compositor_t over Hyprland's IPC socket.
   */
  class hyprland_compositor_t: public compositor_t {
  public:
    struct options_t {
      /// Directory holding the instance directories. Empty: runtime_dir().
      std::filesystem::path hypr_dir;
      /// Talk only to this instance. nullopt: the instance find_instance_signature() finds, looked up again whenever
      /// the connection fails (Hyprland restarted).
      std::optional<std::string> signature;
      /// Safety check: outputs this returns false for are never changed (set_monitor() and show_workspace() refuse).
      /// Tests pass one that only allows headless outputs. Empty: every output may be changed.
      std::function<bool(const output_t &)> may_change;
      /// Virtual display HDR. Default: platf::vdisplay::get_hdr() / set_hdr().
      std::function<std::optional<bool>(const std::string &)> get_virtual_hdr;
      std::function<bool(const std::string &, bool)> set_virtual_hdr;
    };

    explicit hyprland_compositor_t(options_t options);

    std::optional<layout_t> layout() override;
    bool merges_rules() const override;
    bool set_monitor(const monitor_rule_t &rule) override;
    bool focus_monitor(const std::string &name) override;
    bool show_workspace(const std::string &monitor, const std::string &workspace) override;
    std::optional<bool> virtual_hdr(const std::string &name) override;
    bool set_virtual_hdr(const std::string &name, bool enable) override;

    /**
     * @return The signature of the instance currently connected to, or empty.
     */
    std::string signature() const;

  private:
    const ipc_t *connected();
    bool allowed(const std::string &output);

    options_t options_;
    std::unique_ptr<ipc_t> ipc_;
  };

  /**
   * @brief What the manager has changed on an output, i.e. what a revert restores.
   */
  struct touched_t {
    bool enabled = false;
    bool mode = false;
    bool position = false;
    bool hdr = false;

    bool any() const;
    touched_t &operator|=(const touched_t &other);
    bool operator==(const touched_t &) const = default;
  };

  /**
   * @brief An output as it was before the manager changed anything.
   */
  struct saved_output_t {
    std::string name;
    std::string description;
    bool enabled = true;
    output_mode_t mode;
    int x = 0;
    int y = 0;
    std::string cm;
    std::optional<int> bitdepth;
    std::optional<bool> virtual_hdr;  ///< Virtual displays only, if it was read.
    std::string active_workspace;
    bool is_virtual = false;

    bool operator==(const saved_output_t &) const = default;
  };

  /**
   * @brief The persisted state (appdata/display_device.state): the original layout and what was changed.
   */
  struct state_t {
    std::vector<saved_output_t> initial;
    std::map<std::string, touched_t> touched;
    std::vector<workspace_t> workspaces;  ///< Workspaces and their monitors before the first change.
    std::string focused;  ///< The focused monitor before the first change.
    bool focus_changed = false;  ///< The manager moved focus; a revert moves it back.

    const saved_output_t *find(std::string_view name) const;
    bool operator==(const state_t &) const = default;
  };

  nlohmann::json state_to_json(const state_t &state);

  /**
   * @return nullopt if the JSON is not a valid state.
   */
  std::optional<state_t> state_from_json(const nlohmann::json &json);

  /**
   * @brief Target values for one output. Unset fields are left alone.
   */
  struct desired_t {
    std::optional<bool> enabled;
    std::optional<output_mode_t> mode;
    std::optional<std::pair<int, int>> position;
    std::optional<std::string> cm;
    std::optional<int> bitdepth;
    std::optional<bool> virtual_hdr;
  };

  using plan_t = std::map<std::string, desired_t>;

  /**
   * @brief Result of planning an apply.
   */
  struct apply_plan_t {
    std::string target;  ///< Output the configuration is for.
    plan_t plan;
    std::map<std::string, touched_t> touched;  ///< Everything changed after the apply, including earlier applies.
    bool focus_target = false;  ///< ensure_primary / ensure_only_display: the target gets focus.
  };

  /**
   * @brief Find the output a device id names: a connector name, a description (optionally prefixed `desc:`), or
   * empty for the primary output (the enabled physical output at 0,0, else the focused one, else the first).
   * Looks in `state.initial`, which should include every current output.
   * @return The connector name, or nullopt.
   */
  std::optional<std::string> resolve_device(const state_t &state, const layout_t &current, const std::string &device_id);

  /**
   * @brief Plan an apply: restore what earlier applies changed, then apply `config` to its device.
   * @param state The persisted state; `initial` must include every current output.
   * @param current The current layout.
   * @return The plan, or the failure to report.
   */
  std::variant<apply_plan_t, display_device::SettingsManagerInterface::ApplyResult> plan_apply(
    const state_t &state,
    const layout_t &current,
    const display_device::SingleDisplayConfiguration &config
  );

  /**
   * @brief Plan a revert: every touched field of every present output goes back to its initial value.
   * @param pending Receives the touched entries of physical outputs that are missing (to retry once they return).
   */
  plan_t plan_revert(const state_t &state, const layout_t &current, std::map<std::string, touched_t> &pending);

  /**
   * @brief Build the monitor rules that take `current` to `plan`: enables first, then changes, then disables.
   * @param merges compositor_t::merges_rules().
   */
  std::vector<monitor_rule_t> rules_for(const plan_t &plan, const layout_t &current, bool merges);

  /**
   * @brief Which kind of change failed.
   */
  enum class failure_e {
    none,
    unreachable,  ///< Hyprland stopped answering.
    topology,  ///< An output was not enabled / disabled, or is gone.
    mode,
    position,
    hdr,
  };

  /**
   * @brief Whether `current` satisfies the plan (virtual HDR is not checked).
   * @param failed Receives a description of the first mismatch.
   * @param kind Receives the kind of the first mismatch.
   */
  bool plan_satisfied(const plan_t &plan, const layout_t &current, std::string *failed = nullptr, failure_e *kind = nullptr);

  /**
   * @brief SettingsManagerInterface for Hyprland.
   */
  class settings_manager_t: public display_device::SettingsManagerInterface {
  public:
    struct options_t {
      /// How long to wait for Hyprland to report the new layout after a change.
      std::chrono::milliseconds settle_timeout {5000};
      std::chrono::milliseconds poll_interval {50};
      /// Sleeps between polls. Default: std::this_thread::sleep_for.
      std::function<void(std::chrono::milliseconds)> sleep;
    };

    settings_manager_t(
      std::shared_ptr<compositor_t> compositor,
      std::shared_ptr<display_device::SettingsPersistenceInterface> persistence,
      options_t options
    );
    settings_manager_t(std::shared_ptr<compositor_t> compositor, std::shared_ptr<display_device::SettingsPersistenceInterface> persistence);

    [[nodiscard]] display_device::EnumeratedDeviceList enumAvailableDevices() const override;
    [[nodiscard]] std::string getDisplayName(const std::string &device_id) const override;
    [[nodiscard]] ApplyResult applySettings(const display_device::SingleDisplayConfiguration &config) override;
    [[nodiscard]] RevertResult revertSettings() override;
    [[nodiscard]] bool resetPersistence() override;

    /**
     * @brief The state as last loaded or saved (for tests and logging).
     */
    const std::optional<state_t> &state() const;

  private:
    bool save_state(const std::optional<state_t> &state);
    /// Issue the rules and HDR changes and wait until the layout reports them.
    failure_e execute(const plan_t &plan, const layout_t &current, std::string &failure);
    std::optional<layout_t> wait_for(const plan_t &plan, std::string &failure, failure_e &kind);
    RevertResult revert(bool log_nothing_to_do);

    std::shared_ptr<compositor_t> compositor_;
    std::shared_ptr<display_device::SettingsPersistenceInterface> persistence_;
    options_t options_;
    std::optional<state_t> state_;
  };

  /**
   * @brief The settings manager for Apollo: Hyprland found the usual way, state in `persistence_filepath`.
   * @return nullptr when there is nothing to manage: no Hyprland session (none reachable, HYPRLAND_INSTANCE_SIGNATURE
   *         unset) and no state left to restore.
   */
  std::unique_ptr<display_device::SettingsManagerInterface> make_settings_manager(const std::filesystem::path &persistence_filepath);
}  // namespace hyprland::dd
