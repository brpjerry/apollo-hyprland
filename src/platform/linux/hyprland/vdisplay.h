/**
 * @file src/platform/linux/hyprland/vdisplay.h
 * @brief Declarations for the Hyprland headless-output virtual display backend.
 *
 * platf::vdisplay on Linux (src/platform/vdisplay.h) is implemented by one global vdisplay_t. The class takes its
 * dependencies through vdisplay_t::options_t so the unit tests can run it against fake Hyprland sockets.
 * Only compiled when Wayland support is enabled (`SUNSHINE_BUILD_WAYLAND`).
 */
#pragma once

// standard includes
#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// local includes
#include "display_settings.h"
#include "ipc.h"
#include "src/platform/vdisplay.h"
#include "src/uuid.h"

namespace hyprland {
  /**
   * @brief The output name for an identity: `APOLLO-` and the first 8 hex digits of the UUID, lowercase.
   *
   * The same identity always gets the same name, so monitor rules the user writes for it keep applying.
   */
  std::string vdisplay_output_name(const uuid_util::uuid_t &identity);

  /**
   * @brief Whether a name has the form vdisplay_output_name() produces (`APOLLO-` + exactly 8 lowercase hex digits).
   *
   * Test outputs such as `APOLLO-TEST-1234abcd` do not match.
   */
  bool is_vdisplay_output_name(std::string_view name);

  /**
   * @brief The Hyprland mode string for a mode: `WxH@R.RRR`, e.g. `2560x1600@119.880`.
   */
  std::string vdisplay_mode_string(const platf::vdisplay::mode_t &mode);

  /**
   * @brief Whether a monitor shows a mode: exact size, refresh within 1 Hz.
   *
   * Hyprland reports custom refresh rates exactly as requested (e.g. 119.88), but snaps a request within about
   * 1 Hz of a listed mode to that mode (1920x1080@59.94 on a headless output becomes 60).
   */
  bool monitor_has_mode(const monitor_info_t &monitor, const platf::vdisplay::mode_t &mode);

  /**
   * @brief Where an isolated virtual display goes: below and to the right of every other enabled monitor,
   * with a gap, so it touches none of them and the pointer cannot cross between it and the physical layout.
   * @param monitors All monitors (the virtual display itself and disabled monitors are skipped).
   * @param own_name Name of the virtual display.
   * @param gap Gap in logical pixels.
   * @return A position string such as `5000x3000`.
   */
  std::string isolated_position(const std::vector<monitor_info_t> &monitors, const std::string &own_name, int gap);

  /**
   * @brief Whether a monitor shows the desktop, i.e. capturing it gives a picture.
   *
   * Disabled monitors are not advertised to Wayland clients at all. DPMS-off monitors still are, but Hyprland
   * stops rendering them: screencopy returns solid black once the DPMS fade has finished (checked on 0.57.2).
   * `FALLBACK` is the placeholder Hyprland adds when no monitor is left; it never counts.
   * Headless outputs, including Apollo's own, count like any other monitor.
   */
  bool monitor_is_active(const monitor_info_t &monitor);

  /**
   * @brief Whether any monitor in the list is active (see monitor_is_active()).
   */
  bool any_monitor_active(const std::vector<monitor_info_t> &monitors);

  /**
   * @brief The color management fields of a virtual display rule in HDR or SDR mode (see vdisplay_t::set_hdr()).
   * @details SDR only sets `cm = "srgb"`; the bit depth stays what the rule otherwise has.
   *          HDR advertises a 1000 cd/m² mastering display (0.005 cd/m² black, 400 cd/m² frame average) since a
   *          headless output has no EDID to take them from; clients receive them as HDR metadata.
   */
  void apply_hdr_fields(monitor_rule_t &rule, bool hdr);

  /**
   * @brief Whether a monitor shows HDR: its color management preset is `hdr` or `hdredid`.
   */
  bool monitor_is_hdr(const monitor_info_t &monitor);

