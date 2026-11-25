/**
 * @file src/platform/linux/idle_inhibit.h
 * @brief Keep the session from idling (blanking, locking, suspending) while streaming, over D-Bus.
 *
 * Two inhibitors are taken, each best effort:
 * - `org.freedesktop.ScreenSaver.Inhibit` on the session bus. hypridle implements this service (and honours it
 *   unless `ignore_dbus_inhibit` is set), as do KDE and GNOME. The cookie belongs to the D-Bus connection, so the
 *   inhibitor keeps its own connection open while it holds one; closing it also releases the cookie.
 * - A logind `idle` block inhibitor on the system bus (`systemd-inhibit --what=idle`), held as a file descriptor.
 *   hypridle honours it unless `ignore_systemd_inhibit` is set; it also shows up in `systemd-inhibit --list`.
 *
 * Only compiled when GIO is available (`SUNSHINE_BUILD_GIO`).
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

typedef struct _GDBusConnection GDBusConnection;

namespace platf::idle {
  struct options_t {
    /// Bus addresses; nullopt means the user's session bus and the system bus. Tests pass private buses.
    std::optional<std::string> session_bus_address;
    std::optional<std::string> system_bus_address;
    bool use_screensaver = true;  ///< Take org.freedesktop.ScreenSaver.Inhibit.
    bool use_logind = true;  ///< Take a logind idle inhibitor.
    std::string who = "Apollo";  ///< Application name shown by the inhibitor lists.
    std::string why = "Streaming to a Moonlight client";
    std::chrono::milliseconds timeout {3000};  ///< Per D-Bus call.
  };

  /**
   * @brief Holds idle inhibitors between acquire() and release(). Thread-safe.
   */
  class inhibitor_t {
  public:
    explicit inhibitor_t(options_t options = {});
    ~inhibitor_t();

    inhibitor_t(const inhibitor_t &) = delete;
    inhibitor_t &operator=(const inhibitor_t &) = delete;

    /**
     * @brief Take the inhibitors that are not held yet.
     * @return true if at least one inhibitor is held afterwards.
     */
    bool acquire();

    /**
     * @brief Release whatever is held. Safe to call when nothing is held.
     */
    void release();

    bool screensaver_held() const;
    bool logind_held() const;

  private:
    bool acquire_screensaver_locked();
    bool acquire_logind_locked();
    void release_screensaver_locked();
    void release_logind_locked();

    options_t options_;
    mutable std::mutex mutex_;
    GDBusConnection *screensaver_bus_ = nullptr;  ///< Our own connection; the cookie lives as long as it does.
    std::optional<std::uint32_t> screensaver_cookie_;
    int logind_fd_ = -1;  ///< The inhibitor lasts until every copy of this fd is closed.
  };
}  // namespace platf::idle
