/**
 * @file src/platform/linux/wayland.cpp
 * @brief Definitions for Wayland capture.
 */
// standard includes
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>

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

// local includes
#include "graphics.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/platform/display_selection.h"
#include "src/round_robin.h"
#include "src/utility.h"
#include "wayland.h"

extern const wl_interface wl_output_interface;

using namespace std::literals;

// Disable warning for converting incompatible functions
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wpmf-conversions"

namespace wl {

  // Helper to call C++ method from wayland C callback
  template<class T, class Method, Method m, class... Params>
  static auto classCall(void *data, Params... params) -> decltype(((*reinterpret_cast<T *>(data)).*m)(params...)) {
    return ((*reinterpret_cast<T *>(data)).*m)(params...);
  }

#define CLASS_CALL(c, m) classCall<c, decltype(&c::m), &c::m>

  int display_t::init(const char *display_name) {
    if (!display_name) {
      display_name = std::getenv("WAYLAND_DISPLAY");
    }

    if (!display_name) {
      BOOST_LOG(error) << "Environment variable WAYLAND_DISPLAY has not been defined"sv;
      return -1;
    }

    display_internal.reset(wl_display_connect(display_name));
    if (!display_internal) {
      BOOST_LOG(error) << "Couldn't connect to Wayland display: "sv << display_name;
      return -1;
    }

    BOOST_LOG(info) << "Found display ["sv << display_name << ']';

    return 0;
  }

  void display_t::roundtrip() {
    wl_display_roundtrip(display_internal.get());
  }