  /**
   * @brief Hyprland headless outputs as virtual displays.
   *
   * All public methods are thread-safe. Output creation, removal and mode changes are serialised by one mutex.
   * Events from `.socket2.sock` are tracked under a separate mutex, so a thread waiting for an event never
   * blocks the listener thread.
   */
  class vdisplay_t {
  public:
    struct options_t {
      /// Connects to the compositor. Default: ipc_t::connect().
      std::function<std::unique_ptr<ipc_t>()> connect;
      /// Whether a live Hyprland instance exists at all; decides unavailable vs failed when connect() fails.
      /// Default: find_instance_signature().has_value().
      std::function<bool()> instance_exists;
      /// Whether Wayland capture can see an output with this name. nullopt: cannot tell (no Wayland connection),
      /// so creation does not wait for it. Default: looks the name up with wl::monitors().
      std::function<std::optional<bool>(const std::string &name)> output_visible;
      /// Directory for the files recording which process owns each output (crash cleanup).
      /// Default: `$XDG_RUNTIME_DIR/apollo-vdisplay`.
      std::filesystem::path state_dir;
      /// Upper bound for each wait: output appearing, becoming visible to Wayland, taking its mode, disappearing.
      std::chrono::milliseconds timeout {5000};
      /// Interval between polls of `j/monitors` and the Wayland registry.
      std::chrono::milliseconds poll_interval {50};
      /// Gap between an isolated virtual display and the rest of the layout, in logical pixels.
      int isolation_gap = 1000;
      /// File with the remembered per-client settings (HYP-12). Empty: nothing is remembered.
      /// Default: `vdisplay_settings.json` next to Apollo's state file (`file_state`).
      std::filesystem::path settings_file;
      /// Looks up a process for window placement. Default: read_proc_stat().
      std::function<std::optional<proc_stat_t>(int pid)> read_stat;
    };

    /**
     * @brief The defaults for every option.
     */
    static options_t default_options();

    explicit vdisplay_t(options_t options);
    ~vdisplay_t();

    vdisplay_t(const vdisplay_t &) = delete;
    vdisplay_t &operator=(const vdisplay_t &) = delete;

    /**
     * @brief Connect to Hyprland and start listening to events.
     *
     * The first time this succeeds, outputs left over by a crashed Apollo are removed (cleanup_stale()).
     */
    platf::vdisplay::status_e init();
    platf::vdisplay::status_e status() const;

    /**
     * @brief Remove `APOLLO-xxxxxxxx` outputs that this process does not own and whose recorded owner process
     * is no longer running.
     */
    void cleanup_stale();

    /**
     * @brief Create (or reuse) the output for an identity and give it a mode.
     *
     * The remembered settings for the output (scale, transform, bit depth, position) are applied with the mode;
     * `mode.scale_percent` other than 100 sets the scale unless a scale was remembered under the same factor
     * (see choose_scale()).
     * @return The output name, or "" on failure (the output is removed again in that case).
     */
    std::string create(const uuid_util::uuid_t &identity, const std::string &client_name, const platf::vdisplay::mode_t &mode);

    /**
     * @brief Apply a mode to an output this backend created.
     * @param isolated Move the output away from the rest of the layout (see isolated_position()).
     */
    bool set_mode(const std::string &name, const platf::vdisplay::mode_t &mode, bool isolated);

    /**
     * @brief Show the display's workspace on it and move focus there, remembering the previous focus.
     *
     * The workspace is the remembered one, or `apollo-xxxxxxxx` (default_workspace_name()). It is created on the
     * output, or moved there if it exists elsewhere (e.g. left behind on a physical monitor by the last session).
     * New windows then open on the virtual display rather than on a physical monitor.
     * @return true on success.
     */
    bool activate(const std::string &name);

    /**
     * @brief Move the windows of a launched app onto the display's workspace, now and whenever one opens later.
     *
     * A window belongs to the app if its process, or an ancestor, is in the process group (process_belongs_to_group()).
     * Windows already on the display are left alone, and focus does not follow a moved window. Needs activate().
     * Stops when the output is removed.
     * @param pgid Process group of the launched app.
     */
    void track_process_group(const std::string &name, int pgid);

    /**
     * @brief Whether an output this backend created is in HDR mode (color management preset `hdr`).
     * @return std::nullopt for outputs Apollo did not create (physical monitors belong to the display device code),
     *         or if Hyprland cannot be asked.
     */
    std::optional<bool> get_hdr(const std::string &name);

    /**
     * @brief Switch an output this backend created to HDR (or back to SDR) and wait until Hyprland applied it.
     * @details HDR: `cm = "hdr"`, `bitdepth = 10`, HDR and wide color support forced on (headless outputs have no
     *          EDID) and the luminances of apply_hdr_fields(). SDR: `cm = "srgb"` and the bit depth the output would have
     *          anyway. The state is part of the output's rule from then on, so set_mode(), config reloads and the
     *          baseline remove() learns user changes against include it.
     */
    bool set_hdr(const std::string &name, bool enable);

    /**
     * @brief Remove the output for an identity.
     *
     * Before removing it, what the user changed on the output during the session (scale, transform, bit depth,
     * position unless isolated, workspace) is saved for the next session (learn_settings()). If the display had focus
     * and was activated, focus goes back to the workspace that had it before activate().
     * @return true if it existed and is gone.
     */
    bool remove(const uuid_util::uuid_t &identity);

