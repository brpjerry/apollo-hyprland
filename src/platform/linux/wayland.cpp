/**
 * @file src/platform/linux/wayland.cpp
 * @brief Definitions for Wayland capture.
 */
// standard includes
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>

// platform includes
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-util.h>
#include <xf86drm.h>

// lib includes
#include <lizardbyte/common/env.h>

// local includes
#include "graphics.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/platform/display_selection.h"
#include "src/round_robin.h"
#include "src/utility.h"
#include "src/video.h"
#include "wayland.h"

extern const wl_interface wl_output_interface;  ///< Wayland output interface.

using namespace std::literals;

// Disable warning for converting incompatible functions
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wpmf-conversions"

namespace wl {

  namespace {
    const gbm_bo_accessors_t gbm_bo_accessors {
      .get_plane_count = gbm_bo_get_plane_count,
      .get_fd_for_plane = gbm_bo_get_fd_for_plane,
      .get_stride_for_plane = gbm_bo_get_stride_for_plane,
      .get_offset = gbm_bo_get_offset,
      .get_modifier = gbm_bo_get_modifier,
    };

    int open_render_node(const char *path) {
      return open(path, O_RDWR | O_CLOEXEC);
    }

    const gbm_device_accessors_t gbm_device_accessors {
      .open_render_node = open_render_node,
      .create_device = gbm_create_device,
      .destroy_device = gbm_device_destroy,
    };
  }  // namespace

  gbm_device_t::~gbm_device_t() {
    reset();
  }

  bool gbm_device_t::init(const std::string &render_path, const gbm_device_accessors_t &device_accessors) {
    reset();
    accessors = device_accessors;

    drm_fd = device_accessors.open_render_node(render_path.c_str());
    if (drm_fd < 0) {
      BOOST_LOG(error) << "[wayland] Failed to open DRM render node: "sv << render_path;
      return false;
    }

    device = device_accessors.create_device(drm_fd);
    if (!device) {
      BOOST_LOG(error) << "[wayland] Failed to create GBM device"sv;
      reset();
      return false;
    }

    return true;
  }

  void gbm_device_t::reset() {
    if (device) {
      accessors.destroy_device(device);
      device = nullptr;
    }

    // gbm_device_destroy() does not close the descriptor the device was created from
    if (drm_fd >= 0) {
      close(drm_fd);
      drm_fd = -1;
    }
  }

  bool gbm_device_t::open(std::optional<std::uint64_t> wanted) {
    if (device) {
      return true;
    }

    std::string node;
    if (wanted) {
      drmDevice *drm_device = nullptr;
      if (drmGetDeviceFromDevId((dev_t) *wanted, 0, &drm_device) == 0) {
        if (drm_device->available_nodes & (1 << DRM_NODE_RENDER)) {
          node = drm_device->nodes[DRM_NODE_RENDER];
        } else if (drm_device->available_nodes & (1 << DRM_NODE_PRIMARY)) {
          node = drm_device->nodes[DRM_NODE_PRIMARY];
        }
        drmFreeDevice(&drm_device);
      } else {
        BOOST_LOG(warning) << "Couldn't find the DRM device the compositor asked for ("sv << major((dev_t) *wanted) << ':' << minor((dev_t) *wanted) << "); using the configured render node"sv;
      }
    }

    if (node.empty()) {
      // The configured render device (adapter_name), like the wlr-screencopy backend.
      node = platf::resolve_render_device();
    }

    if (!init(node, gbm_device_accessors)) {
      return false;
    }

    struct stat st {};
    if (fstat(drm_fd, &st) == 0) {
      dev = (std::uint64_t) st.st_rdev;
    }

    BOOST_LOG(debug) << "Allocating capture buffers on "sv << node;
    return true;
  }

  // Helper to call C++ method from wayland C callback
  template<class T, class Method, Method m, class... Params>
  static auto classCall(void *data, Params... params) -> decltype(((*reinterpret_cast<T *>(data)).*m)(params...)) {  // NOSONAR(cpp:S5008): Wayland listener callbacks require a void pointer for user data.
    return ((*reinterpret_cast<T *>(data)).*m)(params...);
  }

/**
 * @def CLASS_CALL(c, m)
 * @brief Macro for CLASS CALL.
 */
#define CLASS_CALL(c, m) classCall<c, decltype(&c::m), &c::m>

  // Define buffer params listener
  static const struct zwp_linux_buffer_params_v1_listener params_listener = {
    .created = dmabuf_t::buffer_params_created,
    .failed = dmabuf_t::buffer_params_failed
  };

  int display_t::init(const char *display_name) {
    std::string env_display_name;
    if (!display_name) {
      if (lizardbyte::common::get_env("WAYLAND_DISPLAY", env_display_name)) {
        display_name = env_display_name.c_str();
      }
    }

    if (!display_name) {
      BOOST_LOG(error) << "[wayland] Environment variable WAYLAND_DISPLAY has not been defined"sv;
      return -1;
    }

    display_internal.reset(wl_display_connect(display_name));
    if (!display_internal) {
      BOOST_LOG(error) << "[wayland] Couldn't connect to Wayland display: "sv << display_name;
      return -1;
    }

    BOOST_LOG(info) << "[wayland] Found display ["sv << display_name << ']';

    return 0;
  }

  void display_t::roundtrip() {
    wl_display_roundtrip(display_internal.get());
  }

  /**
   * @brief Waits up to the specified timeout to dispatch new events on the wl_display.
   * @param timeout Maximum wait, to the nanosecond. Zero or negative reads only what has arrived.
   * @return `true` if new events were dispatched, `false` if the timeout expired or the connection is broken
   *         (see broken()).
   */
  bool display_t::dispatch(std::chrono::nanoseconds timeout) {
    if (broken()) {
      return false;
    }

    // Check if any events are queued already. If not, flush
    // outgoing events, and prepare to wait for readability.
    if (wl_display_prepare_read(display_internal.get()) == 0) {
      wl_display_flush(display_internal.get());

      // Wait for an event to come in
      struct pollfd pfd = {};
      pfd.fd = wl_display_get_fd(display_internal.get());
      pfd.events = POLLIN;
      // ppoll() rather than poll(): a deadline of the capture pacer is kept to the microsecond, not the next millisecond.
      const auto ns = std::max(timeout, std::chrono::nanoseconds::zero()).count();
      const timespec ts {.tv_sec = static_cast<time_t>(ns / 1'000'000'000), .tv_nsec = static_cast<long>(ns % 1'000'000'000)};
      // A hangup or socket error is read too: libwayland then records the broken connection.
      if (ppoll(&pfd, 1, &ts, nullptr) == 1 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
        // Read the new event(s)
        wl_display_read_events(display_internal.get());
      } else {
        // We timed out, so unlock the queue now
        wl_display_cancel_read(display_internal.get());
        return false;
      }
    }

    // Dispatch any existing or new pending events
    return wl_display_dispatch_pending(display_internal.get()) >= 0;
  }

  bool display_t::broken() {
    return wl_display_get_error(display_internal.get()) != 0;
  }

  namespace {
    /// Highest wl_output version we handle (v4 adds the name and description events).
#ifdef WL_OUTPUT_NAME_SINCE_VERSION
    constexpr std::uint32_t max_wl_output_version = 4;
#else
    constexpr std::uint32_t max_wl_output_version = 2;
#endif
    /// Highest zxdg_output_manager_v1 version described by the protocol XML we generate code from.
    constexpr std::uint32_t max_xdg_output_manager_version = 3;
  }  // namespace

  wl_registry *display_t::registry() {
    return wl_display_get_registry(display_internal.get());
  }

  monitor_t::monitor_t(wl_output *output):
      output {output},
      wl_listener {
        .geometry = &CLASS_CALL(monitor_t, wl_geometry),
        .mode = &CLASS_CALL(monitor_t, wl_mode),
        .done = &CLASS_CALL(monitor_t, wl_done),
        .scale = &CLASS_CALL(monitor_t, wl_scale),
#ifdef WL_OUTPUT_NAME_SINCE_VERSION
        .name = &CLASS_CALL(monitor_t, wl_name),
        .description = &CLASS_CALL(monitor_t, wl_description),
#endif
      },
      xdg_listener {
        &CLASS_CALL(monitor_t, xdg_position),
        &CLASS_CALL(monitor_t, xdg_size),
        &CLASS_CALL(monitor_t, xdg_done),
        &CLASS_CALL(monitor_t, xdg_name),
        &CLASS_CALL(monitor_t, xdg_description)
      } {
  }

  void monitor_t::xdg_name(zxdg_output_v1 *, const char *name) {
    property_changed();
    // wl_output.name (v4) is the authoritative connector name; xdg_output.name is the fallback.
    if (!has_wl_name) {
      this->name = name;
    }

    BOOST_LOG(info) << "[wayland] xdg_output name: "sv << name;
  }

