/**
 * @file src/platform/vdisplay_stub.cpp
 * @brief Virtual display backend for platforms without virtual display support.
 *
 * It reports status_e::unavailable, so common code never tries to create a display.
 */
#include "vdisplay.h"

namespace platf::vdisplay {
  status_e init() {
    return status_e::unavailable;
  }

  status_e status() {
    return status_e::unavailable;
  }

  void cleanup_stale() {
  }

  std::string create(const uuid_util::uuid_t &, const std::string &, const mode_t &, const std::string &) {
    return {};
  }

  bool set_mode(const std::string &, const mode_t &, bool) {
    return false;
  }

  bool remove(const uuid_util::uuid_t &) {
    return false;
  }

  std::optional<bool> get_hdr(const std::string &) {
    return std::nullopt;
  }

  bool set_hdr(const std::string &, bool) {
    return false;
  }

  void set_render_adapter(const std::string &) {
  }

  std::string output_name_for(const std::string &display_name) {
    return display_name;
  }
}  // namespace platf::vdisplay