    /**
     * @brief Names of the outputs this backend created and has not removed yet.
     */
    std::vector<std::string> owned_outputs() const;

    /**
     * @brief Whether Hyprland has any active monitor (see monitor_is_active()).
     * @return std::nullopt if Hyprland cannot be asked.
     */
    std::optional<bool> any_monitor_active();

  private:
    struct focus_t {
      std::string monitor;
      std::string workspace;
    };

    struct owned_t {
      uuid_util::uuid_t identity;
      monitor_rule_t rule;  ///< The rule last applied, re-applied after a config reload.
      display_settings_t settings;  ///< Remembered settings, as loaded at create().
      int scale_percent = 100;  ///< The session's scale factor.
      bool isolated = false;
      std::optional<monitor_info_t> baseline;  ///< The output right after Apollo last applied its rule.
      std::string workspace;  ///< Workspace shown by activate(); empty before.
      std::optional<focus_t> previous_focus;  ///< Focus before activate().
      int pgid = 0;  ///< Process group whose windows go to `workspace`; 0: none.
      platf::vdisplay::mode_t mode {};  ///< The mode of the rule last applied.
      std::optional<bool> hdr;  ///< HDR state set by set_hdr(); nullopt: never set, the rule leaves it alone.
    };

    void on_event(std::string_view event, std::string_view data);
    void reapply_loop();

    // The following expect mutex_ to be held.
    bool ensure_connected_locked();
    bool connect_locked();
    std::optional<std::vector<monitor_info_t>> monitors_locked();
    std::optional<monitor_info_t> find_monitor_locked(const std::string &name);
    bool apply_rule_locked(owned_t &entry, monitor_rule_t rule, const platf::vdisplay::mode_t &mode);
    monitor_rule_t rule_for_locked(const std::string &name, const owned_t &entry, const platf::vdisplay::mode_t &mode, std::optional<std::string> isolated_position);
    void learn_locked(const std::string &name, const owned_t &entry, const monitor_info_t &now);
    void restore_focus_locked(const focus_t &focus);
    bool focus_monitor_locked(const std::string &monitor);
    void adopt_windows_locked(const std::vector<std::string> &addresses);
    /// Wait until the output has the mode (and position, if given). Also waits for the rule's scale and transform,
    /// which Hyprland applies together with the mode, but only fails on the mode and position.
    bool wait_for_mode_locked(const monitor_rule_t &rule, const platf::vdisplay::mode_t &mode, const std::optional<std::pair<int, int>> &position);
    bool wait_for_cm_locked(const std::string &name, bool hdr);
    bool remove_output_locked(const std::string &name);
    void cleanup_stale_locked();
    /// Wait until more than `seen` monitoraddedv2 (added) or monitorremovedv2 events arrived for `name`.
    /// Falls back to polling `j/monitors` while the event socket is disconnected.
    bool wait_for_output_event_locked(const std::string &name, bool added, std::uint64_t seen);

    // Event counters, guarded by events_mutex_.
    std::uint64_t added_count(const std::string &name);
    std::uint64_t removed_count(const std::string &name);

    // Owner files.
    void write_owner_file(const std::string &name);
    void remove_owner_file(const std::string &name);
    std::optional<int> read_owner_pid(const std::string &name);

    options_t options_;

    mutable std::mutex mutex_;  ///< Serialises IPC operations and guards everything below up to events_mutex_.
    std::unique_ptr<ipc_t> ipc_;
    std::unique_ptr<event_listener_t> listener_;
    std::map<std::string, owned_t> owned_;
    std::map<std::string, emitted_fields_t> emitted_;  ///< Kept after removal; see emitted_fields_t.
    display_settings_store_t store_;
    std::atomic<platf::vdisplay::status_e> status_ {platf::vdisplay::status_e::unknown};  ///< Written under mutex_, read without.
    bool cleaned_up_ = false;  ///< cleanup_stale() has run after a successful init().

    std::mutex events_mutex_;
    std::condition_variable events_cv_;
    std::map<std::string, std::uint64_t> added_;  ///< monitoraddedv2 events seen per output name.
    std::map<std::string, std::uint64_t> removed_;  ///< monitorremovedv2 events seen per output name.
    bool reload_pending_ = false;
    std::deque<std::string> opened_windows_;  ///< Addresses from openwindow events, for the worker thread.
    bool stopping_ = false;
    std::thread reapply_thread_;
  };
}  // namespace hyprland
