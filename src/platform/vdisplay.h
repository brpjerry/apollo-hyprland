/**
 * @file src/platform/vdisplay.h
 * @brief Declarations for the platform-neutral virtual display interface.
 *
 * Each platform provides one implementation:
 * - Windows: src/platform/windows/vdisplay.cpp, a thin wrapper over the SudoVDA driver (VDISPLAY::*).
 * - Linux with Wayland: src/platform/linux/hyprland/vdisplay.cpp, Hyprland headless outputs.
 * - Elsewhere: src/platform/vdisplay_stub.cpp, which reports status_e::unavailable and never creates a display.
 *
 * The rules for which identity, name and mode a session's display gets live in common code (src/process.cpp).
 */
#pragma once

// standard includes
#include <optional>
#include <string>

// local includes
#include "src/utility.h"
#include "src/uuid.h"

namespace platf::vdisplay {
  /**
   * @brief State of the virtual display backend.
   * @note The numeric values of every state except `unavailable` are the SudoVDA driver status codes
   *       the web UI understands (`vdisplayStatus`); keep them stable.
   */
  enum class status_e : int {
    ok = 0,  ///< The backend works and displays can be created.
    unavailable = -100,  ///< This platform or session has no virtual display support at all.
    failed = -1,  ///< The backend exists but could not be opened.
    version_incompatible = -2,  ///< The backend is too old or too new for this build.
    watchdog_failed = -3,  ///< The backend stopped answering after it was opened.
    unknown = 1,  ///< Not initialised yet.
  };

  /**
   * @brief A display mode.
   */
  struct mode_t {
    int width;
    int height;
    int refresh_millihz;  ///< Refresh rate in mHz, e.g. 60000 for 60 Hz.
    /// Desktop scale in percent (the app/client resolution scale factor, 100 = none). Hyprland uses it as the output
    /// scale, so the desktop keeps the client's logical size; Windows ignores it.
    int scale_percent = 100;
  };

  /**
   * @brief (Re)initialise the backend.
   * @return The new status, also returned by status() afterwards.
   */
  status_e init();

  /**
   * @brief The cached backend status.
   */
  status_e status();

  /**
   * @brief Remove displays left behind by a previous run that crashed, and retry removing the displays of ended
   *        sessions that could not be removed then. No-op on Windows.
   * @note Safe to call while sessions run: displays of running sessions are never removed.
   * @param only_if_pending Only retry pending removals, and do nothing when there are none (no compositor request).
   */
  void cleanup_stale(bool only_if_pending = false);

  /**
   * @brief Create the virtual display for an identity.
   * @param identity Stable identity of the display. The same identity gets the same display (and the
   *                 display settings the OS remembers for it) on every session.
   * @param client_name Human-readable name (the EDID monitor name on Windows).
   * @param mode Initial mode.
   * @param identity_str The textual form of `identity` as it was configured (client or app UUID string).
   *                     Windows hands it to the driver as the monitor serial; other backends may ignore it.
   *                     Empty means `identity.string()`.
   * @return The name capture should use as `proc_t::display_name`, or "" on failure. On failure the display
   *         may still have been created, so callers must still call remove() with the same identity.
   */
  std::string create(const uuid_util::uuid_t &identity, const std::string &client_name, const mode_t &mode, const std::string &identity_str = {});

  /**
   * @brief Change the mode of a display created by create().
   * @param display_name The name returned by create().
   * @param mode The new mode.
   * @param isolated Also make it the only active display (Windows: "isolated virtual display" option).
   * @return true on success.
   */
  bool set_mode(const std::string &display_name, const mode_t &mode, bool isolated);

#ifndef _WIN32
  /**
   * @brief Give the display focus and its own workspace for the stream, so new windows open on it.
   *
   * Hyprland: shows the display's workspace (`apollo-xxxxxxxx`, or the one the client used last) on it and focuses
   * it; remove() restores the previous focus. Elsewhere a no-op. Not declared on Windows, where Apollo does not do this.
   * @param display_name The name returned by create().
   * @return true on success.
   */
  bool activate(const std::string &display_name);

  /**
   * @brief Keep the windows of a launched app on the display: move them there now and whenever one opens.
   *
   * Hyprland: windows whose process (or an ancestor) is in the process group go to the display's workspace. Stops
   * when the display is removed. Elsewhere a no-op. Not declared on Windows.
   * @param display_name The name returned by create().
   * @param process_group_id The app's process group (`boost::process::group::native_handle()`).
   */
  void track_process_group(const std::string &display_name, int process_group_id);
#endif

  /**
   * @brief Remove the display created for an identity.
   *
   * Hyprland also remembers what the user changed on the display during the session (scale, transform, position,
   * workspace) and applies it again on the next create() for the same identity.
   * @return true if a display was removed.
   */
  bool remove(const uuid_util::uuid_t &identity);

  /**
   * @brief Whether HDR is enabled on a display.
   * @return std::nullopt if the backend cannot tell (HDR handling is then skipped).
   */
  std::optional<bool> get_hdr(const std::string &display_name);

  /**
   * @brief Enable or disable HDR on a display.
   * @return true on success.
   */
  bool set_hdr(const std::string &display_name, bool enable);

  /**
   * @brief Ask the backend to render virtual displays on the named GPU adapter. Windows only; no-op elsewhere.
   */
  void set_render_adapter(const std::string &adapter_name);

  /**
   * @brief The value `config::video.output_name` should get so capture picks the display.
   * @param display_name The name returned by create().
   */
  std::string output_name_for(const std::string &display_name);

  /**
   * @brief Whether any display, physical or virtual, is active, i.e. can be captured.
   *
   * Used where libdisplaydevice cannot tell (it only supports Windows): with no active display Apollo probes
   * encoders on a temporary virtual display and streams every app on a virtual display.
   * Hyprland: an output is active when it is enabled and DPMS is on (see hyprland::monitor_is_active()).
   * @return std::nullopt if the backend cannot tell (always on Windows, and whenever status() is not `ok`).
   */
  std::optional<bool> any_display_active();
}  // namespace platf::vdisplay