  /**
   * @brief Waits up to the specified timeout to dispatch new events on the wl_display.
   * @param timeout The timeout in milliseconds.
   * @return `true` if new events were dispatched or `false` if the timeout expired.
   */
  bool display_t::dispatch(std::chrono::milliseconds timeout) {
    // Check if any events are queued already. If not, flush
    // outgoing events, and prepare to wait for readability.
    if (wl_display_prepare_read(display_internal.get()) == 0) {
      wl_display_flush(display_internal.get());

      // Wait for an event to come in
      struct pollfd pfd = {};
      pfd.fd = wl_display_get_fd(display_internal.get());
      pfd.events = POLLIN;
      if (poll(&pfd, 1, timeout.count()) == 1 && (pfd.revents & POLLIN)) {
        // Read the new event(s)
        wl_display_read_events(display_internal.get());
      } else {
        // We timed out, so unlock the queue now
        wl_display_cancel_read(display_internal.get());
        return false;
      }
    }

    // Dispatch any existing or new pending events
    wl_display_dispatch_pending(display_internal.get());
    return true;
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

  inline monitor_t::monitor_t(wl_output *output):
      output {output},
      wl_listener {
        &CLASS_CALL(monitor_t, wl_geometry),
        &CLASS_CALL(monitor_t, wl_mode),
        &CLASS_CALL(monitor_t, wl_done),
        &CLASS_CALL(monitor_t, wl_scale),
#ifdef WL_OUTPUT_NAME_SINCE_VERSION
        &CLASS_CALL(monitor_t, wl_name),
        &CLASS_CALL(monitor_t, wl_description),
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
    // wl_output.name (v4) is the authoritative connector name; xdg_output.name is the fallback.
    if (!has_wl_name) {
      this->name = name;
    }

    BOOST_LOG(debug) << "xdg_output name: "sv << name;
  }

  void monitor_t::xdg_description(zxdg_output_v1 *, const char *description) {
    if (this->description.empty()) {
      this->description = description;
    }

    BOOST_LOG(debug) << "xdg_output description: "sv << description;
  }

  void monitor_t::xdg_position(zxdg_output_v1 *, std::int32_t x, std::int32_t y) {
    geometry.logical_x = x;
    geometry.logical_y = y;

    BOOST_LOG(debug) << "Logical position: "sv << x << ',' << y;
  }

  void monitor_t::xdg_size(zxdg_output_v1 *, std::int32_t width, std::int32_t height) {
    geometry.logical_width = width;
    geometry.logical_height = height;

    BOOST_LOG(debug) << "Logical size: "sv << width << 'x' << height;
  }

  void monitor_t::wl_geometry(
    wl_output *wl_output,
    std::int32_t x,
    std::int32_t y,
    std::int32_t physical_width,
    std::int32_t physical_height,
    std::int32_t subpixel,
    const char *make,
    const char *model,
    std::int32_t transform
  ) {
    geometry.transform = transform;

    BOOST_LOG(debug) << "Transform: "sv << transform;
  }

  void monitor_t::wl_mode(
    wl_output *wl_output,
    std::uint32_t flags,
    std::int32_t width,
    std::int32_t height,
    std::int32_t refresh
  ) {
    if (!(flags & WL_OUTPUT_MODE_CURRENT)) {
      return;
    }

    geometry.mode_width = width;
    geometry.mode_height = height;

    BOOST_LOG(debug) << "Mode: "sv << width << 'x' << height;
  }

  void monitor_t::wl_scale(wl_output *wl_output, std::int32_t factor) {
    geometry.integer_scale = factor;
  }

  void monitor_t::wl_name(wl_output *wl_output, const char *name) {
    this->name = name;
    has_wl_name = true;

    BOOST_LOG(debug) << "wl_output name: "sv << name;
  }

  void monitor_t::wl_description(wl_output *wl_output, const char *description) {
    this->description = description;
  }

  void monitor_t::listen(zxdg_output_manager_v1 *output_manager) {
    auto xdg_output = zxdg_output_manager_v1_get_xdg_output(output_manager, output);
    zxdg_output_v1_add_listener(xdg_output, &xdg_listener, this);
    wl_output_add_listener(output, &wl_listener, this);
  }

  interface_t::interface_t() noexcept
      :
      screencopy_manager {nullptr},
      dmabuf_interface {nullptr},
      output_manager {nullptr},
      ext_copy_manager {nullptr},
      ext_output_source_manager {nullptr},
      shm {nullptr},
      listener {
        &CLASS_CALL(interface_t, add_interface),
        &CLASS_CALL(interface_t, del_interface)
      } {
  }

  void interface_t::listen(wl_registry *registry) {
    wl_registry_add_listener(registry, &listener, this);
  }

  void interface_t::add_interface(
    wl_registry *registry,
    std::uint32_t id,
    const char *interface,
    std::uint32_t version
  ) {
    BOOST_LOG(debug) << "Available interface: "sv << interface << '(' << id << ") version "sv << version;

    if (!std::strcmp(interface, wl_output_interface.name)) {
      BOOST_LOG(info) << "Found interface: "sv << interface << '(' << id << ") version "sv << version;
      monitors.emplace_back(
        std::make_unique<monitor_t>(
          (wl_output *) wl_registry_bind(registry, id, &wl_output_interface, std::min(version, max_wl_output_version))
        )
      );
    } else if (!std::strcmp(interface, zxdg_output_manager_v1_interface.name)) {
      BOOST_LOG(info) << "Found interface: "sv << interface << '(' << id << ") version "sv << version;
      output_manager = (zxdg_output_manager_v1 *) wl_registry_bind(registry, id, &zxdg_output_manager_v1_interface, std::min(version, max_xdg_output_manager_version));

      this->interface[XDG_OUTPUT] = true;
    } else if (!std::strcmp(interface, zwlr_screencopy_manager_v1_interface.name)) {
      BOOST_LOG(info) << "Found interface: "sv << interface << '(' << id << ") version "sv << version;
      screencopy_manager = (zwlr_screencopy_manager_v1 *) wl_registry_bind(registry, id, &zwlr_screencopy_manager_v1_interface, version);

      this->interface[WLR_EXPORT_DMABUF] = true;
    } else if (!std::strcmp(interface, zwp_linux_dmabuf_v1_interface.name)) {
      BOOST_LOG(info) << "Found interface: "sv << interface << '(' << id << ") version "sv << version;
      dmabuf_interface = (zwp_linux_dmabuf_v1 *) wl_registry_bind(registry, id, &zwp_linux_dmabuf_v1_interface, version);

      this->interface[LINUX_DMABUF] = true;
    } else if (!std::strcmp(interface, ext_image_copy_capture_manager_v1_interface.name)) {
      BOOST_LOG(info) << "Found interface: "sv << interface << '(' << id << ") version "sv << version;
      ext_copy_manager = (ext_image_copy_capture_manager_v1 *) wl_registry_bind(registry, id, &ext_image_copy_capture_manager_v1_interface, 1);

      this->interface[EXT_IMAGE_COPY_CAPTURE] = true;
    } else if (!std::strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name)) {
      BOOST_LOG(info) << "Found interface: "sv << interface << '(' << id << ") version "sv << version;
      ext_output_source_manager = (ext_output_image_capture_source_manager_v1 *) wl_registry_bind(registry, id, &ext_output_image_capture_source_manager_v1_interface, 1);

      this->interface[EXT_OUTPUT_CAPTURE_SOURCE] = true;
    } else if (!std::strcmp(interface, wl_shm_interface.name)) {
      shm = (wl_shm *) wl_registry_bind(registry, id, &wl_shm_interface, 1);

      this->interface[SHM] = true;
    }
  }

