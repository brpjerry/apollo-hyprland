/**
 * @file src/platform/linux/idle_inhibit.cpp
 * @brief Idle inhibition over D-Bus: org.freedesktop.ScreenSaver and logind.
 */
// local includes
#include "idle_inhibit.h"

// standard includes
#include <utility>

// platform includes
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <unistd.h>

// local includes
#include "src/logging.h"

using namespace std::literals;

namespace platf::idle {
  namespace {
    struct gerror_t {
      GError *error = nullptr;

      ~gerror_t() {
        if (error) {
          g_error_free(error);
        }
      }

      std::string message() const {
        return error ? error->message : "unknown error";
      }
    };

    /**
     * @brief Open a private connection to a message bus, so that closing it releases what it holds.
     */
    GDBusConnection *open_bus(const std::optional<std::string> &address, GBusType type) {
      gerror_t err;
      std::string addr;
      if (address) {
        addr = *address;
      } else {
        gchar *found = g_dbus_address_get_for_bus_sync(type, nullptr, &err.error);
        if (!found) {
          BOOST_LOG(debug) << "Idle inhibit: no "sv << (type == G_BUS_TYPE_SYSTEM ? "system"sv : "session"sv)
                           << " bus: "sv << err.message();
          return nullptr;
        }
        addr = found;
        g_free(found);
      }
      auto flags = static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION);
      GDBusConnection *bus = g_dbus_connection_new_for_address_sync(addr.c_str(), flags, nullptr, nullptr, &err.error);
      if (!bus) {
        BOOST_LOG(debug) << "Idle inhibit: cannot connect to "sv << addr << ": "sv << err.message();
        return nullptr;
      }
      // We call exit() ourselves; a dropped bus must not take Apollo down.
      g_dbus_connection_set_exit_on_close(bus, FALSE);
      return bus;
    }

    void close_bus(GDBusConnection *bus) {
      if (bus) {
        g_dbus_connection_close_sync(bus, nullptr, nullptr);
        g_object_unref(bus);
      }
    }
  }  // namespace

  inhibitor_t::inhibitor_t(options_t options):
      options_ {std::move(options)} {
  }

  inhibitor_t::~inhibitor_t() {
    release();
  }

  bool inhibitor_t::acquire() {
    std::lock_guard lock {mutex_};
    bool any = false;
    if (options_.use_screensaver) {
      any |= acquire_screensaver_locked();
    }
    if (options_.use_logind) {
      any |= acquire_logind_locked();
    }
    return any;
  }

  void inhibitor_t::release() {
    std::lock_guard lock {mutex_};
    release_screensaver_locked();
    release_logind_locked();
  }

  bool inhibitor_t::screensaver_held() const {
    std::lock_guard lock {mutex_};
    return screensaver_cookie_.has_value();
  }

  bool inhibitor_t::logind_held() const {
    std::lock_guard lock {mutex_};
    return logind_fd_ >= 0;
  }

  bool inhibitor_t::acquire_screensaver_locked() {
    if (screensaver_cookie_) {
      return true;
    }
    const int timeout_ms = static_cast<int>(options_.timeout.count());
    GDBusConnection *bus = open_bus(options_.session_bus_address, G_BUS_TYPE_SESSION);
    if (!bus) {
      return false;
    }
    gerror_t err;
    GVariant *reply = g_dbus_connection_call_sync(
      bus,
      "org.freedesktop.ScreenSaver",
      "/org/freedesktop/ScreenSaver",
      "org.freedesktop.ScreenSaver",
      "Inhibit",
      g_variant_new("(ss)", options_.who.c_str(), options_.why.c_str()),
      G_VARIANT_TYPE("(u)"),
      G_DBUS_CALL_FLAGS_NO_AUTO_START,
      timeout_ms,
      nullptr,
      &err.error
    );
    if (!reply) {
      BOOST_LOG(info) << "Idle inhibit: org.freedesktop.ScreenSaver.Inhibit failed: "sv << err.message();
      close_bus(bus);
      return false;
    }
    guint32 cookie = 0;
    g_variant_get(reply, "(u)", &cookie);
    g_variant_unref(reply);
    screensaver_bus_ = bus;
    screensaver_cookie_ = cookie;
    BOOST_LOG(info) << "Idle inhibit: holding org.freedesktop.ScreenSaver inhibitor (cookie "sv << cookie << ')';
    return true;
  }

  void inhibitor_t::release_screensaver_locked() {
    if (!screensaver_bus_) {
      return;
    }
    if (screensaver_cookie_) {
      gerror_t err;
      GVariant *reply = g_dbus_connection_call_sync(
        screensaver_bus_,
        "org.freedesktop.ScreenSaver",
        "/org/freedesktop/ScreenSaver",
        "org.freedesktop.ScreenSaver",
        "UnInhibit",
        g_variant_new("(u)", *screensaver_cookie_),
        nullptr,
        G_DBUS_CALL_FLAGS_NO_AUTO_START,
        static_cast<int>(options_.timeout.count()),
        nullptr,
        &err.error
      );
      if (reply) {
        g_variant_unref(reply);
        BOOST_LOG(info) << "Idle inhibit: released org.freedesktop.ScreenSaver inhibitor"sv;
      } else {
        // Closing the connection below releases it anyway.
        BOOST_LOG(debug) << "Idle inhibit: UnInhibit failed: "sv << err.message();
      }
    }
    close_bus(screensaver_bus_);
    screensaver_bus_ = nullptr;
    screensaver_cookie_.reset();
  }

  bool inhibitor_t::acquire_logind_locked() {
    if (logind_fd_ >= 0) {
      return true;
    }
    const int timeout_ms = static_cast<int>(options_.timeout.count());
    GDBusConnection *bus = open_bus(options_.system_bus_address, G_BUS_TYPE_SYSTEM);
    if (!bus) {
      return false;
    }
    gerror_t err;
    GUnixFDList *fds = nullptr;
    GVariant *reply = g_dbus_connection_call_with_unix_fd_list_sync(
      bus,
      "org.freedesktop.login1",
      "/org/freedesktop/login1",
      "org.freedesktop.login1.Manager",
      "Inhibit",
      g_variant_new("(ssss)", "idle", options_.who.c_str(), options_.why.c_str(), "block"),
      G_VARIANT_TYPE("(h)"),
      G_DBUS_CALL_FLAGS_NONE,
      timeout_ms,
      nullptr,
      &fds,
      nullptr,
      &err.error
    );
    // The fd stays valid after the connection closes; logind only watches the fd.
    close_bus(bus);
    if (!reply) {
      BOOST_LOG(info) << "Idle inhibit: logind Inhibit(idle) failed: "sv << err.message();
      return false;
    }
    gint32 index = -1;
    g_variant_get(reply, "(h)", &index);
    g_variant_unref(reply);
    int fd = -1;
    if (fds) {
      gerror_t fd_err;
      fd = g_unix_fd_list_get(fds, index, &fd_err.error);  // a dup; the list closes its own copy
      g_object_unref(fds);
    }
    if (fd < 0) {
      BOOST_LOG(warning) << "Idle inhibit: logind returned no inhibitor fd"sv;
      return false;
    }
    logind_fd_ = fd;
    BOOST_LOG(info) << "Idle inhibit: holding logind idle inhibitor"sv;
    return true;
  }

  void inhibitor_t::release_logind_locked() {
    if (logind_fd_ >= 0) {
      ::close(logind_fd_);
      logind_fd_ = -1;
      BOOST_LOG(info) << "Idle inhibit: released logind idle inhibitor"sv;
    }
  }
}  // namespace platf::idle
