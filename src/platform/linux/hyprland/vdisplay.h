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
     * @return The output name, or "" on failure (the output is removed again in that case).
     */
    std::string create(const uuid_util::uuid_t &identity, const std::string &client_name, const platf::vdisplay::mode_t &mode);

    /**
     * @brief Apply a mode to an output this backend created.
     * @param isolated Move the output away from the rest of the layout (see isolated_position()).
     */
    bool set_mode(const std::string &name, const platf::vdisplay::mode_t &mode, bool isolated);

    /**
     * @brief Remove the output for an identity.
     * @return true if it existed and is gone.
     */
    bool remove(const uuid_util::uuid_t &identity);

    /**
     * @brief Names of the outputs this backend created and has not removed yet.
     */
    std::vector<std::string> owned_outputs() const;

  private:
    struct owned_t {
      uuid_util::uuid_t identity;
      monitor_rule_t rule;  ///< The rule last applied, re-applied after a config reload.
    };

    void on_event(std::string_view event, std::string_view data);
    void reapply_loop();

    // The following expect mutex_ to be held.
    bool ensure_connected_locked();
    bool connect_locked();
    std::optional<std::vector<monitor_info_t>> monitors_locked();
    std::optional<monitor_info_t> find_monitor_locked(const std::string &name);
    bool apply_rule_locked(owned_t &entry, monitor_rule_t rule, const platf::vdisplay::mode_t &mode);
    bool wait_for_mode_locked(const std::string &name, const platf::vdisplay::mode_t &mode, const std::optional<std::pair<int, int>> &position);
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
    std::atomic<platf::vdisplay::status_e> status_ {platf::vdisplay::status_e::unknown};  ///< Written under mutex_, read without.
    bool cleaned_up_ = false;  ///< cleanup_stale() has run after a successful init().

    std::mutex events_mutex_;
    std::condition_variable events_cv_;
    std::map<std::string, std::uint64_t> added_;  ///< monitoraddedv2 events seen per output name.
    std::map<std::string, std::uint64_t> removed_;  ///< monitorremovedv2 events seen per output name.
    bool reload_pending_ = false;
    bool stopping_ = false;
    std::thread reapply_thread_;
  };
}  // namespace hyprland