  void interface_t::del_interface(wl_registry *registry, uint32_t id) {
    BOOST_LOG(info) << "Delete: "sv << id;
  }

  gbm_device_t::~gbm_device_t() {
    if (device) {
      gbm_device_destroy(device);
    }
    if (fd >= 0) {
      close(fd);
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
        BOOST_LOG(warning) << "Couldn't find the DRM device the compositor asked for ("sv << major((dev_t) *wanted) << ':' << minor((dev_t) *wanted) << "); using the first render node"sv;
      }
    }

    if (node.empty()) {
      drmDevice *devices[16];
      int n = drmGetDevices2(0, devices, 16);
      if (n <= 0) {
        BOOST_LOG(error) << "No DRM devices found"sv;
        return false;
      }
      for (int i = 0; i < n && node.empty(); i++) {
        if (devices[i]->available_nodes & (1 << DRM_NODE_RENDER)) {
          node = devices[i]->nodes[DRM_NODE_RENDER];
        }
      }
      drmFreeDevices(devices, n);
    }

    if (node.empty()) {
      BOOST_LOG(error) << "No DRM render node found"sv;
      return false;
    }

    fd = ::open(node.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
      BOOST_LOG(error) << "Failed to open DRM node "sv << node;
      return false;
    }

    device = gbm_create_device(fd);
    if (!device) {
      close(fd);
      fd = -1;
      BOOST_LOG(error) << "Failed to create GBM device on "sv << node;
      return false;
    }

    struct stat st {};
    if (fstat(fd, &st) == 0) {
      dev = (std::uint64_t) st.st_rdev;
    }