  void monitor_t::xdg_description(zxdg_output_v1 *, const char *description) {
    if (this->description.empty()) {
      this->description = description;
    }

    BOOST_LOG(info) << "[wayland] Found monitor: "sv << description;
  }

  void monitor_t::xdg_position(zxdg_output_v1 *, std::int32_t x, std::int32_t y) {
    property_changed();
    viewport.offset_x = x;
    viewport.offset_y = y;
    geometry.logical_x = x;
    geometry.logical_y = y;

    BOOST_LOG(info) << "[wayland] Offset: "sv << x << 'x' << y;
  }

  void monitor_t::xdg_size(zxdg_output_v1 *, std::int32_t width, std::int32_t height) {
    property_changed();
    viewport.logical_width = width;
    viewport.logical_height = height;
    geometry.logical_width = width;
    geometry.logical_height = height;

    BOOST_LOG(info) << "[wayland] Logical size: "sv << width << 'x' << height;
  }

  void monitor_t::wl_geometry(wl_output *, std::int32_t, std::int32_t, std::int32_t, std::int32_t, std::int32_t, const char *, const char *, std::int32_t transform) {  // NOSONAR(cpp:S107): libwayland's wl_output geometry callback signature.
    property_changed();
    viewport.transform = transform;
    geometry.transform = transform;

    BOOST_LOG(debug) << "[wayland] Transform: "sv << transform;
  }

  void monitor_t::wl_mode(
    wl_output *wl_output,
    std::uint32_t flags,
    std::int32_t width,
    std::int32_t height,
    std::int32_t refresh
  ) {
    BOOST_LOG(info) << "[wayland] Resolution: "sv << width << 'x' << height;

    if (!(flags & WL_OUTPUT_MODE_CURRENT)) {
      return;
    }

    property_changed();
    viewport.width = width;
    viewport.height = height;
    refresh_mhz = refresh;
    geometry.mode_width = width;
    geometry.mode_height = height;
  }

  void monitor_t::wl_scale(wl_output *, std::int32_t factor) {
    property_changed();
    geometry.integer_scale = factor;
  }

  void monitor_t::wl_name(wl_output *, const char *name) {
    this->name = name;
    has_wl_name = true;

    BOOST_LOG(debug) << "[wayland] wl_output name: "sv << name;
  }

  void monitor_t::wl_description(wl_output *, const char *description) {
    this->description = description;
  }

  void monitor_t::property_changed() {
    // wl_output.done arrived in version 2; xdg-output 1 and 2 have a done event of their own, and version 3 is
    // completed by wl_output.done.
    if (output && wl_proxy_get_version(reinterpret_cast<wl_proxy *>(output)) >= 2) {
      updating = true;
    }
  }

  void monitor_t::mark_removed() {
    removed = true;
    updating = false;
    if (xdg_output) {
      zxdg_output_v1_destroy(xdg_output);
      xdg_output = nullptr;
    }
  }

  void monitor_t::listen(zxdg_output_manager_v1 *output_manager) {
    if (removed || xdg_output) {
      return;
    }
    xdg_output = zxdg_output_manager_v1_get_xdg_output(output_manager, output);
    zxdg_output_v1_add_listener(xdg_output, &xdg_listener, this);
    wl_output_add_listener(output, &wl_listener, this);
  }

  namespace {
    void cm_supported_intent(void *, wp_color_manager_v1 *, std::uint32_t) {}

    void cm_supported_feature(void *data, wp_color_manager_v1 *, std::uint32_t feature) {
      static_cast<interface_t *>(data)->color_features.push_back(feature);
    }

    void cm_supported_tf_named(void *data, wp_color_manager_v1 *, std::uint32_t tf) {
      static_cast<interface_t *>(data)->color_tfs.push_back(tf);
    }

    void cm_supported_primaries_named(void *data, wp_color_manager_v1 *, std::uint32_t primaries) {
      static_cast<interface_t *>(data)->color_primaries.push_back(primaries);
    }

    void cm_done(void *, wp_color_manager_v1 *) {}

    const wp_color_manager_v1_listener color_manager_listener {
      .supported_intent = cm_supported_intent,
      .supported_feature = cm_supported_feature,
      .supported_tf_named = cm_supported_tf_named,
      .supported_primaries_named = cm_supported_primaries_named,
      .done = cm_done,
    };
  }  // namespace

  interface_t::interface_t() noexcept
      :
      screencopy_manager {nullptr},
      dmabuf_interface {nullptr},
      output_manager {nullptr},
      listener {
        &CLASS_CALL(interface_t, add_interface),
        &CLASS_CALL(interface_t, del_interface)
      },
      dmabuf_listener {
        &CLASS_CALL(interface_t, dmabuf_format),
        &CLASS_CALL(interface_t, dmabuf_modifier)
      } {
  }

  void interface_t::listen(wl_registry *registry) {
    wl_registry_add_listener(registry, &listener, this);
  }

  void interface_t::dmabuf_format(zwp_linux_dmabuf_v1 *zwp_linux_dmabuf, uint32_t format) {
    // Modifier events supply the explicit format/modifier pairs used for allocation.
  }

  void interface_t::dmabuf_modifier(zwp_linux_dmabuf_v1 *zwp_linux_dmabuf, uint32_t format, uint32_t modifier_hi, uint32_t modifier_lo) {
    const auto modifier = (static_cast<std::uint64_t>(modifier_hi) << 32) | modifier_lo;
    if (modifier != DRM_FORMAT_MOD_INVALID) {
      supported_modifiers[format].push_back(modifier);
    }
  }

  void interface_t::add_interface(
    wl_registry *registry,
    std::uint32_t id,
    const char *interface_name,
    std::uint32_t version
  ) {
    BOOST_LOG(debug) << "[wayland] Available interface: "sv << interface_name << '(' << id << ") version "sv << version;

    if (!std::strcmp(interface_name, wl_output_interface.name)) {
      BOOST_LOG(info) << "[wayland] Found interface: "sv << interface_name << '(' << id << ") version "sv << version;
      monitors.emplace_back(
        std::make_unique<monitor_t>(
          (wl_output *) wl_registry_bind(registry, id, &wl_output_interface, std::min(version, max_wl_output_version))
        )
      );
      monitors.back()->global_name = id;
      // An output added after setup (hotplug, a new virtual display) is described right away, so that the layout
      // it extends is known. During setup the manager may not be bound yet; enumerate_outputs() listens then.
      if (output_manager) {
        monitors.back()->listen(output_manager);
      }
    } else if (!std::strcmp(interface_name, zxdg_output_manager_v1_interface.name)) {
      BOOST_LOG(info) << "[wayland] Found interface: "sv << interface_name << '(' << id << ") version "sv << version;
      output_manager = (zxdg_output_manager_v1 *) wl_registry_bind(registry, id, &zxdg_output_manager_v1_interface, std::min(version, max_xdg_output_manager_version));

      interface[XDG_OUTPUT] = true;
    } else if (!std::strcmp(interface_name, zwlr_screencopy_manager_v1_interface.name)) {
      BOOST_LOG(info) << "[wayland] Found interface: "sv << interface_name << '(' << id << ") version "sv << version;
      screencopy_manager = (zwlr_screencopy_manager_v1 *) wl_registry_bind(registry, id, &zwlr_screencopy_manager_v1_interface, version);
      screencopy_version = version;

      interface[WLR_EXPORT_DMABUF] = true;
    } else if (!std::strcmp(interface_name, zwp_linux_dmabuf_v1_interface.name)) {
      BOOST_LOG(info) << "[wayland] Found interface: "sv << interface_name << '(' << id << ") version "sv << version;
      dmabuf_interface = (zwp_linux_dmabuf_v1 *) wl_registry_bind(registry, id, &zwp_linux_dmabuf_v1_interface, std::min(version, 3u));
      zwp_linux_dmabuf_v1_add_listener(dmabuf_interface, &dmabuf_listener, this);

      interface[LINUX_DMABUF] = true;
    } else if (!std::strcmp(interface_name, wl_shm_interface.name)) {
      BOOST_LOG(info) << "[wayland] Found interface: "sv << interface_name << '(' << id << ") version "sv << version;
      shm_interface = (wl_shm *) wl_registry_bind(registry, id, &wl_shm_interface, 1);

      interface[SHM] = true;
    } else if (!std::strcmp(interface_name, ext_image_copy_capture_manager_v1_interface.name)) {
      BOOST_LOG(info) << "[wayland] Found interface: "sv << interface_name << '(' << id << ") version "sv << version;
      ext_copy_manager = (ext_image_copy_capture_manager_v1 *) wl_registry_bind(registry, id, &ext_image_copy_capture_manager_v1_interface, 1);

      interface[EXT_IMAGE_COPY_CAPTURE] = true;
    } else if (!std::strcmp(interface_name, ext_output_image_capture_source_manager_v1_interface.name)) {
      BOOST_LOG(info) << "[wayland] Found interface: "sv << interface_name << '(' << id << ") version "sv << version;
      ext_output_source_manager = (ext_output_image_capture_source_manager_v1 *) wl_registry_bind(registry, id, &ext_output_image_capture_source_manager_v1_interface, 1);

      interface[EXT_OUTPUT_CAPTURE_SOURCE] = true;
    } else if (bind_color_manager && !std::strcmp(interface_name, wp_color_manager_v1_interface.name)) {
      BOOST_LOG(debug) << "[wayland] Found interface: "sv << interface_name << '(' << id << ") version "sv << version;
      color_manager = (wp_color_manager_v1 *) wl_registry_bind(registry, id, &wp_color_manager_v1_interface, 1);
      wp_color_manager_v1_add_listener(color_manager, &color_manager_listener, this);

      interface[COLOR_MANAGER] = true;
    }
  }

