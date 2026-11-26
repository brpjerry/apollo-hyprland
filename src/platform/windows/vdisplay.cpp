/**
 * @file src/platform/windows/vdisplay.cpp
 * @brief platf::vdisplay on Windows: a thin wrapper over the SudoVDA driver (VDISPLAY::*).
 */
// standard includes
#include <atomic>
#include <cstring>

// local includes
#include "src/display_device.h"
#include "src/platform/vdisplay.h"

// platform includes
#include <windows.h>

#include "misc.h"
#include "virtual_display.h"

namespace platf::vdisplay {
  namespace {
    std::atomic<status_e> driver_status {status_e::unknown};

    status_e from_driver_status(VDISPLAY::DRIVER_STATUS s) {
      switch (s) {
        case VDISPLAY::DRIVER_STATUS::OK:
          return status_e::ok;
        case VDISPLAY::DRIVER_STATUS::FAILED:
          return status_e::failed;
        case VDISPLAY::DRIVER_STATUS::VERSION_INCOMPATIBLE:
          return status_e::version_incompatible;
        case VDISPLAY::DRIVER_STATUS::WATCHDOG_FAILED:
          return status_e::watchdog_failed;
        case VDISPLAY::DRIVER_STATUS::UNKNOWN:
        default:
          return status_e::unknown;
      }
    }

    GUID to_guid(const uuid_util::uuid_t &identity) {
      static_assert(sizeof(GUID) == sizeof(uuid_util::uuid_t));
      GUID guid;
      std::memcpy(&guid, &identity, sizeof(GUID));
      return guid;
    }

    void on_watchdog_failed() {
      driver_status = status_e::watchdog_failed;
      VDISPLAY::closeVDisplayDevice();
    }
  }  // namespace

  status_e init() {
    driver_status = from_driver_status(VDISPLAY::openVDisplayDevice());
    if (driver_status == status_e::ok) {
      if (!VDISPLAY::startPingThread(on_watchdog_failed)) {
        on_watchdog_failed();
      }
    }
    return driver_status;
  }

  status_e status() {
    return driver_status;
  }

  void cleanup_stale() {
    // SudoVDA removes displays whose watchdog expires, so nothing survives a crash.
  }

  std::string create(const uuid_util::uuid_t &identity, const std::string &client_name, const mode_t &mode, const std::string &identity_str) {
    const auto uid = identity_str.empty() ? identity.string() : identity_str;
    const auto name = VDISPLAY::createVirtualDisplay(
      uid.c_str(),
      client_name.c_str(),
      mode.width,
      mode.height,
      mode.refresh_millihz,
      to_guid(identity)
    );
    return platf::to_utf8(name);
  }

  bool set_mode(const std::string &display_name, const mode_t &mode, bool isolated) {
    const auto name = platf::from_utf8(display_name);
    LONG result;
    if (isolated) {
      result = VDISPLAY::changeDisplaySettings2(name.c_str(), mode.width, mode.height, mode.refresh_millihz, true);
    } else {
      result = VDISPLAY::changeDisplaySettings(name.c_str(), mode.width, mode.height, mode.refresh_millihz);
    }
    return result == ERROR_SUCCESS;
  }

  bool remove(const uuid_util::uuid_t &identity) {
    return VDISPLAY::removeVirtualDisplay(to_guid(identity));
  }

  std::optional<bool> get_hdr(const std::string &display_name) {
    const auto name = platf::from_utf8(display_name);
    return VDISPLAY::getDisplayHDRByName(name.c_str());
  }

  bool set_hdr(const std::string &display_name, bool enable) {
    const auto name = platf::from_utf8(display_name);
    return VDISPLAY::setDisplayHDRByName(name.c_str(), enable);
  }

  void set_render_adapter(const std::string &adapter_name) {
    VDISPLAY::setRenderAdapterByName(platf::from_utf8(adapter_name));
  }

  std::string output_name_for(const std::string &display_name) {
    return display_device::map_display_name(display_name);
  }

  std::optional<bool> any_display_active() {
    // libdisplaydevice answers this on Windows (video::allow_encoder_probing()).
    return std::nullopt;
  }
}  // namespace platf::vdisplay