    BOOST_LOG(debug) << "Allocating capture buffers on "sv << node;
    return true;
  }

  capture_buffer_t::capture_buffer_t() {
    std::fill_n(sd.fds, 4, -1);
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
        std::fill_n(out.fds, 4, -1);
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

  std::optional<buffer_spec_t> screencopy_buffer_spec(const screencopy_constraints_t &constraints, bool allow_dmabuf, bool allow_shm) {
    if (allow_dmabuf && constraints.dmabuf) {
      buffer_spec_t spec;
      spec.format = constraints.dmabuf->format;
      spec.width = constraints.dmabuf->width;
      spec.height = constraints.dmabuf->height;
      // wlr-screencopy announces no modifiers: let the allocator pick an implicit one.
      spec.implicit_modifier = true;
      return spec;
    }

    if (allow_shm && constraints.shm && (constraints.shm->format == shm_format::XRGB8888 || constraints.shm->format == shm_format::ARGB8888)) {
      buffer_spec_t spec;
      spec.shm = true;
      spec.format = constraints.shm->format;
      spec.width = constraints.shm->width;
      spec.height = constraints.shm->height;
      spec.stride = constraints.shm->stride;
      return spec;
    }

    return std::nullopt;
  }

  screencopy_t::screencopy_t(std::size_t max_buffers):
      pool {max_buffers},
      listener {
        &CLASS_CALL(screencopy_t, buffer),
        &CLASS_CALL(screencopy_t, flags),
        &CLASS_CALL(screencopy_t, ready),
        &CLASS_CALL(screencopy_t, failed),
        &CLASS_CALL(screencopy_t, damage),
        &CLASS_CALL(screencopy_t, linux_dmabuf),
        &CLASS_CALL(screencopy_t, buffer_done),
      } {
  }

  screencopy_t::~screencopy_t() {
    if (frame) {
      zwlr_screencopy_frame_v1_destroy(frame);
    }
  }

  buffer_pool_t::acquire_e screencopy_t::start(display_t &display, zwlr_screencopy_manager_v1 *screencopy_manager, zwp_linux_dmabuf_v1 *linux_dmabuf, wl_shm *shm, struct gbm_device *device, wl_output *output, bool blend_cursor) {
    if (frame) {
      // A frame is still in flight (a previous snapshot timed out): keep waiting for it.
      return buffer_pool_t::acquire_e::ok;
    }

    std::size_t slot;
    auto result = pool.acquire(display, linux_dmabuf, shm, device, slot);
    if (result == buffer_pool_t::acquire_e::failed) {
      status = REINIT;
    }
    if (result != buffer_pool_t::acquire_e::ok) {
      return result;
    }

    frame_slot = slot;
    announced = {};
    frame_y_invert = false;

    frame = zwlr_screencopy_manager_v1_capture_output(screencopy_manager, blend_cursor ? 1 : 0, output);
    zwlr_screencopy_frame_v1_add_listener(frame, &listener, this);

    status = WAITING;
    return result;
  }

  void screencopy_t::abort_frame() {
    if (frame) {
      zwlr_screencopy_frame_v1_destroy(frame);
      frame = nullptr;
      pool.slots.abandoned(frame_slot);
    }
    status = REINIT;
  }

  void screencopy_t::buffer(zwlr_screencopy_frame_v1 *, std::uint32_t format, std::uint32_t width, std::uint32_t height, std::uint32_t stride) {
    announced.shm = screencopy_constraints_t::shm_t {format, width, height, stride};
  }

  void screencopy_t::linux_dmabuf(zwlr_screencopy_frame_v1 *, std::uint32_t format, std::uint32_t width, std::uint32_t height) {
    announced.dmabuf = screencopy_constraints_t::dmabuf_t {format, width, height};
  }

  void screencopy_t::buffer_done(zwlr_screencopy_frame_v1 *frame) {
    // Our buffers were allocated for the constraints probed at startup. If the compositor now wants something
    // else (mode change, ...), the display has to be recreated.
    auto spec = screencopy_buffer_spec(announced, !pool.spec.shm, pool.spec.shm);
    if (!spec || *spec != pool.spec) {
      BOOST_LOG(info) << "Screencopy buffer constraints changed; reinitializing capture"sv;
      abort_frame();
      return;
    }

    zwlr_screencopy_frame_v1_copy(frame, pool[frame_slot].buffer);
  }

  void screencopy_t::flags(zwlr_screencopy_frame_v1 *, std::uint32_t flags) {
    frame_y_invert = flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
  }

  void screencopy_t::damage(zwlr_screencopy_frame_v1 *, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) {
    // Frames are copied in full (copy rather than copy_with_damage), so damage is not needed.
  }

  void screencopy_t::ready(zwlr_screencopy_frame_v1 *frame, std::uint32_t, std::uint32_t, std::uint32_t) {
    // The ready timestamp's seconds "may have an arbitrary offset", so it is not used as the frame time.
    zwlr_screencopy_frame_v1_destroy(frame);
    this->frame = nullptr;

    pool.slots.completed(frame_slot, rect_t {0, 0, (int) pool.spec.width, (int) pool.spec.height});
    y_invert = frame_y_invert;
    status = READY;
  }

  void screencopy_t::failed(zwlr_screencopy_frame_v1 *) {
    BOOST_LOG(error) << "Frame capture failed"sv;
    abort_frame();
  }

  std::vector<std::unique_ptr<monitor_t>> monitors(const char *display_name) {
    display_t display;

    if (display.init(display_name)) {
      return {};
    }

    interface_t interface;
    interface.listen(display.registry());

    display.roundtrip();

    if (!interface[interface_t::XDG_OUTPUT]) {
      BOOST_LOG(error) << "Missing Wayland wire XDG_OUTPUT"sv;
      return {};
    }

    for (auto &monitor : interface.monitors) {
      monitor->listen(interface.output_manager);
    }

    display.roundtrip();

    return std::move(interface.monitors);
  }

  namespace {
    struct screencopy_probe_t {
      screencopy_constraints_t constraints;
      bool announced {false};

      static void buffer(void *data, zwlr_screencopy_frame_v1 *, std::uint32_t format, std::uint32_t width, std::uint32_t height, std::uint32_t stride) {
        auto self = static_cast<screencopy_probe_t *>(data);
        self->constraints.shm = screencopy_constraints_t::shm_t {format, width, height, stride};
        self->announced = true;
      }

      static void linux_dmabuf(void *data, zwlr_screencopy_frame_v1 *, std::uint32_t format, std::uint32_t width, std::uint32_t height) {
        auto self = static_cast<screencopy_probe_t *>(data);
        self->constraints.dmabuf = screencopy_constraints_t::dmabuf_t {format, width, height};
        self->announced = true;
      }

      static void buffer_done(void *, zwlr_screencopy_frame_v1 *) {}

      static void failed(void *, zwlr_screencopy_frame_v1 *) {}

      static void flags(void *, zwlr_screencopy_frame_v1 *, std::uint32_t) {}

      static void ready(void *, zwlr_screencopy_frame_v1 *, std::uint32_t, std::uint32_t, std::uint32_t) {}

      static void damage(void *, zwlr_screencopy_frame_v1 *, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) {}
    };

    const zwlr_screencopy_frame_v1_listener screencopy_probe_listener {
      .buffer = screencopy_probe_t::buffer,
      .flags = screencopy_probe_t::flags,
      .ready = screencopy_probe_t::ready,
      .failed = screencopy_probe_t::failed,
      .damage = screencopy_probe_t::damage,
      .linux_dmabuf = screencopy_probe_t::linux_dmabuf,
      .buffer_done = screencopy_probe_t::buffer_done,
    };
  }  // namespace

  std::optional<screencopy_constraints_t> probe_screencopy(display_t &display, zwlr_screencopy_manager_v1 *screencopy_manager, wl_output *output) {
    screencopy_probe_t probe;

    auto frame = zwlr_screencopy_manager_v1_capture_output(screencopy_manager, 0, output);
    zwlr_screencopy_frame_v1_add_listener(frame, &screencopy_probe_listener, &probe);

    // The buffer events are sent right away. A roundtrip is enough for every protocol version:
    // version 3 ends them with buffer_done, older versions only send buffer.
    display.roundtrip();

    // Destroying a frame that was never copied is allowed and does not capture anything.
    zwlr_screencopy_frame_v1_destroy(frame);

    if (!probe.announced) {
      return std::nullopt;
    }
    return probe.constraints;
  }

  std::optional<extent_t> probe_frame_size(display_t &display, zwlr_screencopy_manager_v1 *screencopy_manager, wl_output *output) {
    auto constraints = probe_screencopy(display, screencopy_manager, output);
    if (!constraints) {
      return std::nullopt;
    }
    if (constraints->dmabuf) {
      return extent_t {(int) constraints->dmabuf->width, (int) constraints->dmabuf->height};
    }
    return extent_t {(int) constraints->shm->width, (int) constraints->shm->height};
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

    std::vector<output_geometry_t> geometries;
    geometries.reserve(interface.monitors.size());
    for (const auto &m : interface.monitors) {
      geometries.emplace_back(m->geometry);
    }
    const auto bounds = layout_bounds(geometries);
    display.env_x = bounds.min_x;
    display.env_y = bounds.min_y;
    display.env_width = bounds.width;
    display.env_height = bounds.height;

    const auto name = output_names(interface)[index];
    BOOST_LOG(info) << "Selected Wayland output ["sv << name << "] ("sv << monitor.description << ") for streaming"sv;
    BOOST_LOG(debug) << "Mode: "sv << geometry.mode_width << 'x' << geometry.mode_height << ", frame "sv << display.width << 'x' << display.height << ", transform "sv << geometry.transform << ", scale "sv << effective_scale(geometry);
    BOOST_LOG(debug) << "Logical geometry: "sv << display.logical_width << 'x' << display.logical_height << " at "sv << display.offset_x << ',' << display.offset_y;
    BOOST_LOG(debug) << "Desktop layout: "sv << display.env_width << 'x' << display.env_height << " at "sv << display.env_x << ',' << display.env_y;

    if (frame != transformed_mode_size(geometry)) {
      BOOST_LOG(warning) << "Wayland output ["sv << name << "] has transform "sv << geometry.transform
                         << " but the compositor delivers "sv << frame.width << 'x' << frame.height
                         << " frames; the stream will not be rotated to match"sv;
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

  int init() {
    static bool validated = validate();

    return !validated;
  }

}  // namespace wl

#pragma GCC diagnostic pop