  void interface_t::del_interface(wl_registry *registry, uint32_t id) {
    BOOST_LOG(info) << "[wayland] Delete: "sv << id;

    for (auto &monitor : monitors) {
      if (monitor->global_name == id && !monitor->removed) {
        monitor->mark_removed();
        BOOST_LOG(info) << "[wayland] Output removed: "sv << monitor->name;
      }
    }
  }

  std::vector<output_geometry_t> layout_geometries(const interface_t &interface) {
    std::vector<output_geometry_t> geometries;
    geometries.reserve(interface.monitors.size());
    for (const auto &m : interface.monitors) {
      // An output added a moment ago has its mode before its logical size: until then its place is unknown.
      const bool described = m->geometry.logical_width > 0 || !m->listening();
      if (!m->removed && described) {
        geometries.emplace_back(m->geometry);
      }
    }
    return geometries;
  }

  namespace {
    /**
     * @brief Bounds of the layout of the outputs that are still there.
     */
    layout_bounds_t current_layout(const interface_t &interface) {
      return layout_bounds(layout_geometries(interface));
    }
  }  // namespace

  output_watch_t watch_output(const interface_t &interface, const monitor_t &monitor) {
    return {
      .monitor = &monitor,
      .geometry = monitor.geometry,
      .refresh_mhz = monitor.refresh_mhz,
      .layout = current_layout(interface),
    };
  }

  std::optional<std::string_view> output_change(const output_watch_t &watch, const interface_t &interface) {
    if (!watch.monitor) {
      return std::nullopt;
    }
    const auto &monitor = *watch.monitor;
    const auto &before = watch.geometry;
    const auto &now = monitor.geometry;
    if (monitor.removed) {
      return "the output was removed"sv;
    }
    // Half of an update says nothing yet: a new mode may come with a new logical size, a resize with a move.
    if (std::ranges::any_of(interface.monitors, [](const auto &m) {
          return !m->removed && m->updating;
        })) {
      return std::nullopt;
    }
    if (now.mode_width != before.mode_width || now.mode_height != before.mode_height || monitor.refresh_mhz != watch.refresh_mhz) {
      return "the output's mode changed"sv;
    }
    if (now.integer_scale != before.integer_scale || now.logical_width != before.logical_width || now.logical_height != before.logical_height) {
      return "the output's scale changed"sv;
    }
    if (now.transform != before.transform) {
      return "the output's transform changed"sv;
    }
    if (now.logical_x != before.logical_x || now.logical_y != before.logical_y) {
      return "the output moved"sv;
    }
    if (current_layout(interface) != watch.layout) {
      return "the output layout changed"sv;
    }
    return std::nullopt;
  }

  std::chrono::nanoseconds capture_interval(const ::video::config_t &config, std::int32_t refresh_mhz) {
    if (config.framerateX100 <= 0) {
      if (auto matched = refresh_matched_rate(config.framerate, refresh_mhz)) {
        BOOST_LOG(info) << "Capturing at "sv << static_cast<double>(matched->num) / static_cast<double>(matched->den) << " fps instead of "sv
                        << config.framerate << " to match the output's refresh rate of "sv << refresh_mhz / 1000.0 << " Hz"sv;
        return frame_interval(*matched);
      }
    }
    return ::video::capture_frame_interval(config);
  }

  // Initialize GBM
  bool dmabuf_t::init_gbm() {
    if (gbm) {
      return true;
    }

    return gbm.init(platf::resolve_render_device(), gbm_device_accessors);
  }

  // Cleanup GBM
  void dmabuf_t::cleanup_gbm() {
    if (current_bo) {
      gbm_bo_destroy(current_bo);
      current_bo = nullptr;
    }

    if (current_wl_buffer) {
      wl_buffer_destroy(current_wl_buffer);
      current_wl_buffer = nullptr;
    }
  }

  std::map<std::uint32_t, std::vector<std::uint64_t>> intersect_modifiers(
    const std::map<std::uint32_t, std::vector<std::uint64_t>> &compositor_modifiers,
    const std::map<std::uint32_t, std::vector<std::uint64_t>> &encoder_modifiers
  ) {
    std::map<std::uint32_t, std::vector<std::uint64_t>> result;

    for (const auto &[format, comp_mods] : compositor_modifiers) {
      auto it = encoder_modifiers.find(format);
      if (it == encoder_modifiers.end()) {
        continue;
      }

      const auto &enc_mods = it->second;
      std::vector<std::uint64_t> common;

      for (auto mod : comp_mods) {
        if (std::find(enc_mods.begin(), enc_mods.end(), mod) != enc_mods.end()) {
          common.push_back(mod);
        }
      }

      if (!common.empty()) {
        BOOST_LOG(debug) << "[wayland] Format 0x"sv << std::hex << format << std::dec
                         << " has "sv << common.size() << " common modifiers"sv;
        result[format] = std::move(common);
      }
    }

    return result;
  }

  bool should_attempt_linear_copy(bool prefer_linear_copy, const std::vector<std::uint64_t> &modifiers) {
    return prefer_linear_copy && std::find(modifiers.begin(), modifiers.end(), DRM_FORMAT_MOD_LINEAR) != modifiers.end();
  }

  const std::vector<std::uint64_t> *find_advertised_modifiers(const std::map<std::uint32_t, std::vector<std::uint64_t>> *modifiers, std::uint32_t format) {
    if (modifiers) {
      if (const auto it = modifiers->find(format); it != modifiers->end() && !it->second.empty()) {
        return &it->second;
      }
    }
    return nullptr;
  }

  // Destroy the per-frame SHM Wayland objects, keeping the memfd mapping
  void dmabuf_t::cleanup_shm() {
    if (shm.buffer) {
      wl_buffer_destroy(shm.buffer);
      shm.buffer = nullptr;
    }

    if (shm.pool) {
      wl_shm_pool_destroy(shm.pool);
      shm.pool = nullptr;
    }
  }

  // Release the memfd mapping as well
  void dmabuf_t::destroy_shm() {
    cleanup_shm();

    if (shm.mapping && shm.size > 0) {
      munmap(shm.mapping, shm.size);
      shm.mapping = nullptr;
      shm.size = 0;
    }

    if (shm.fd >= 0) {
      close(shm.fd);
      shm.fd = -1;
    }
  }

  bool dmabuf_t::init_shm(std::size_t size) {
    if (size == shm.size) {
      return true;
    }

    destroy_shm();

    shm.fd = memfd_create("sunshine-shm", 0);
    if (shm.fd < 0) {
      BOOST_LOG(error) << "[wayland] memfd_create failed: "sv << strerror(errno);
      return false;
    }

    if (ftruncate(shm.fd, size) < 0) {
      BOOST_LOG(error) << "[wayland] ftruncate failed: "sv << strerror(errno);
      close(shm.fd);
      shm.fd = -1;
      return false;
    }

    shm.mapping = static_cast<std::uint8_t *>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm.fd, 0));
    if (shm.mapping == MAP_FAILED) {
      BOOST_LOG(error) << "[wayland] mmap failed: "sv << strerror(errno);
      shm.mapping = nullptr;
      close(shm.fd);
      shm.fd = -1;
      return false;
    }

    shm.size = size;
    return true;
  }

  std::optional<std::size_t> shm_buffer_size(std::uint32_t format, std::uint32_t width, std::uint32_t height, std::uint32_t stride) {
    std::uint32_t bpp;
    switch (format) {
      case WL_SHM_FORMAT_XRGB8888:
      case WL_SHM_FORMAT_ARGB8888:
      case WL_SHM_FORMAT_XBGR8888:
      case WL_SHM_FORMAT_ABGR8888:
        bpp = 4;
        break;
      case WL_SHM_FORMAT_RGB888:
      case WL_SHM_FORMAT_BGR888:
        bpp = 3;
        break;
      default:
        return std::nullopt;
    }

    if (constexpr auto limit = std::numeric_limits<std::int32_t>::max(); !width || !height || !stride || width > limit || height > limit || stride > limit || width > stride / bpp || height > limit / stride) {
      return std::nullopt;
    }

    return static_cast<std::size_t>(stride) * height;
  }

  // SHM creation helper
  void dmabuf_t::create_and_copy_shm(zwlr_screencopy_frame_v1 *frame) {
    auto needed = shm_buffer_size(shm_info.format, shm_info.width, shm_info.height, shm_info.stride);

    if (!needed || !init_shm(*needed)) {
      zwlr_screencopy_frame_v1_destroy(frame);
      status = REINIT;
      return;
    }

    // The compositor may hold a reference to the previous buffer, so hand it a fresh one
    cleanup_shm();

    shm.pool = wl_shm_create_pool(shm.interface, shm.fd, shm.size);
    if (!shm.pool) {
      BOOST_LOG(error) << "[wayland] Failed to create SHM pool"sv;
      zwlr_screencopy_frame_v1_destroy(frame);
      status = REINIT;
      return;
    }

    shm.buffer = wl_shm_pool_create_buffer(
      shm.pool,
      0,
      shm_info.width,
      shm_info.height,
      shm_info.stride,
      shm_info.format
    );
    if (!shm.buffer) {
      BOOST_LOG(error) << "[wayland] Failed to create SHM buffer"sv;
      cleanup_shm();
      zwlr_screencopy_frame_v1_destroy(frame);
      status = REINIT;
      return;
    }

    shm.active = true;

    static std::once_flag shm_format_log_flag;
    std::call_once(shm_format_log_flag, [&] {
      BOOST_LOG(info) << "[wayland] SHM capture: "sv
                      << shm_info.width << "x"sv << shm_info.height
                      << " stride="sv << shm_info.stride
                      << " format=0x"sv << std::hex << shm_info.format << std::dec;
    });

    if (with_damage) {
      zwlr_screencopy_frame_v1_copy_with_damage(frame, shm.buffer);
    } else {
      zwlr_screencopy_frame_v1_copy(frame, shm.buffer);
    }
    pending_copy = frame;
  }

  dmabuf_t::dmabuf_t():
      current_frame {&frames[0]},
      listener {
        &CLASS_CALL(dmabuf_t, buffer),
        &CLASS_CALL(dmabuf_t, flags),
        &CLASS_CALL(dmabuf_t, ready),
        &CLASS_CALL(dmabuf_t, failed),
        &CLASS_CALL(dmabuf_t, damage),
        &CLASS_CALL(dmabuf_t, linux_dmabuf),
        &CLASS_CALL(dmabuf_t, buffer_done),
      } {
  }

  // Start capture
  void dmabuf_t::listen(
    zwlr_screencopy_manager_v1 *screencopy_manager,
    zwp_linux_dmabuf_v1 *dmabuf_interface_in,
    const std::map<std::uint32_t, std::vector<std::uint64_t>> *compositor_formats_in,
    wl_shm *shm_interface_in,
    wl_output *output,
    bool blend_cursor,
    const std::map<std::uint32_t, std::vector<std::uint64_t>> *encoder_formats_in,
    bool prefer_linear_copy_in
  ) {
    capture_context = {screencopy_manager, output, blend_cursor};
    dmabuf_interface = dmabuf_interface_in;
    supported_modifiers = compositor_formats_in;
    encoder_modifiers = encoder_formats_in;
    prefer_linear_copy = prefer_linear_copy_in;
    shm.interface = shm_interface_in;
    // Reset state
    shm_info.supported = false;
    dmabuf_info.supported = false;

    // Create new frame
    auto frame = zwlr_screencopy_manager_v1_capture_output(
      screencopy_manager,
      blend_cursor ? 1 : 0,
      output
    );

    // Store frame data pointer for callbacks
    zwlr_screencopy_frame_v1_set_user_data(frame, this);

    // Add listener
    zwlr_screencopy_frame_v1_add_listener(frame, &listener, this);

    status = WAITING;
  }

  dmabuf_t::~dmabuf_t() {
    cleanup_gbm();
    destroy_shm();

    for (auto &frame : frames) {
      frame.destroy();
    }

    gbm.reset();
  }

  // Buffer format callback
  void dmabuf_t::buffer(
    zwlr_screencopy_frame_v1 *frame,
    uint32_t format,
    uint32_t width,
    uint32_t height,
    uint32_t stride
  ) {
    shm_info.supported = true;
    shm_info.format = format;
    shm_info.width = width;
    shm_info.height = height;
    shm_info.stride = stride;

    // buffer_done (and linux_dmabuf) came with version 3. Before that, this SHM event is the only buffer description,
    // so request the copy now or the frame is never filled.
    if (zwlr_screencopy_frame_v1_get_version(frame) < ZWLR_SCREENCOPY_FRAME_V1_BUFFER_DONE_SINCE_VERSION) {
      buffer_done(frame);
    }
  }

  // DMA-BUF format callback
  void dmabuf_t::linux_dmabuf(
    zwlr_screencopy_frame_v1 *frame,
    std::uint32_t format,
    std::uint32_t width,
    std::uint32_t height
  ) {
    dmabuf_info.supported = true;
    dmabuf_info.format = format;
    dmabuf_info.width = width;
    dmabuf_info.height = height;
  }

  // Flags callback
  void dmabuf_t::flags(zwlr_screencopy_frame_v1 *frame, std::uint32_t flags) {
    y_invert = flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
    BOOST_LOG(verbose) << "Frame flags: "sv << flags << (y_invert ? " (y_invert)" : "");
  }

  std::optional<std::uint32_t> export_gbm_bo_planes(gbm_bo *bo, frame_t &frame, const gbm_bo_accessors_t &accessors) {
    frame.destroy();

    const auto plane_count = accessors.get_plane_count(bo);
    if (plane_count <= 0 || plane_count > static_cast<int>(std::size(frame.sd.fds))) {
      BOOST_LOG(error) << "[wayland] GBM buffer has unsupported plane count ["sv << plane_count << ']';
      return std::nullopt;
    }

    frame.sd.modifier = accessors.get_modifier(bo);
    for (auto plane = 0; plane < plane_count; ++plane) {
      frame.sd.fds[plane] = accessors.get_fd_for_plane(bo, plane);
      if (frame.sd.fds[plane] < 0) {
        BOOST_LOG(error) << "[wayland] Failed to export DMA-BUF plane ["sv << plane << ']';
        frame.destroy();
        return std::nullopt;
      }

      frame.sd.pitches[plane] = accessors.get_stride_for_plane(bo, plane);
      frame.sd.offsets[plane] = accessors.get_offset(bo, plane);
    }

    return static_cast<std::uint32_t>(plane_count);
  }

  // DMA-BUF creation helper — returns false when the caller should fall back to SHM
  bool dmabuf_t::create_and_copy_dmabuf(zwlr_screencopy_frame_v1 *frame) {
    if (!init_gbm()) {
      BOOST_LOG(error) << "Failed to initialize GBM"sv;
      return false;
    }

    // Create GBM buffer - prefer encoder-intersected modifiers when available
    const std::map<std::uint32_t, std::vector<std::uint64_t>> *modifiers_to_use = encoder_modifiers ? encoder_modifiers : supported_modifiers;
    if (const auto *advertised = find_advertised_modifiers(modifiers_to_use, dmabuf_info.format); advertised) {
      if (should_attempt_linear_copy(prefer_linear_copy, *advertised)) {
        // Cross-GPU encode (e.g. NVENC on a headless NVIDIA GPU while the compositor
        // renders on AMD/Intel) cannot import vendor-tiled modifiers, so request a
        // linear buffer the encoder is guaranteed to import instead. Note: no
        // GBM_BO_USE_LINEAR here, Mesa rejects that flag with modifiers2.
        constexpr std::uint64_t linear_modifier = DRM_FORMAT_MOD_LINEAR;
        current_bo = gbm_bo_create_with_modifiers2(gbm.get(), dmabuf_info.width, dmabuf_info.height, dmabuf_info.format, &linear_modifier, 1, GBM_BO_USE_RENDERING);
        if (!current_bo) {
          BOOST_LOG(debug) << "Linear screencopy buffer unavailable, falling back to compositor modifiers"sv;
        }
      }
      if (!current_bo) {
        current_bo = gbm_bo_create_with_modifiers2(gbm.get(), dmabuf_info.width, dmabuf_info.height, dmabuf_info.format, advertised->data(), advertised->size(), GBM_BO_USE_RENDERING);
      }
    }

    if (!current_bo) {
      current_bo = gbm_bo_create(gbm.get(), dmabuf_info.width, dmabuf_info.height, dmabuf_info.format, GBM_BO_USE_RENDERING);
    }

    if (!current_bo) {
      BOOST_LOG(error) << "Failed to create GBM buffer"sv;
      return false;
    }

    // Export every memory plane into the surface descriptor
    auto next_frame = get_next_frame();
    const auto plane_count = export_gbm_bo_planes(current_bo, *next_frame, gbm_bo_accessors);
    if (!plane_count.has_value()) {
      gbm_bo_destroy(current_bo);
      current_bo = nullptr;
      return false;
    }

    // Create linux-dmabuf buffer
    auto params = zwp_linux_dmabuf_v1_create_params(dmabuf_interface);
    for (std::uint32_t plane = 0; plane < *plane_count; ++plane) {
      zwp_linux_buffer_params_v1_add(
        params,
        next_frame->sd.fds[plane],
        plane,
        next_frame->sd.offsets[plane],
        next_frame->sd.pitches[plane],
        next_frame->sd.modifier >> 32,
        next_frame->sd.modifier & 0xffffffff
      );
    }

    // Add listener for buffer creation
    zwp_linux_buffer_params_v1_add_listener(params, &params_listener, frame);

    // Create Wayland buffer (async - callback will handle copy)
    zwp_linux_buffer_params_v1_create(params, dmabuf_info.width, dmabuf_info.height, dmabuf_info.format, 0);

    return true;
  }

  // Buffer done callback - time to create buffer
  void dmabuf_t::buffer_done(zwlr_screencopy_frame_v1 *frame) {
    auto next_frame = get_next_frame();
    shm.active = false;

    // Prefer DMA-BUF if supported
    if (dmabuf_info.supported && dmabuf_interface && allow_dmabuf && !gbm_failed) {
      // Store format info first
      next_frame->sd.fourcc = dmabuf_info.format;
      next_frame->sd.width = dmabuf_info.width;
      next_frame->sd.height = dmabuf_info.height;

      // Create and start copy
      if (create_and_copy_dmabuf(frame)) {
        return;
      }

      // GBM allocation is not going to start working later on, so stop retrying it
      gbm_failed = true;
      cleanup_gbm();

      if (shm_info.supported && shm.interface) {
        BOOST_LOG(warning) << "[wayland] DMA-BUF capture failed, falling back to SHM"sv;
        create_and_copy_shm(frame);
        return;
      }

      BOOST_LOG(error) << "[wayland] DMA-BUF capture failed and no SHM fallback is available"sv;
      zwlr_screencopy_frame_v1_destroy(frame);
      status = REINIT;
    } else if (shm_info.supported && shm.interface) {
      create_and_copy_shm(frame);
    } else {
      BOOST_LOG(error) << "[wayland] No supported buffer types"sv;
      zwlr_screencopy_frame_v1_destroy(frame);
      status = REINIT;
    }
  }

  // Buffer params created callback
  void dmabuf_t::buffer_params_created(
    void *data,
    struct zwp_linux_buffer_params_v1 *params,
    struct wl_buffer *buffer
  ) {
    auto frame = static_cast<zwlr_screencopy_frame_v1 *>(data);
    auto self = static_cast<dmabuf_t *>(zwlr_screencopy_frame_v1_get_user_data(frame));

    // Store for cleanup
    self->current_wl_buffer = buffer;

    // Start the actual copy
    zwp_linux_buffer_params_v1_destroy(params);
    if (self->with_damage) {
      zwlr_screencopy_frame_v1_copy_with_damage(frame, buffer);
    } else {
      zwlr_screencopy_frame_v1_copy(frame, buffer);
    }
    self->pending_copy = frame;
  }

  // Buffer params failed callback
  void dmabuf_t::buffer_params_failed(
    void *data,
    struct zwp_linux_buffer_params_v1 *params
  ) {
    auto frame = static_cast<zwlr_screencopy_frame_v1 *>(data);
    auto self = static_cast<dmabuf_t *>(zwlr_screencopy_frame_v1_get_user_data(frame));

    BOOST_LOG(error) << "[wayland] Failed to create buffer from params"sv;
    self->cleanup_gbm();
    self->get_next_frame()->destroy();

    // The compositor rejected our DMA-BUF; retrying it would loop, so take the SHM path
    self->gbm_failed = true;

    zwp_linux_buffer_params_v1_destroy(params);
    if (self->shm_info.supported && self->shm.interface) {
      self->create_and_copy_shm(frame);
      return;
    }

    zwlr_screencopy_frame_v1_destroy(frame);
    self->status = REINIT;
  }

  // Ready callback
  void dmabuf_t::ready(
    zwlr_screencopy_frame_v1 *frame,
    std::uint32_t tv_sec_hi,
    std::uint32_t tv_sec_lo,
    std::uint32_t tv_nsec
  ) {
    // Frame is ready for use, GBM buffer now contains screen content
    current_frame->destroy();
    current_frame = get_next_frame();

    std::uint64_t sec = (std::uint64_t(tv_sec_hi) << 32) | tv_sec_lo;
    auto ready_ts = std::chrono::seconds(sec) + std::chrono::nanoseconds(tv_nsec);
    current_frame->frame_timestamp = std::chrono::steady_clock::time_point {
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(ready_ts)
    };

    if (shm.active) {
      // wlr_t::snapshot() checks these against the configured resolution
      current_frame->sd.width = shm_info.width;
      current_frame->sd.height = shm_info.height;

      // Copy out — the compositor writes the next frame into the same mapping
      const auto *src = shm.mapping;
      current_frame->shm_data.assign(src, src + static_cast<std::size_t>(shm_info.stride) * shm_info.height);
      current_frame->shm_stride = shm_info.stride;
      current_frame->shm_format = shm_info.format;
      current_frame->is_shm = true;
      current_frame->shm_y_invert = y_invert;

      cleanup_shm();
    } else {
      current_frame->is_shm = false;
      // The encoder flips upside-down DMA-BUFs (egl::img_descriptor_t::y_invert).
      current_frame->shm_y_invert = y_invert;

      // Keep the GBM buffer alive but destroy the Wayland objects
      if (current_wl_buffer) {
        wl_buffer_destroy(current_wl_buffer);
        current_wl_buffer = nullptr;
      }

      cleanup_gbm();
    }

    zwlr_screencopy_frame_v1_destroy(frame);
    pending_copy = nullptr;
    status = READY;
  }

  // Failed callback
  void dmabuf_t::failed(zwlr_screencopy_frame_v1 *frame) {
    BOOST_LOG(error) << "[wayland] Frame capture failed"sv;

    const bool retry_shm = pending_copy == frame && current_wl_buffer && !shm.active && !gbm_failed && shm_info.supported && shm.interface;

    // Clean up resources
    cleanup_gbm();
    cleanup_shm();
    auto next_frame = get_next_frame();
    next_frame->destroy();

    zwlr_screencopy_frame_v1_destroy(frame);
    pending_copy = nullptr;
    if (retry_shm) {
      // A failed screencopy is terminal. Retry with a new request, immediately,
      // so an unchanged output does not leave the SHM fallback waiting for damage.
      gbm_failed = true;
      with_damage = false;
      BOOST_LOG(warning) << "[wayland] DMA-BUF copy failed, retrying capture with SHM"sv;
      listen(capture_context.manager, dmabuf_interface, supported_modifiers, shm.interface, capture_context.output, capture_context.blend_cursor, encoder_modifiers, prefer_linear_copy);
      return;
    }
    status = REINIT;
  }

  bool dmabuf_t::cancel() {
    if (!pending_copy) {
      return false;
    }

    zwlr_screencopy_frame_v1_destroy(pending_copy);
    pending_copy = nullptr;

    // Same as a failed copy: the buffer the compositor was to fill is not needed anymore
    cleanup_gbm();
    cleanup_shm();
    get_next_frame()->destroy();

    status = READY;
    return true;
  }

  // Only called if using zwlr_screencopy_frame_v1_copy_with_damage()
  void dmabuf_t::damage(
    zwlr_screencopy_frame_v1 *frame,
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t width,
    std::uint32_t height
  ) {
    // Capture and encoding consume the whole buffer, so no damage rectangles are tracked.
  };

  void frame_t::destroy() {
    for (auto x = 0; x < 4; ++x) {
      if (sd.fds[x] >= 0) {
        close(sd.fds[x]);

        sd.fds[x] = -1;
      }
    }
  }

  frame_t::frame_t() {
    // File descriptors aren't open
    sd.fds.fill(-1);
  };

  /**
   * @brief Refresh the monitor list reported by the display server.
   */
  std::vector<std::unique_ptr<monitor_t>> monitors(const char *display_name) {
    display_t display;

    if (display.init(display_name)) {
      return {};
    }

    interface_t interface;
    interface.listen(display.registry());

    display.roundtrip();

    if (!interface[interface_t::XDG_OUTPUT]) {
      BOOST_LOG(error) << "[wayland] Missing Wayland wire XDG_OUTPUT"sv;
      return {};
    }

    for (auto &monitor : interface.monitors) {
      monitor->listen(interface.output_manager);
    }

    display.roundtrip();

    return std::move(interface.monitors);
  }

  capture_buffer_t::capture_buffer_t() {
    sd.fds.fill(-1);
  }

  capture_buffer_t::~capture_buffer_t() {
    if (buffer) {
      wl_buffer_destroy(buffer);
    }
    for (auto &fd : sd.fds) {
      if (fd >= 0) {
        close(fd);
        fd = -1;
      }
    }
    if (bo) {
      gbm_bo_destroy(bo);
    }
    if (data) {
      munmap(data, size);
    }
  }

  namespace {
    struct params_result_t {
      wl_buffer *buffer {nullptr};
      bool done {false};

      static void created(void *data, zwp_linux_buffer_params_v1 *, wl_buffer *buffer) {
        auto self = static_cast<params_result_t *>(data);
        self->buffer = buffer;
        self->done = true;
      }

      static void failed(void *data, zwp_linux_buffer_params_v1 *) {
        static_cast<params_result_t *>(data)->done = true;
      }
    };

    const zwp_linux_buffer_params_v1_listener params_result_listener {
      .created = params_result_t::created,
      .failed = params_result_t::failed,
    };
  }  // namespace

  std::unique_ptr<capture_buffer_t> capture_buffer_t::make_dmabuf(display_t &display, zwp_linux_dmabuf_v1 *linux_dmabuf, struct gbm_device *device, const buffer_spec_t &spec) {
    if (!linux_dmabuf || !device || spec.shm) {
      return nullptr;
    }

    std::unique_ptr<capture_buffer_t> result {new capture_buffer_t()};
    result->spec = spec;

    if (!spec.modifiers.empty()) {
      result->bo = gbm_bo_create_with_modifiers2(device, spec.width, spec.height, spec.format, spec.modifiers.data(), spec.modifiers.size(), GBM_BO_USE_RENDERING);
      if (!result->bo) {
        BOOST_LOG(debug) << "gbm_bo_create_with_modifiers2 failed for format "sv << util::hex(spec.format).to_string_view() << "; trying without modifiers"sv;
      }
    }
    if (!result->bo && spec.implicit_modifier) {
      result->bo = gbm_bo_create(device, spec.width, spec.height, spec.format, GBM_BO_USE_RENDERING);
    }
    if (!result->bo) {
      BOOST_LOG(error) << "Failed to allocate a "sv << spec.width << 'x' << spec.height << " capture buffer with GBM"sv;
      return nullptr;
    }

    const auto planes = std::clamp(gbm_bo_get_plane_count(result->bo), 1, 4);
    const auto modifier = gbm_bo_get_modifier(result->bo);

    auto &sd = result->sd;
    sd.width = (int) spec.width;
    sd.height = (int) spec.height;
    sd.fourcc = spec.format;
    sd.modifier = modifier;

    auto params = zwp_linux_dmabuf_v1_create_params(linux_dmabuf);
    for (int plane = 0; plane < planes; ++plane) {
      sd.fds[plane] = planes == 1 ? gbm_bo_get_fd(result->bo) : gbm_bo_get_fd_for_plane(result->bo, plane);
      if (sd.fds[plane] < 0) {
        BOOST_LOG(error) << "Failed to export capture buffer plane "sv << plane;
        zwp_linux_buffer_params_v1_destroy(params);
        return nullptr;
      }
      sd.pitches[plane] = gbm_bo_get_stride_for_plane(result->bo, plane);
      sd.offsets[plane] = gbm_bo_get_offset(result->bo, plane);
      zwp_linux_buffer_params_v1_add(params, sd.fds[plane], plane, sd.offsets[plane], sd.pitches[plane], modifier >> 32, modifier & 0xffffffff);
    }

    params_result_t created;
    zwp_linux_buffer_params_v1_add_listener(params, &params_result_listener, &created);
    zwp_linux_buffer_params_v1_create(params, spec.width, spec.height, spec.format, 0);

    // The compositor answers with created or failed before the roundtrip's done event.
    for (int x = 0; x < 2 && !created.done; ++x) {
      display.roundtrip();
    }
    zwp_linux_buffer_params_v1_destroy(params);

    if (!created.buffer) {
      BOOST_LOG(error) << "The compositor rejected the capture buffer (format "sv << util::hex(spec.format).to_string_view() << ", modifier "sv << util::hex(modifier).to_string_view() << ')';
      return nullptr;
    }

    result->buffer = created.buffer;
    return result;
  }

  std::unique_ptr<capture_buffer_t> capture_buffer_t::make_shm(wl_shm *shm, const buffer_spec_t &spec) {
    if (!shm || !spec.shm) {
      return nullptr;
    }

    std::unique_ptr<capture_buffer_t> result {new capture_buffer_t()};
    result->spec = spec;
    result->stride = spec.stride ? spec.stride : spec.width * 4;
    result->size = (std::size_t) result->stride * spec.height;

    int fd = memfd_create("apollo-capture", MFD_CLOEXEC);
    if (fd < 0) {
      BOOST_LOG(error) << "memfd_create failed: "sv << std::strerror(errno);
      return nullptr;
    }
    if (ftruncate(fd, (off_t) result->size) < 0) {
      BOOST_LOG(error) << "ftruncate failed: "sv << std::strerror(errno);
      close(fd);
      return nullptr;
    }

    auto mapping = mmap(nullptr, result->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
      BOOST_LOG(error) << "mmap failed: "sv << std::strerror(errno);
      close(fd);
      return nullptr;
    }
    result->data = static_cast<std::uint8_t *>(mapping);

    auto pool = wl_shm_create_pool(shm, fd, (std::int32_t) result->size);
    result->buffer = wl_shm_pool_create_buffer(pool, 0, (std::int32_t) spec.width, (std::int32_t) spec.height, (std::int32_t) result->stride, spec.format);
    wl_shm_pool_destroy(pool);
    close(fd);

    return result;
  }

  bool capture_buffer_t::export_descriptor(egl::surface_descriptor_t &out) const {
    out = sd;
    for (int x = 0; x < 4; ++x) {
      if (sd.fds[x] < 0) {
        continue;
      }
      out.fds[x] = fcntl(sd.fds[x], F_DUPFD_CLOEXEC, 0);
      if (out.fds[x] < 0) {
        for (int y = 0; y < x; ++y) {
          if (out.fds[y] >= 0) {
            close(out.fds[y]);
          }
        }
        out.fds.fill(-1);
        return false;
      }
    }
    return true;
  }

  void buffer_pool_t::configure(const buffer_spec_t &new_spec) {
    if (configured() && new_spec == spec) {
      return;
    }

    spec = new_spec;
    buffers.clear();
    slots = buffer_slots_t {max_buffers};
  }

  buffer_pool_t::acquire_e buffer_pool_t::acquire(display_t &display, zwp_linux_dmabuf_v1 *linux_dmabuf, wl_shm *shm, struct gbm_device *device, std::size_t &index) {
    auto acquired = slots.acquire();
    if (!acquired) {
      return acquire_e::busy;
    }

    index = acquired->index;
    if (!acquired->allocate) {
      return acquire_e::ok;
    }

    auto buffer = spec.shm ? capture_buffer_t::make_shm(shm, spec) : capture_buffer_t::make_dmabuf(display, linux_dmabuf, device, spec);
    if (!buffer) {
      slots.abandoned(index);
      return acquire_e::failed;
    }

    ++allocations;
    BOOST_LOG(debug) << "Allocated capture buffer "sv << index << " ("sv << (spec.shm ? "shm"sv : "dma-buf"sv) << ' ' << spec.width << 'x' << spec.height << ')';

    if (buffers.size() <= index) {
      buffers.resize(index + 1);
    }
    buffers[index] = std::move(buffer);
    return acquire_e::ok;
  }

  namespace {
    /// Collects one wp_image_description_v1 and its information.
    struct description_fetch_t {
      enum class state_e {
        pending,
        ready,
        failed,
      } state {state_e::pending};
      bool info_done {false};
      image_description_t desc;

      static void failed(void *data, wp_image_description_v1 *, std::uint32_t cause, const char *msg) {
        BOOST_LOG(warning) << "The compositor cannot describe the output's colors (cause "sv << cause << "): "sv << msg;
        static_cast<description_fetch_t *>(data)->state = state_e::failed;
      }

      static void ready(void *data, wp_image_description_v1 *, std::uint32_t) {
        static_cast<description_fetch_t *>(data)->state = state_e::ready;
      }

      static void done(void *data, wp_image_description_info_v1 *) {
        static_cast<description_fetch_t *>(data)->info_done = true;
      }

      static void icc_file(void *data, wp_image_description_info_v1 *, std::int32_t fd, std::uint32_t) {
        close(fd);
        static_cast<description_fetch_t *>(data)->desc.icc = true;
      }

      static chromaticities_t xy(std::int32_t r_x, std::int32_t r_y, std::int32_t g_x, std::int32_t g_y, std::int32_t b_x, std::int32_t b_y, std::int32_t w_x, std::int32_t w_y) {
        return chromaticities_t {{r_x, r_y, g_x, g_y, b_x, b_y, w_x, w_y}};
      }

      static void primaries(void *data, wp_image_description_info_v1 *, std::int32_t r_x, std::int32_t r_y, std::int32_t g_x, std::int32_t g_y, std::int32_t b_x, std::int32_t b_y, std::int32_t w_x, std::int32_t w_y) {
        static_cast<description_fetch_t *>(data)->desc.primaries = xy(r_x, r_y, g_x, g_y, b_x, b_y, w_x, w_y);
      }

      static void primaries_named(void *data, wp_image_description_info_v1 *, std::uint32_t primaries) {
        static_cast<description_fetch_t *>(data)->desc.primaries_named = primaries;
      }

      static void tf_power(void *data, wp_image_description_info_v1 *, std::uint32_t eexp) {
        static_cast<description_fetch_t *>(data)->desc.tf_power = eexp;
      }

      static void tf_named(void *data, wp_image_description_info_v1 *, std::uint32_t tf) {
        static_cast<description_fetch_t *>(data)->desc.tf_named = tf;
      }

      static void luminances(void *data, wp_image_description_info_v1 *, std::uint32_t min_lum, std::uint32_t max_lum, std::uint32_t reference_lum) {
        auto &desc = static_cast<description_fetch_t *>(data)->desc;
        desc.min_lum = min_lum;
        desc.max_lum = max_lum;
        desc.reference_lum = reference_lum;
      }

      static void target_primaries(void *data, wp_image_description_info_v1 *, std::int32_t r_x, std::int32_t r_y, std::int32_t g_x, std::int32_t g_y, std::int32_t b_x, std::int32_t b_y, std::int32_t w_x, std::int32_t w_y) {
        static_cast<description_fetch_t *>(data)->desc.target_primaries = xy(r_x, r_y, g_x, g_y, b_x, b_y, w_x, w_y);
      }

      static void target_luminance(void *data, wp_image_description_info_v1 *, std::uint32_t min_lum, std::uint32_t max_lum) {
        auto &desc = static_cast<description_fetch_t *>(data)->desc;
        desc.target_min_lum = min_lum;
        desc.target_max_lum = max_lum;
      }

      static void target_max_cll(void *data, wp_image_description_info_v1 *, std::uint32_t max_cll) {
        static_cast<description_fetch_t *>(data)->desc.target_max_cll = max_cll;
      }

      static void target_max_fall(void *data, wp_image_description_info_v1 *, std::uint32_t max_fall) {
        static_cast<description_fetch_t *>(data)->desc.target_max_fall = max_fall;
      }
    };

    const wp_image_description_v1_listener description_listener {
      .failed = description_fetch_t::failed,
      .ready = description_fetch_t::ready,
    };

    const wp_image_description_info_v1_listener description_info_listener {
      .done = description_fetch_t::done,
      .icc_file = description_fetch_t::icc_file,
      .primaries = description_fetch_t::primaries,
      .primaries_named = description_fetch_t::primaries_named,
      .tf_power = description_fetch_t::tf_power,
      .tf_named = description_fetch_t::tf_named,
      .luminances = description_fetch_t::luminances,
      .target_primaries = description_fetch_t::target_primaries,
      .target_luminance = description_fetch_t::target_luminance,
      .target_max_cll = description_fetch_t::target_max_cll,
      .target_max_fall = description_fetch_t::target_max_fall,
    };

    void on_image_description_changed(void *data, wp_color_management_output_v1 *) {
      *static_cast<bool *>(data) = true;
    }

    const wp_color_management_output_v1_listener cm_output_listener {
      .image_description_changed = on_image_description_changed,
    };

    /// Dispatches until `done()` or the deadline; false on timeout or a broken connection.
    template<class F>
    bool dispatch_until(display_t &display, std::chrono::steady_clock::time_point deadline, F done) {
      while (!done()) {
        auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0 || wl_display_get_error(display.get())) {
          return false;
        }
        display.dispatch(remaining);
      }
      return true;
    }
  }  // namespace

  std::string describe(const image_description_t &desc) {
    std::string out;
    auto add = [&](std::string_view name, auto value) {
      if (!out.empty()) {
        out += ", ";
      }
      out += name;
      out += '=';
      out += std::to_string(value);
    };
    if (desc.icc) {
      out = "ICC profile";
    }
    if (desc.tf_named) {
      add("tf", *desc.tf_named);
    }
    if (desc.tf_power) {
      add("tf_power", *desc.tf_power);
    }
    if (desc.primaries_named) {
      add("primaries", *desc.primaries_named);
    }
    if (desc.max_lum) {
      add("min_lum_x10000", desc.min_lum.value_or(0));
      add("max_lum", *desc.max_lum);
      add("reference_lum", desc.reference_lum.value_or(0));
    }
    if (desc.target_max_lum) {
      add("target_min_lum_x10000", desc.target_min_lum.value_or(0));
      add("target_max_lum", *desc.target_max_lum);
    }
    if (desc.target_max_cll) {
      add("max_cll", *desc.target_max_cll);
    }
    if (desc.target_max_fall) {
      add("max_fall", *desc.target_max_fall);
    }
    return out + (is_hdr10(desc) ? " (HDR10)" : "");
  }

  output_color_t::~output_color_t() {
    reset();
  }

  void output_color_t::reset() {
    if (cm_output) {
      wp_color_management_output_v1_destroy(cm_output);
      cm_output = nullptr;
    }
    if (interface && interface->color_manager) {
      wp_color_manager_v1_destroy(interface->color_manager);
      interface->color_manager = nullptr;
    }
    desc.reset();
    changed = false;
  }

  bool output_color_t::open(const std::string &output_name, std::chrono::milliseconds timeout) {
    reset();

    if (display.init()) {
      return false;
    }

    interface = std::make_unique<interface_t>();
    interface->bind_color_manager = true;
    if (!enumerate_outputs(display, *interface)) {
      return false;
    }
    // The manager's supported_* events arrive in the roundtrips above.
    if (!interface->color_manager) {
      BOOST_LOG(info) << "The compositor does not support wp-color-management-v1; output colors are assumed to be SDR"sv;
      return false;
    }

    auto index = select_output(*interface, output_name);
    if (!index) {
      return false;
    }

    cm_output = wp_color_manager_v1_get_output(interface->color_manager, interface->monitors[*index]->output);
    wp_color_management_output_v1_add_listener(cm_output, &cm_output_listener, &changed);

    desc = fetch(timeout);
    if (!desc) {
      return false;
    }
    BOOST_LOG(info) << "Output ["sv << output_name << "] colors: "sv << describe(*desc);
    return true;
  }

  std::optional<image_description_t> output_color_t::fetch(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    changed = false;

    description_fetch_t fetch;
    auto description = wp_color_management_output_v1_get_image_description(cm_output);
    wp_image_description_v1_add_listener(description, &description_listener, &fetch);
    if (!dispatch_until(display, deadline, [&]() {
          return fetch.state != description_fetch_t::state_e::pending;
        })) {
      BOOST_LOG(warning) << "The compositor did not describe the output's colors in time"sv;
      wp_image_description_v1_destroy(description);
      return std::nullopt;
    }
    if (fetch.state == description_fetch_t::state_e::failed) {
      wp_image_description_v1_destroy(description);
      return std::nullopt;
    }

    auto info = wp_image_description_v1_get_information(description);
    wp_image_description_info_v1_add_listener(info, &description_info_listener, &fetch);
    const bool complete = dispatch_until(display, deadline, [&]() {
      return fetch.info_done;
    });
    // `done` is a destructor event: the compositor has destroyed its side, only the proxy is left.
    wl_proxy_destroy((wl_proxy *) info);
    wp_image_description_v1_destroy(description);

    if (!complete) {
      BOOST_LOG(warning) << "The compositor did not send the output's color information in time"sv;
      return std::nullopt;
    }
    return fetch.desc;
  }

  bool output_color_t::poll_changed() {
    if (!cm_output) {
      return false;
    }
    display.dispatch(0ms);
    if (!changed) {
      return false;
    }

    auto fresh = fetch(1s);
    if (fresh == desc) {
      return false;
    }
    if (fresh) {
      BOOST_LOG(info) << "Output colors changed: "sv << describe(*fresh);
    }
    desc = std::move(fresh);
    return true;
  }

  float output_color_t::sdr_white_nits() const {
    if (!is_hdr()) {
      return 0;
    }
    // BT.2408 reference white, also Hyprland's default for HDR outputs.
    constexpr std::uint32_t default_reference = 203;
    const auto reference = desc->reference_lum.value_or(default_reference);
    return (float) (reference ? reference : default_reference);
  }

  bool output_color_t::compositor_supports_hdr10() const {
    if (!interface || !interface->color_manager) {
      return false;
    }
    auto has = [](const std::vector<std::uint32_t> &list, std::uint32_t value) {
      return std::find(list.begin(), list.end(), value) != list.end();
    };
    return has(interface->color_tfs, color::tf::st2084_pq) && has(interface->color_primaries, color::primaries::bt2020);
  }

  bool enumerate_outputs(display_t &display, interface_t &interface) {
    interface.listen(display.registry());

    display.roundtrip();

    if (!interface[interface_t::XDG_OUTPUT]) {
      BOOST_LOG(error) << "Missing Wayland wire for xdg_output"sv;
      return false;
    }

    if (interface.monitors.empty()) {
      BOOST_LOG(error) << "The Wayland compositor reported no outputs"sv;
      return false;
    }

    // Names and the layout bounding box need every output, not just the one we stream.
    for (auto &monitor : interface.monitors) {
      monitor->listen(interface.output_manager);
    }

    display.roundtrip();

    return true;
  }

  std::vector<std::string> output_names(const interface_t &interface) {
    std::vector<std::string> names;
    names.reserve(interface.monitors.size());
    for (std::size_t x = 0; x < interface.monitors.size(); ++x) {
      const auto &name = interface.monitors[x]->name;
      names.emplace_back(name.empty() ? std::to_string(x) : name);
    }
    return names;
  }

  std::optional<std::size_t> select_output(const interface_t &interface, const std::string &display_name) {
    if (display_name.empty()) {
      return interface.monitors.empty() ? std::nullopt : std::optional<std::size_t> {0};
    }

    const auto names = output_names(interface);
    auto found = platf::find_display(names, display_name, true);
    if (!found) {
      BOOST_LOG(error) << "Wayland output ["sv << display_name << "] not found; available outputs: "sv << platf::join_display_names(names);
      return std::nullopt;
    }

    if (names[*found] != display_name) {
      BOOST_LOG(warning) << "Selecting Wayland output by index ["sv << display_name << "] is deprecated; use its name ["sv << names[*found] << "] instead"sv;
    }

    return found;
  }

  void apply_output_geometry(platf::display_t &display, const interface_t &interface, std::size_t index, extent_t frame) {
    const auto &monitor = *interface.monitors[index];
    const auto &geometry = monitor.geometry;

    display.width = frame.width;
    display.height = frame.height;

    const auto logical = logical_size(geometry);
    display.offset_x = geometry.logical_x;
    display.offset_y = geometry.logical_y;
    display.logical_width = logical.width;
    display.logical_height = logical.height;

    // The whole layout is in logical pixels, like the offsets above. The same outputs as output_change() watches.
    const auto bounds = layout_bounds(layout_geometries(interface));
    display.env_offset_x = bounds.min_x;
    display.env_offset_y = bounds.min_y;
    display.env_width = bounds.width;
    display.env_height = bounds.height;
    display.env_logical_width = bounds.width;
    display.env_logical_height = bounds.height;

    const auto name = output_names(interface)[index];
    display.output_name = name;
    // Absolute input is rotated by display.transform. Compositors that deliver frames already rotated (Hyprland) leave
    // nothing to rotate; only frames in the output's native orientation need it.
    display.transform = frame == transformed_mode_size(geometry) ? 0 : geometry.transform;
    BOOST_LOG(info) << "Selected Wayland output ["sv << name << "] ("sv << monitor.description << ") for streaming"sv;
    BOOST_LOG(debug) << "Mode: "sv << geometry.mode_width << 'x' << geometry.mode_height << ", frame "sv << display.width << 'x' << display.height << ", transform "sv << geometry.transform << ", scale "sv << effective_scale(geometry);
    BOOST_LOG(debug) << "Logical geometry: "sv << display.logical_width << 'x' << display.logical_height << " at "sv << display.offset_x << ',' << display.offset_y;
    BOOST_LOG(debug) << "Desktop layout: "sv << display.env_width << 'x' << display.env_height << " at "sv << display.env_offset_x << ',' << display.env_offset_y;

    if (frame != transformed_mode_size(geometry)) {
      BOOST_LOG(warning) << "Wayland output ["sv << name << "] has transform "sv << geometry.transform
                         << " but the compositor delivers "sv << frame.width << 'x' << frame.height
                         << " frames; the stream will not be rotated to match (absolute input is)"sv;
    }
  }

  void log_outputs(const interface_t &interface, const std::vector<std::string> &names) {
    BOOST_LOG(info) << "-------- Start of Wayland monitor list --------"sv;

    for (std::size_t x = 0; x < interface.monitors.size(); ++x) {
      auto monitor = interface.monitors[x].get();
      const auto &geometry = monitor->geometry;
      const auto logical = logical_size(geometry);

      BOOST_LOG(info) << "Monitor "sv << x << " is "sv << names[x] << ": "sv << monitor->description
                      << " ("sv << geometry.mode_width << 'x' << geometry.mode_height
                      << ", logical "sv << logical.width << 'x' << logical.height << " at "sv << geometry.logical_x << ',' << geometry.logical_y
                      << ", scale "sv << effective_scale(geometry) << ", transform "sv << geometry.transform << ')';
    }

    BOOST_LOG(info) << "--------- End of Wayland monitor list ---------"sv;
  }

  bool read_buffer(egl::display_t::pointer egl_display, const capture_buffer_t &buffer, platf::img_t &img, bool y_invert) {
    const auto width = std::min<std::size_t>(img.width, buffer.spec.width);
    const auto height = std::min<std::size_t>(img.height, buffer.spec.height);

    if (buffer.spec.shm) {
      copy_rows(buffer.data, buffer.stride, img.data, img.row_pitch, width * 4, height, y_invert);
      return true;
    }

    auto rgb_opt = egl::import_source(egl_display, buffer.sd);
    if (!rgb_opt) {
      return false;
    }

    gl::ctx.BindTexture(GL_TEXTURE_2D, (*rgb_opt)->tex[0]);

    // Don't remove these lines, see https://github.com/LizardByte/Sunshine/issues/453
    int w, h;
    gl::ctx.GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    gl::ctx.GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);

    gl::ctx.GetTextureSubImage((*rgb_opt)->tex[0], 0, 0, 0, 0, width, height, 1, GL_BGRA, GL_UNSIGNED_BYTE, img.height * img.row_pitch, img.data);
    gl::ctx.BindTexture(GL_TEXTURE_2D, 0);

    if (y_invert) {
      std::vector<std::uint8_t> row(width * 4);
      for (std::size_t y = 0; y < height / 2; ++y) {
        auto top = img.data + y * img.row_pitch;
        auto bottom = img.data + (height - 1 - y) * img.row_pitch;
        std::copy_n(top, row.size(), row.data());
        std::copy_n(bottom, row.size(), top);
        std::copy_n(row.data(), row.size(), bottom);
      }
    }

    return true;
  }

  bool hand_over_buffer(buffer_pool_t &pool, std::size_t slot, const std::shared_ptr<platf::img_t> &img_out, std::uint64_t sequence, bool y_invert) {
    auto img = static_cast<egl::img_descriptor_t *>(img_out.get());
    img->reset();

    if (!pool[slot].export_descriptor(img->sd)) {
      BOOST_LOG(error) << "Couldn't duplicate the capture buffer's file descriptors: "sv << std::strerror(errno);
      return false;
    }

    img->sequence = sequence;
    img->y_invert = y_invert;
    pool.slots.hand_over(slot, std::weak_ptr<void> {std::shared_ptr<void> {img_out}});
    return true;
  }

  static bool validate() {
    display_t display;

    return display.init() == 0;
  }

  /**
   * @brief Initialize Wayland registry interfaces required for capture.
   */
  int init() {
    static bool validated = validate();

    return !validated;
  }

}  // namespace wl

#pragma GCC diagnostic pop
