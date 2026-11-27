/**
 * @file src/platform/linux/wayland.h
 * @brief Declarations for Wayland capture.
 */
#pragma once

// standard includes
#include <array>
#include <bitset>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef SUNSHINE_BUILD_WAYLAND
  #include <color-management-v1.h>
  #include <ext-image-capture-source-v1.h>
  #include <ext-image-copy-capture-v1.h>
  #include <linux-dmabuf-unstable-v1.h>
  #include <wlr-screencopy-unstable-v1.h>
  #include <xdg-output-unstable-v1.h>
#endif

// local includes
#include "graphics.h"
#include "src/platform/common.h"
#include "wayland_capture_logic.h"
#include "wayland_color.h"
#include "wayland_geometry.h"

/**
 * The classes defined in this macro block should only be used by
 * cpp files whose compilation depends on SUNSHINE_BUILD_WAYLAND
 */
#ifdef SUNSHINE_BUILD_WAYLAND

struct gbm_bo;
struct gbm_device;
struct _drmDevice;

namespace platf {
  class display_t;
}

namespace wl {
  /**
   * @brief PCI bus address identifying one GPU.
   */
  struct pci_bus_id {
    std::uint32_t domain {};  ///< PCI domain number.
    std::uint8_t bus {};  ///< PCI bus number.
    std::uint8_t device {};  ///< PCI device number.
    std::uint8_t function {};  ///< PCI function number.

    /**
     * @brief Compare two PCI bus addresses for equality.
     * @return True when domain, bus, device, and function all match.
     */
    bool operator==(const pci_bus_id &) const = default;
  };

  /**
   * @brief Read the PCI bus id of a DRM render node, without parsing it.
   *
   * @param render_path Render node path to query.
   * @return Raw bus id (e.g. "0000:0f:00.0"), or no value when it cannot be read.
   */
  using render_node_pci_reader_t = std::function<std::optional<std::string>(const std::string &render_path)>;

  /**
   * @brief Read the PCI bus id of a CUDA device, without parsing it.
   *
   * @param cuda_index CUDA device index to query.
   * @return Raw bus id reported by the driver (e.g. "00000000:0F:00.0"),
   *         or no value when CUDA is unavailable or the query fails.
   */
  using cuda_device_pci_reader_t = std::function<std::optional<std::string>(int cuda_index)>;

  /**
   * @brief Resolve a DRM render node path to its canonical device node.
   *
   * @param path Render node path to resolve.
   * @return Canonical device path supplied by the resolver.
   */
  using render_path_resolver_t = std::function<std::string(const std::string &path)>;

  /**
   * @brief Resolve a DRM render node path, following udev by-path aliases.
   *
   * @param path Render node path to resolve, possibly a by-path alias.
   * @return Canonical device node path, or the input path verbatim when it cannot be resolved.
   */
  std::string resolve_drm_node_path(const std::string &path);

  /**
   * @brief Parse a PCI bus id in sysfs or CUDA form.
   *
   * Accepts both the sysfs rendering ("dddd:bb:dd.f") and the CUDA rendering
   * ("dddddddd:BB:DD.F"); comparison is numeric so width and case differences
   * do not matter.
   *
   * @param bus_id Bus id text to parse.
   * @return Parsed address, or no value when the text is malformed.
   */
  std::optional<pci_bus_id> parse_pci_bus_id(std::string_view bus_id);

  /**
   * @brief Functions used to query the PCI identity of a DRM render node.
   */
  struct drm_device_accessors_t {
    int (*open_render_node)(const char *path);  ///< Open the render node and return its descriptor, or -1.
    int (*get_device)(int fd, std::uint32_t flags, _drmDevice **device);  ///< Query the DRM device, returning 0 on success.
    void (*free_device)(_drmDevice **device);  ///< Release a DRM device returned by the query.
  };

  /**
   * @brief Query the PCI bus id of a DRM render node from its device.
   *
   * @param render_path Render node path to query.
   * @return Bus id text, or no value when the node cannot be opened or is not a PCI device.
   */
  std::optional<std::string> render_node_pci_bus_id(const std::string &render_path);

  /**
   * @brief Query the PCI bus id of a DRM render node with injected device operations.
   *
   * @param render_path Render node path to query.
   * @param accessors Device operations used to open and query the node.
   * @return Bus id text, or no value when the node cannot be opened or is not a PCI device.
   */
  std::optional<std::string> render_node_pci_bus_id(const std::string &render_path, const drm_device_accessors_t &accessors);

  /**
   * @brief Check whether the capture render node is the CUDA encode device.
   *
   * Zero-copy VRAM capture hands the compositor's DMA-BUF directly to the
   * encoder's GL context, which is only valid when capture and encode run on
   * the same GPU, so identity is compared by PCI bus address. Anything
   * unprovable (unreadable node, unavailable CUDA, unparsable ids, distinct
   * addresses) selects the GPU -> RAM -> GPU bridge.
   *
   * @param read_render_pci Render node PCI reader; queries the node directly when empty.
   * @param read_cuda_pci CUDA device PCI reader; queries the driver directly when empty.
   * @param resolve_path Render node path resolver; resolves aliases directly when empty.
   * @param cuda_index CUDA device index NVENC encodes on (always 0: video.cpp creates the CUDA hwdevice with a null device string).
   * @return True when the capture node and the CUDA device share a PCI address.
   */
  bool capture_node_is_cuda_device(const render_node_pci_reader_t &read_render_pci = {}, const cuda_device_pci_reader_t &read_cuda_pci = {}, const render_path_resolver_t &resolve_path = {}, int cuda_index = 0);

  /**
   * @brief Check whether a linear copy buffer should be attempted for the format.
   *
   * The linear attempt is only valid when the compositor advertised the linear
   * modifier for the format; otherwise allocation falls through to the
   * advertised modifier list.
   *
   * @param prefer_linear_copy Whether linear copy buffers are preferred for cross-GPU encode.
   * @param modifiers Effective modifier list advertised for the format.
   * @return True only when linear copy is preferred and the linear modifier is advertised.
   */
  bool should_attempt_linear_copy(bool prefer_linear_copy, const std::vector<std::uint64_t> &modifiers);

  /**
   * @brief Look up the advertised modifier list for a DMA-BUF format.
   *
   * @param modifiers Modifier map to search, or nullptr when no list is available.
   * @param format DMA-BUF format to look up.
   * @return Pointer to the advertised list, or nullptr when the map is missing, the format is absent, or the list is empty.
   */
  const std::vector<std::uint64_t> *find_advertised_modifiers(const std::map<std::uint32_t, std::vector<std::uint64_t>> *modifiers, std::uint32_t format);

  /**
   * @brief Determine whether wlroots capture should keep frames in VRAM for the requested memory type.
   *
   * @param hwdevice_type Hardware device type requested for capture or encode.
   * @param read_render_pci Render node PCI reader forwarded to the CUDA-device identity check.
   * @param read_cuda_pci CUDA device PCI reader forwarded to the CUDA-device identity check.
   * @return `true` when the requested memory type should use the wlroots VRAM path.
   */
  bool use_vram_capture(platf::mem_type_e hwdevice_type, const render_node_pci_reader_t &read_render_pci = {}, const cuda_device_pci_reader_t &read_cuda_pci = {});

  /**
   * @brief Determine whether wlroots capture should ask for frames with `copy_with_damage`.
   *
   * With `copy_with_damage` the compositor answers when the output has a new picture, so capture
   * follows the compositor's frames instead of sampling them on a clock of Sunshine's own. That
   * needs version 2 of the screencopy protocol and an output that refreshes at least half again
   * as fast as the stream. On an output running at the stream's own rate, the commits a paced
   * copy forces are part of what keeps the output's clients on time.
   *
   * @param screencopy_version Version of the screencopy global the compositor offered.
   * @param stream_fps Frame rate requested for the stream.
   * @param refresh_mhz Refresh rate of the captured output in mHz, 0 when the compositor did not say.
   * @return `true` when frames should be asked for with `copy_with_damage`.
   */
  bool use_damage_capture(std::uint32_t screencopy_version, double stream_fps, std::int32_t refresh_mhz);

  /**
   * @brief Decide whether a received frame means capture has to be reinitialized.
   *
   * The display's size and transform feed the touch port, so a change in either has to rebuild it.
   * Turning an output between 90 and 270 degrees changes neither its native nor its logical size,
   * so the transform is compared on its own.
   *
   * @param frame_width Width of the received frame.
   * @param frame_height Height of the received frame.
   * @param width Width the capture was initialized with.
   * @param height Height the capture was initialized with.
   * @param output_transform Current transform of the captured output.
   * @param transform Transform the capture was initialized with.
   * @return `true` when capture has to be reinitialized.
   */
  bool capture_needs_reinit(int frame_width, int frame_height, int width, int height, int output_transform, int transform);

  /**
   * @brief Move the earliest time of the next damage-driven request on by one captured frame.
   *
   * Every captured frame costs one frame interval of budget, which holds an output that changes
   * faster than the stream to the stream's rate. A frame that arrives after the budget ran out
   * resets it to a quarter interval before the frame's own time, so a source running at the
   * stream's rate is asked for with room to spare.
   *
   * @param next_request Earliest request time before this frame.
   * @param frame_time Time the compositor presented the captured frame.
   * @param delay Frame interval of the stream.
   * @return Earliest time the next frame may be asked for.
   */
  std::chrono::steady_clock::time_point next_damage_request(
    std::chrono::steady_clock::time_point next_request,
    std::chrono::steady_clock::time_point frame_time,
    std::chrono::nanoseconds delay
  );

  /**
   * @brief Pick the time a captured frame is charged to the damage-driven budget at.
   *
   * The compositor's own timestamp is the better one, it does not carry the delay of getting the
   * frame to Sunshine. The screencopy protocol does not say which clock it is on, though. A time
   * ahead of `now`, or more than one interval behind it, is not from the clock the budget runs on
   * and would leave the budget permanently due or permanently out of reach, so the frame is
   * charged at `now` instead.
   *
   * @param compositor_time Time the compositor gave for the frame, if any.
   * @param now Time the frame was received, on Sunshine's clock.
   * @param delay Frame interval of the stream.
   * @return `compositor_time` when it lies within one interval before `now`, otherwise `now`.
   */
  std::chrono::steady_clock::time_point damage_frame_time(
    std::optional<std::chrono::steady_clock::time_point> compositor_time,
    std::chrono::steady_clock::time_point now,
    std::chrono::nanoseconds delay
  );

  /**
   * @brief What a capture does about its screencopy request before waiting for a frame.
   */
  enum class screencopy_request_e {
    keep,  ///< Keep waiting for the request that is already out.
    copy,  ///< Ask for a frame now.
    copy_with_damage,  ///< Ask for the next frame that differs.
  };

  /**
   * @brief Decide which screencopy request a capture waits on next.
   *
   * A request that outlived a timeout is kept: with `copy_with_damage` a still picture never
   * answers, and asking again on every timeout would pile requests up in the compositor. It is
   * replaced when the cursor setting changed since it was made. The setting is fixed when a
   * request is created and changing it does not damage the output, so on a still picture the
   * old request would go on answering with the cursor as it was. The replacement is a plain
   * copy, which brings the picture up to date at once.
   *
   * The same holds when no request is out. The last frame was captured with the setting of the
   * last request, so if the setting changed since then, the picture the stream is showing is
   * out of date and the next request is a plain copy as well. This covers a change between two
   * requests, and a request that could not be withdrawn and was answered with the old setting.
   *
   * @param pending Whether a request is still waiting for the compositor.
   * @param requested_cursor Cursor setting the last request was made with, pending or answered.
   * @param cursor Cursor setting wanted now.
   * @param event_driven Whether frames are requested with `copy_with_damage`.
   * @param have_frame Whether this capture has delivered a frame yet.
   * @return The request to wait on.
   */
  screencopy_request_e next_screencopy_request(bool pending, bool requested_cursor, bool cursor, bool event_driven, bool have_frame);

  /**
   * @brief Determine whether a damage-driven request should wait for its budget.
   *
   * @param next_request Earliest time the next frame may be asked for.
   * @param now Current time.
   * @param delay Frame interval of the stream.
   * @return `true` when the caller should sleep until `next_request`. A time further off than one
   *         interval is never waited for. With frame times from `damage_frame_time` the budget does
   *         not get that far ahead; the bound keeps a wrong budget from stalling capture.
   */
  bool should_wait_for_damage_request(
    std::chrono::steady_clock::time_point next_request,
    std::chrono::steady_clock::time_point now,
    std::chrono::nanoseconds delay
  );

  /**
   * @brief Intersect compositor and encoder modifiers to find common formats.
   *
   * @param compositor_modifiers Modifiers supported by the compositor for each format.
   * @param encoder_modifiers Modifiers supported by the encoder for each format.
   * @return Intersection of formats and modifiers supported by both.
   */
  std::map<std::uint32_t, std::vector<std::uint64_t>> intersect_modifiers(
    const std::map<std::uint32_t, std::vector<std::uint64_t>> &compositor_modifiers,
    const std::map<std::uint32_t, std::vector<std::uint64_t>> &encoder_modifiers
  );

  /**
   * @brief Owning pointer for a Wayland display connection.
   */
  using display_internal_t = util::safe_ptr<wl_display, wl_display_disconnect>;

  /**
   * @brief GBM buffer accessors used to export DMA-BUF plane metadata.
   */
  struct gbm_bo_accessors_t {
    int (*get_plane_count)(gbm_bo *bo);  ///< Return the number of memory planes in the buffer.
    int (*get_fd_for_plane)(gbm_bo *bo, int plane);  ///< Duplicate the DMA-BUF descriptor for one plane.
    std::uint32_t (*get_stride_for_plane)(gbm_bo *bo, int plane);  ///< Return the row stride for one plane.
    std::uint32_t (*get_offset)(gbm_bo *bo, int plane);  ///< Return the byte offset for one plane.
    std::uint64_t (*get_modifier)(gbm_bo *bo);  ///< Return the DRM format modifier shared by the planes.
  };

  /**
   * @brief Functions used to open a render node and create/destroy a GBM device on it.
   */
  struct gbm_device_accessors_t {
    int (*open_render_node)(const char *path);  ///< Open the render node and return its descriptor, or -1.
    gbm_device *(*create_device)(int fd);  ///< Create a GBM device on the descriptor, or nullptr.
    void (*destroy_device)(gbm_device *device);  ///< Destroy a GBM device (does not close the descriptor).
  };

  /**
   * @brief Owner of a GBM device together with the render-node descriptor it was created from.
   *
   * `gbm_device_destroy()` does not close the descriptor passed to `gbm_create_device()`,
   * so the descriptor has to be tracked and closed here once the device is gone.
   */
  class gbm_device_t {
  public:
    /**
     * @brief Construct an empty owner that holds neither a device nor a descriptor.
     */
    gbm_device_t() = default;

    /**
     * @brief Copying is disabled: the descriptor and the device have exactly one owner.
     */
    gbm_device_t(const gbm_device_t &) = delete;

    /**
     * @brief Copy assignment is disabled: the descriptor and the device have exactly one owner.
     */
    gbm_device_t &operator=(const gbm_device_t &) = delete;

    /**
     * @brief Destroy the GBM device and close the render-node descriptor.
     */
    ~gbm_device_t();

    /**
     * @brief Open the render node and create the GBM device on it.
     *
     * @param render_path Path of the DRM render node.
     * @param device_accessors Functions used to open the node and create the device.
     * @return `true` when the device is ready, `false` when nothing is held.
     */
    bool init(const std::string &render_path, const gbm_device_accessors_t &device_accessors);

    /**
     * @brief Open a GBM device on the DRM node the compositor named, for capture buffers it can import.
     *
     * Keeps a device that is already open.
     *
     * @param device dev_t of the DRM node to use (its render node is preferred), or no value for the configured
     *        render device (platf::resolve_render_device()).
     * @return `true` when a device is held.
     */
    bool open(std::optional<std::uint64_t> device = std::nullopt);

    /**
     * @brief Destroy the GBM device and close the render-node descriptor.
     */
    void reset();

    /**
     * @return The GBM device, or nullptr.
     */
    gbm_device *get() const {
      return device;
    }

    /**
     * @return The render-node descriptor, or -1.
     */
    int fd() const {
      return drm_fd;
    }

    /**
     * @return `true` when a GBM device is held.
     */
    explicit operator bool() const {
      return device != nullptr;
    }

    std::optional<std::uint64_t> dev;  ///< dev_t of the node opened by open(), if known.

  private:
    gbm_device_accessors_t accessors {};  ///< Functions used to create and destroy the device.
    int drm_fd {-1};  ///< Render-node descriptor the device was created from, or -1.
    gbm_device *device {nullptr};  ///< The GBM device, or nullptr.
  };

  /**
   * @brief Captured Wayland frame metadata and DMA-BUF surface state.
   */
  class frame_t {
  public:
    frame_t();
    /**
     * @brief Release file descriptors and native buffers associated with this frame.
     */
    void destroy();

    egl::surface_descriptor_t sd;  ///< DMA-BUF surface descriptor received from the compositor.
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp;  ///< Capture timestamp associated with the frame.
    std::vector<std::uint8_t> shm_data;  ///< Pixel data copied out of the shared-memory pool.
    std::uint32_t shm_stride {0};  ///< Number of bytes per row in `shm_data`.
    std::uint32_t shm_format {0};  ///< `wl_shm` pixel format describing `shm_data`.
    bool is_shm {false};  ///< Whether this frame carries shared-memory pixels instead of a DMA-BUF.
    bool shm_y_invert {false};  ///< Whether rows are ordered bottom to top (shared memory and, for the encoder, DMA-BUFs).
  };

  /**
   * @brief Validate SHM format, dimensions, stride, and signed protocol limits.
   * @param format Wayland shared-memory pixel format.
   * @param width Buffer width in pixels.
   * @param height Buffer height in pixels.
   * @param stride Buffer row stride in bytes.
   * @return Required pool size, or no value for an unsupported or invalid buffer.
   */
  std::optional<std::size_t> shm_buffer_size(std::uint32_t format, std::uint32_t width, std::uint32_t height, std::uint32_t stride);

  /**
   * @brief Copy shared-memory pixels into a BGRA image without copying row padding.
   * @param frame Captured shared-memory frame.
   * @param img Destination BGRA image.
   * @return True when the source and destination layouts are valid.
   */
  bool convert_shm_frame(const frame_t &frame, platf::img_t &img);

  /**
   * @brief Reverse the row order of an image in place (a Y-inverted frame read back into system memory).
   * @param img Image to flip; rows are `row_pitch` bytes apart.
   */
  void flip_rows(platf::img_t &img);

  /**
   * @brief Export every GBM buffer plane into a captured frame descriptor.
   *
   * @param bo GBM buffer whose DMA-BUF planes will be exported.
   * @param frame Frame that takes ownership of the exported file descriptors.
   * @param accessors GBM accessors used to query the buffer.
   * @return Number of exported planes, or no value when the buffer cannot be exported.
   */
  std::optional<std::uint32_t> export_gbm_bo_planes(gbm_bo *bo, frame_t &frame, const gbm_bo_accessors_t &accessors);

  /**
   * @brief Listener state for Wayland screencopy frames backed by DMA-BUFs.
   */
  class dmabuf_t {
  public:
    /**
     * @brief Capture state for the active screencopy request.
     */
    enum status_e {
      WAITING,  ///< Waiting for a frame
      READY,  ///< Frame is ready
      REINIT,  ///< Reinitialize the frame
    };

    dmabuf_t();
    ~dmabuf_t();

    dmabuf_t(dmabuf_t &&) = delete;
    dmabuf_t(const dmabuf_t &) = delete;
    dmabuf_t &operator=(const dmabuf_t &) = delete;
    dmabuf_t &operator=(dmabuf_t &&) = delete;

    /**
     * @brief Start a screencopy request and attach the DMA-BUF listener callbacks.
     *
     * @param screencopy_manager Compositor screencopy manager used to request frames.
     * @param dmabuf_interface_in Compositor DMA-BUF interface used to allocate buffers.
     * @param compositor_formats_in DMA-BUF format modifiers supported by the compositor.
     * @param shm_interface_in Compositor shared-memory interface used for the fallback path.
     * @param output Wayland output to capture.
     * @param blend_cursor Whether the compositor should include the cursor in the frame.
     * @param encoder_formats_in Optional modifiers supported by the encoder for format intersection.
     * @param prefer_linear_copy_in Allocate linear copy buffers for cross-GPU encode import.
     */
    void listen(zwlr_screencopy_manager_v1 *screencopy_manager, zwp_linux_dmabuf_v1 *dmabuf_interface_in, const std::map<std::uint32_t, std::vector<std::uint64_t>> *compositor_formats_in, wl_shm *shm_interface_in, wl_output *output, bool blend_cursor = false, const std::map<std::uint32_t, std::vector<std::uint64_t>> *encoder_formats_in = nullptr, bool prefer_linear_copy_in = false);
    /**
     * @brief Store the Wayland buffer created for a DMA-BUF parameter request.
     *
     * @param data Pointer to the `dmabuf_t` instance associated with the callback.
     * @param params DMA-BUF parameter object that completed buffer creation.
     * @param wl_buffer Wayland buffer created from the DMA-BUF planes.
     */
    static void buffer_params_created(void *data, struct zwp_linux_buffer_params_v1 *params, struct wl_buffer *wl_buffer);
    /**
     * @brief Mark DMA-BUF buffer creation as failed.
     *
     * @param data Pointer to the `dmabuf_t` instance associated with the callback.
     * @param params DMA-BUF parameter object that failed buffer creation.
     */
    static void buffer_params_failed(void *data, struct zwp_linux_buffer_params_v1 *params);
    /**
     * @brief Record DMA-BUF buffer parameters from the compositor.
     *
     * @param frame Screencopy frame that advertised shared-memory fallback data.
     * @param format DRM pixel format for the shared-memory buffer.
     * @param width Frame width in pixels.
     * @param height Frame height in pixels.
     * @param stride Number of bytes per row in the buffer.
     */
    void buffer(zwlr_screencopy_frame_v1 *frame, std::uint32_t format, std::uint32_t width, std::uint32_t height, std::uint32_t stride);
    /**
     * @brief Record Linux DMA-BUF parameters advertised for the frame.
     *
     * @param frame Screencopy frame that advertised DMA-BUF data.
     * @param format DRM pixel format for the DMA-BUF.
     * @param width Frame width in pixels.
     * @param height Frame height in pixels.
     */
    void linux_dmabuf(zwlr_screencopy_frame_v1 *frame, std::uint32_t format, std::uint32_t width, std::uint32_t height);
    /**
     * @brief Allocate or import the buffer once the compositor finished describing it.
     *
     * @param frame Screencopy frame whose buffer description is complete.
     */
    void buffer_done(zwlr_screencopy_frame_v1 *frame);
    /**
     * @brief Record frame flags reported by the compositor.
     *
     * @param frame Screencopy frame that reported flags.
     * @param flags Wayland screencopy flags such as vertical inversion.
     */
    void flags(zwlr_screencopy_frame_v1 *frame, std::uint32_t flags);
    /**
     * @brief Record damaged frame bounds reported by the compositor.
     *
     * @param frame Screencopy frame that reported damage.
     * @param x Left edge of the damaged rectangle.
     * @param y Top edge of the damaged rectangle.
     * @param width Damaged rectangle width in pixels.
     * @param height Damaged rectangle height in pixels.
     */
    void damage(zwlr_screencopy_frame_v1 *frame, std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height);
    /**
     * @brief Mark the current screencopy frame as ready and store its timestamp.
     *
     * @param frame Screencopy frame that completed.
     * @param tv_sec_hi High 32 bits of the compositor-provided seconds value.
     * @param tv_sec_lo Low 32 bits of the compositor-provided seconds value.
     * @param tv_nsec Nanosecond component of the compositor-provided timestamp.
     */
    void ready(zwlr_screencopy_frame_v1 *frame, std::uint32_t tv_sec_hi, std::uint32_t tv_sec_lo, std::uint32_t tv_nsec);
    /**
     * @brief Retry a failed DMA-BUF copy once with SHM, or request reinitialization.
     *
     * @param frame Screencopy frame that failed.
     */
    void failed(zwlr_screencopy_frame_v1 *frame);

    /**
     * @brief Withdraw the request that is waiting for the compositor to copy a frame.
     *
     * Only a request whose copy has been asked for can be withdrawn. Before that the buffer for it
     * is still being created, and its callback refers to the request.
     *
     * @return `true` when the request was withdrawn and another may be made.
     */
    bool cancel();

    /**
     * @brief Select the inactive frame slot for the next screencopy request.
     *
     * @return Inactive frame buffer that can receive the next capture.
     */
    frame_t *get_next_frame() {
      return current_frame == &frames[0] ? &frames[1] : &frames[0];
    }

    status_e status {READY};  ///< Current state of the active screencopy request, initially ready.
    bool allow_dmabuf {true};  ///< Whether the capture consumer can import DMA-BUF frames.
    bool with_damage {false};  ///< Ask the compositor for the next frame that differs, rather than for a frame now.
    std::array<frame_t, 2> frames {};  ///< Double-buffered frame descriptors.
    frame_t *current_frame;  ///< Frame descriptor currently being filled by the compositor.
    zwlr_screencopy_frame_v1_listener listener;  ///< Callback table registered on screencopy frames.

  private:
    bool init_gbm();
    void cleanup_gbm();
    bool create_and_copy_dmabuf(zwlr_screencopy_frame_v1 *frame);
    void create_and_copy_shm(zwlr_screencopy_frame_v1 *frame);
    bool init_shm(std::size_t size);
    void cleanup_shm();
    void destroy_shm();

    struct {
      zwlr_screencopy_manager_v1 *manager {nullptr};  ///< Non-owning factory for a fresh capture after a terminal copy failure.
      wl_output *output {nullptr};  ///< Output retained for the bounded capture retry.
      bool blend_cursor {false};  ///< Cursor option retained for the bounded capture retry.
    } capture_context;  ///< Parameters needed to replace a terminal capture request.

    zwp_linux_dmabuf_v1 *dmabuf_interface {nullptr};
    const std::map<std::uint32_t, std::vector<std::uint64_t>> *supported_modifiers {nullptr};
    const std::map<std::uint32_t, std::vector<std::uint64_t>> *encoder_modifiers {nullptr};  ///< Optional encoder modifiers for intersection.
    bool prefer_linear_copy {false};  ///< Allocate linear copy buffers for cross-GPU encode import.
    bool gbm_failed {false};  ///< Stay on SHM after DMA-BUF allocation, import or copy fails.

    struct {
      bool supported {false};
      std::uint32_t format;
      std::uint32_t width;
      std::uint32_t height;
      std::uint32_t stride;
    } shm_info;

    struct {
      bool supported {false};
      std::uint32_t format;
      std::uint32_t width;
      std::uint32_t height;
    } dmabuf_info;

    gbm_device_t gbm;  ///< GBM device and render-node descriptor used to allocate capture buffers.
    struct gbm_bo *current_bo {nullptr};
    struct wl_buffer *current_wl_buffer {nullptr};
    zwlr_screencopy_frame_v1 *pending_copy {nullptr};  ///< Request whose copy is asked for and not yet answered.
    bool y_invert {false};

    struct {
      wl_shm *interface {nullptr};  ///< Non-owning factory for shared-memory buffers.
      int fd {-1};  ///< File descriptor backing the shared-memory pool.
      std::uint8_t *mapping {nullptr};  ///< Mapped pixel storage.
      std::size_t size {0};  ///< Size of the mapped pixel storage.
      wl_shm_pool *pool {nullptr};  ///< Per-frame Wayland pool referencing the mapping.
      wl_buffer *buffer {nullptr};  ///< Per-frame copy destination.
      bool active {false};  ///< Whether the pending copy uses shared memory.
    } shm;  ///< Shared-memory resources and capture mode.
  };

  class display_t;

  /**
   * @brief A buffer the compositor copies frames into: a GBM dma-buf or a shared memory buffer.
   */
  class capture_buffer_t {
  public:
    ~capture_buffer_t();

    capture_buffer_t(const capture_buffer_t &) = delete;
    capture_buffer_t &operator=(const capture_buffer_t &) = delete;

    /**
     * @brief Allocates a dma-buf with GBM and wraps it in a wl_buffer through linux-dmabuf.
     * @details Roundtrips with the compositor, so it must not be called from inside a Wayland callback.
     */
    static std::unique_ptr<capture_buffer_t> make_dmabuf(display_t &display, zwp_linux_dmabuf_v1 *linux_dmabuf, struct gbm_device *device, const buffer_spec_t &spec);

    /**
     * @brief Allocates a memfd backed wl_shm buffer.
     */
    static std::unique_ptr<capture_buffer_t> make_shm(wl_shm *shm, const buffer_spec_t &spec);

    /**
     * @brief Copies the dma-buf description with duplicated file descriptors; the caller owns them.
     * @return false if a descriptor could not be duplicated (`out` then owns none).
     */
    bool export_descriptor(egl::surface_descriptor_t &out) const;

    buffer_spec_t spec;
    wl_buffer *buffer {nullptr};

    /// dma-buf planes; the file descriptors belong to this buffer.
    egl::surface_descriptor_t sd;

    /// shm mapping.
    std::uint8_t *data {nullptr};
    std::size_t size {0};
    std::uint32_t stride {0};

  private:
    capture_buffer_t();

    struct gbm_bo *bo {nullptr};
  };

  /**
   * @brief A small pool of capture buffers reused from frame to frame (see buffer_slots_t).
   */
  class buffer_pool_t {
  public:
    explicit buffer_pool_t(std::size_t max_buffers):
        slots {max_buffers},
        max_buffers {max_buffers} {
    }

    /**
     * @brief Sets what buffers to allocate. Existing buffers are dropped if the spec changed.
     */
    void configure(const buffer_spec_t &spec);

    enum class acquire_e {
      ok,  ///< `index` is a slot with an allocated buffer, now marked as capturing.
      busy,  ///< Every buffer is in use; try again once the encoder released one.
      failed,  ///< Allocation failed.
    };

    /**
     * @brief Picks a buffer to capture into, allocating it if needed.
     * @details May roundtrip with the compositor, so it must not be called from inside a Wayland callback.
     */
    acquire_e acquire(display_t &display, zwp_linux_dmabuf_v1 *linux_dmabuf, wl_shm *shm, struct gbm_device *device, std::size_t &index);

    capture_buffer_t &operator[](std::size_t index) {
      return *buffers[index];
    }

    bool configured() const {
      return spec.width && spec.height;
    }

    buffer_spec_t spec;
    buffer_slots_t slots;

    /// Number of buffers allocated since construction (including ones dropped by configure()).
    std::size_t allocations {0};

  private:
    std::size_t max_buffers;
    std::vector<std::unique_ptr<capture_buffer_t>> buffers;
  };

  /**
   * @brief Capture statistics exposed by the Wayland capture backends (for tests and diagnostics).
   */
  struct capture_stats_t {
    std::size_t buffers_allocated {0};  ///< Capture buffers allocated since the display was created.
    std::size_t frames_captured {0};  ///< Frames completed by the compositor.
    std::size_t frames_delivered {0};  ///< Frames handed to the encoder.
  };

  class capture_stats_source_t {
  public:
    virtual ~capture_stats_source_t() = default;
    virtual capture_stats_t capture_stats() const = 0;
  };

  /**
   * @brief Wayland output metadata used to match a configured display name.
   */
  class monitor_t {
  public:
    /**
     * @brief Track a Wayland output and its advertised modes.
     *
     * @param output Wayland output object being described.
     */
    explicit monitor_t(wl_output *output);

    monitor_t(monitor_t &&) = delete;
    monitor_t(const monitor_t &) = delete;
    monitor_t &operator=(const monitor_t &) = delete;
    monitor_t &operator=(monitor_t &&) = delete;

    /**
     * @brief Attach xdg-output and wl-output listeners for this monitor. Does nothing the second time.
     *
     * @param output_manager xdg-output manager used to query logical monitor metadata.
     */
    void listen(zxdg_output_manager_v1 *output_manager);

    /**
     * @brief Whether listen() attached the listeners.
     *
     * @return `true` once the monitor receives output events.
     */
    bool listening() const {
      return xdg_output != nullptr;
    }

    /**
     * @brief Store the xdg-output logical monitor name.
     *
     * @param name Human-readable name to assign.
     */
    void xdg_name(zxdg_output_v1 *, const char *name);
    /**
     * @brief Store the xdg-output human-readable monitor description.
     *
     * @param description Human-readable description used in log output.
     */
    void xdg_description(zxdg_output_v1 *, const char *description);
    /**
     * @brief Store the logical monitor position reported by xdg-output.
     *
     * @param x Horizontal coordinate reported by Wayland.
     * @param y Vertical coordinate reported by Wayland.
     */
    void xdg_position(zxdg_output_v1 *, std::int32_t x, std::int32_t y);
    /**
     * @brief Store the logical monitor size reported by xdg-output.
     *
     * @param width Frame or display width in pixels.
     * @param height Frame or display height in pixels.
     */
    void xdg_size(zxdg_output_v1 *, std::int32_t width, std::int32_t height);

    /**
     * @brief Receive completion of an xdg-output metadata update.
     */
    void xdg_done(zxdg_output_v1 *) {
      // Size and position callbacks already store the metadata; completion needs no action.
    }

    /**
     * @brief Record the wl-output transform; monitor placement comes from xdg-output metadata.
     * @details Physical geometry is ignored. A rotated output is captured in its native orientation, so absolute
     * input needs the transform to map points onto it.
     * @param transform Wayland output transform value.
     */
    void wl_geometry(wl_output *, std::int32_t, std::int32_t, std::int32_t, std::int32_t, std::int32_t, const char *, const char *, std::int32_t transform);  // NOSONAR(cpp:S107): libwayland's wl_output geometry callback signature.

    /**
     * @brief Store the active pixel mode reported by wl-output.
     *
     * @param wl_output Wayland output.
     * @param flags Bit flags that modify the requested operation.
     * @param width Frame or display width in pixels.
     * @param height Frame or display height in pixels.
     * @param refresh Output refresh rate in mHz.
     */
    void wl_mode(wl_output *wl_output, std::uint32_t flags, std::int32_t width, std::int32_t height, std::int32_t refresh);

    /**
     * @brief Acknowledge completion of a wl-output metadata update.
     * @details Retain the wl-output listener signature while ignoring the output argument.
     */
    void wl_done(wl_output *) {
      // Metadata is stored as each event arrives; completion requires no additional work.
    }

    /**
     * @brief Store the wl-output integer scale (used when xdg-output gives no logical size).
     *
     * @param factor Integer scale factor of the output.
     */
    void wl_scale(wl_output *, std::int32_t factor);

    /**
     * @brief Store the wl-output connector name (wl_output v4), preferred over the xdg-output name.
     *
     * @param name Connector name, e.g. "DP-3".
     */
    void wl_name(wl_output *, const char *name);

    /**
     * @brief Store the wl-output description (wl_output v4).
     *
     * @param description Human-readable description used in log output.
     */
    void wl_description(wl_output *, const char *description);

    wl_output *output;  ///< Wayland output associated with this monitor.
    /// Connector name (e.g. "DP-3", "HEADLESS-2") used for display selection, from wl_output.name (v4) or else
    /// zxdg_output_v1.name. Empty if the compositor supports neither.
    std::string name;
    std::string description;  ///< Output description used for logs and UI.
    platf::touch_port_t viewport;  ///< Logical monitor bounds used to scale absolute input.
    output_geometry_t geometry;  ///< Logical position/size, mode size, transform and scale of this output.
    std::int32_t refresh_mhz {0};  ///< Refresh rate of the current mode in mHz, 0 when the compositor did not say.
    std::uint32_t global_name {0};  ///< Registry name of the wl_output global, matched against global_remove.
    bool removed {false};  ///< The compositor removed the output (wl_registry.global_remove).
    wl_output_listener wl_listener;  ///< Callback table for wl-output events.
    zxdg_output_v1_listener xdg_listener;  ///< Callback table for xdg-output events.

  private:
    zxdg_output_v1 *xdg_output {nullptr};  ///< xdg-output object, once listen() created it.
    bool has_wl_name {false};  ///< `name` came from wl_output.name and is kept over the xdg-output name.
  };

  /**
   * @brief Copy a monitor's geometry onto the display that captures it.
   *
   * Capture initialization uses this, so a reinitialization picks up the monitor's current transform for absolute
   * input.
   *
   * @param display Capture display to update.
   * @param monitor Monitor selected for capture.
   */
  void apply_monitor_geometry(platf::display_t &display, const monitor_t &monitor);

  /**
   * @brief Wayland registry state for screencopy, DMA-BUF, and output globals.
   */
  class interface_t {
    struct bind_t {
      std::uint32_t id;
      std::uint32_t version;
    };

  public:
    /**
     * @brief Wayland globals required by the WLR capture backend.
     */
    enum interface_e {
      XDG_OUTPUT,  ///< xdg-output
      WLR_EXPORT_DMABUF,  ///< screencopy manager
      LINUX_DMABUF,  ///< linux-dmabuf protocol
      EXT_IMAGE_COPY_CAPTURE,  ///< ext-image-copy-capture manager
      EXT_OUTPUT_CAPTURE_SOURCE,  ///< ext-image-capture-source output source manager
      SHM,  ///< wl_shm
      COLOR_MANAGER,  ///< wp_color_manager_v1 (only bound when bind_color_manager is set)
      MAX_INTERFACES,  ///< Maximum number of interfaces
    };

    interface_t() noexcept;

    interface_t(interface_t &&) = delete;
    interface_t(const interface_t &) = delete;
    interface_t &operator=(const interface_t &) = delete;
    interface_t &operator=(interface_t &&) = delete;

    /**
     * @brief Discover compositor globals from the Wayland registry.
     *
     * @param registry Wayland registry announcing globals.
     */
    void listen(wl_registry *registry);

    /**
     * @brief Test whether a required Wayland global was advertised.
     *
     * @param bit Interface bit to test.
     * @return True when the requested Wayland interface bit is set.
     */
    bool operator[](interface_e bit) const {
      return interface[bit];
    }

    /**
     * @brief Record a DMA-BUF format advertised by the compositor.
     *
     * @param zwp_linux_dmabuf DMA-BUF interface that emitted the format event.
     * @param format DRM format supported by the compositor.
     */
    void dmabuf_format(zwp_linux_dmabuf_v1 *zwp_linux_dmabuf, uint32_t format);
    /**
     * @brief Record an explicit DMA-BUF format modifier advertised by the compositor.
     *
     * @param zwp_linux_dmabuf DMA-BUF interface that emitted the modifier event.
     * @param format DRM format associated with the modifier.
     * @param modifier_hi High 32 bits of the DRM format modifier.
     * @param modifier_lo Low 32 bits of the DRM format modifier.
     * @note The combined DRM_FORMAT_MOD_INVALID value is omitted because implicit allocation uses the fallback path.
     */
    void dmabuf_modifier(zwp_linux_dmabuf_v1 *zwp_linux_dmabuf, uint32_t format, uint32_t modifier_hi, uint32_t modifier_lo);

    /// Outputs discovered from the Wayland registry. A removed output stays in the list, marked `removed`, so that
    /// pointers to it stay valid until the capture that uses it is reinitialized.
    std::vector<std::unique_ptr<monitor_t>> monitors;
    std::map<std::uint32_t, std::vector<std::uint64_t>> supported_modifiers;  ///< DRM format modifiers grouped by format.
    zwlr_screencopy_manager_v1 *screencopy_manager {nullptr};  ///< WLR screencopy global used to request frames.
    std::uint32_t screencopy_version {0};  ///< Version of the screencopy global the compositor offered.
    zwp_linux_dmabuf_v1 *dmabuf_interface {nullptr};  ///< Linux DMA-BUF global used to allocate frame buffers.
    zxdg_output_manager_v1 *output_manager {nullptr};  ///< xdg-output global used to query monitor names and sizes.
    wl_shm *shm_interface {nullptr};  ///< Shared-memory global used when DMA-BUF allocation is unavailable.
    ext_image_copy_capture_manager_v1 *ext_copy_manager {nullptr};  ///< ext-image-copy-capture manager.
    ext_output_image_capture_source_manager_v1 *ext_output_source_manager {nullptr};  ///< ext output capture sources.

    /**
     * @brief Bind wp_color_manager_v1. Set before listen().
     * @details Off by default: Hyprland treats every client that bound the color manager as color-management aware
     *          and then hands it captured frames in the output's own color space instead of converting them to sRGB
     *          (ScreenshareFrame.cpp, `cmBackToSRGB`). Capture connections therefore leave it unbound; output_color_t
     *          uses a connection of its own.
     */
    bool bind_color_manager {false};
    wp_color_manager_v1 *color_manager {nullptr};  ///< Color manager, when bind_color_manager is set.
    /// What the color manager advertised (supported_tf_named, supported_primaries_named, supported_feature).
    std::vector<std::uint32_t> color_tfs, color_primaries, color_features;

  private:
    /**
     * @brief Bind supported globals advertised by the Wayland registry.
     *
     * @param registry Registry advertising the global.
     * @param id Numeric name used to bind the global.
     * @param interface_name Protocol interface name advertised by the compositor.
     * @param version Protocol version advertised for the global.
     */
    void add_interface(wl_registry *registry, std::uint32_t id, const char *interface_name, std::uint32_t version);
    void del_interface(wl_registry *registry, uint32_t id);

    std::bitset<MAX_INTERFACES> interface;
    wl_registry_listener listener;
    zwp_linux_dmabuf_v1_listener dmabuf_listener;
  };

  /**
   * @brief What a capture was set up for: its output's state and the layout of all outputs.
   * @details The registry and output listeners stay attached while capture dispatches events, so the monitors keep
   *          following the compositor. output_change() compares them with this.
   */
  struct output_watch_t {
    const monitor_t *monitor {nullptr};  ///< Captured output.
    output_geometry_t geometry;  ///< Its position, sizes, transform and scale at setup.
    std::int32_t refresh_mhz {0};  ///< Its refresh rate at setup.
    layout_bounds_t layout {};  ///< Bounds of the layout of all outputs at setup (absolute input maps onto it).
  };

  /**
   * @brief Record the state of a captured output and of the output layout.
   *
   * @param interface Registry state with all outputs.
   * @param monitor Captured output, one of `interface.monitors`.
   * @return State to compare with output_change().
   */
  output_watch_t watch_output(const interface_t &interface, const monitor_t &monitor);

  /**
   * @brief Tell whether the compositor changed the captured output (or the layout) in a way that needs a new capture.
   *
   * Removal, a new mode (size or refresh rate), a new scale (integer or fractional, seen in the logical size), a new
   * transform or position of the output, and a change of the layout bounds (an output added, removed or moved)
   * each change the frames or the mapping of absolute input.
   *
   * @param watch State at setup, from watch_output().
   * @param interface Registry state with all outputs, as the listeners keep it.
   * @return What changed, or nothing.
   */
  std::optional<std::string_view> output_change(const output_watch_t &watch, const interface_t &interface);

  /**
   * @brief Wayland display connection used to dispatch capture events.
   */
  class display_t {
  public:
    /**
     * @brief Connect to the requested Wayland display.
     *
     * @param display_name Wayland display name, or nullptr to use WAYLAND_DISPLAY.
     * @return 0 when the connection is opened and initialized; -1 on failure.
     */
    int init(const char *display_name = nullptr);

    // Roundtrip with Wayland connection
    /**
     * @brief Flush pending Wayland requests and wait for replies.
     */
    void roundtrip();

    // Wait up to the timeout to read and dispatch new events
    bool dispatch(std::chrono::milliseconds timeout);

    /// True once the connection failed (compositor gone, protocol error): nothing will be dispatched any more.
    bool broken();

    /**
     * @brief Return the Wayland registry for global discovery.
     *
     * @return Wayland registry used to discover compositor globals.
     */
    wl_registry *registry();

    /**
     * @brief Return the native Wayland display pointer.
     *
     * @return Native display connection owned by this wrapper.
     */
    inline display_internal_t::pointer get() {
      return display_internal.get();
    }

  private:
    display_internal_t display_internal;
  };

  /**
   * @brief Refresh the monitor list reported by the display server.
   *
   * @param display_name Display name.
   * @return Monitors reported by the Wayland compositor, optionally filtered by name.
   */
  std::vector<std::unique_ptr<monitor_t>> monitors(const char *display_name = nullptr);
  /**
   * @brief The color description of one output, from wp-color-management-v1.
   * @details Uses a Wayland connection of its own (see interface_t::bind_color_manager), so it can live next to a
   *          capture connection without changing what that one receives. Events are only read in open() and
   *          poll_changed(); there is no thread.
   */
  class output_color_t {
  public:
    output_color_t() = default;
    ~output_color_t();

    output_color_t(const output_color_t &) = delete;
    output_color_t &operator=(const output_color_t &) = delete;

    /**
     * @brief Connects, finds the output by connector name and fetches its image description.
     * @return false if the compositor has no wp_color_manager_v1, the output does not exist, or the description
     *         could not be read. description() is empty then.
     */
    bool open(const std::string &output_name, std::chrono::milliseconds timeout = std::chrono::seconds {1});

    /**
     * @brief Reads pending events without blocking. On image_description_changed the description is fetched again.
     * @return true if the description is now different from before.
     */
    bool poll_changed();

    const std::optional<image_description_t> &description() const {
      return desc;
    }

    /// The output's description is HDR10 (ST 2084 PQ with BT.2020 primaries).
    bool is_hdr() const {
      return desc && is_hdr10(*desc);
    }

    /// The compositor can describe outputs as PQ / BT.2020, i.e. it supports HDR outputs at all.
    bool compositor_supports_hdr10() const;

    /**
     * @brief For an HDR output: where white of the captured frames belongs in the HDR stream, in cd/m².
     * @details Capture clients do not get the PQ framebuffer of an HDR output. Hyprland (0.57.2 and its main branch
     *          as of 2026-11) copies frames out of a mirror buffer whose image description is sRGB whenever the output
     *          uses PQ (CMonitorResources::getMirrorTexImageDescription()), converting and tone mapping the HDR
     *          content to SDR, for color-management aware clients as well. Measured on a headless HDR output with
     *          a PQ test pattern: 974, 102 and 5 cd/m² patches came back as sRGB 235, 186 and 71 (PQ would be 191,
     *          130 and 64), and SDR content came back exactly as on an SDR output. The frames are therefore SDR with
     *          white at the output's reference luminance (203 cd/m², from the image description), which is where
     *          Hyprland puts SDR white on the HDR output.
     * @return 0 for SDR outputs.
     */
    float sdr_white_nits() const;

  private:
    std::optional<image_description_t> fetch(std::chrono::milliseconds timeout);
    void reset();

    display_t display;
    std::unique_ptr<interface_t> interface;
    wp_color_management_output_v1 *cm_output {nullptr};
    std::optional<image_description_t> desc;
    bool changed {false};
  };

  /**
   * @brief Logs an image description in one line.
   */
  std::string describe(const image_description_t &desc);

  /**
   * @brief Connects the registry, binds the globals and fetches every output's name and geometry.
   * @return false if xdg-output is missing or there are no outputs.
   */
  bool enumerate_outputs(display_t &display, interface_t &interface);

  /**
   * @brief Names used to select outputs: the connector name, or the enumeration index if the compositor
   *        did not report a name.
   */
  std::vector<std::string> output_names(const interface_t &interface);

  /**
   * @brief Finds the output to stream by connector name (or legacy index). An empty name selects the first output.
   * @return The output's index in interface.monitors, or std::nullopt (logged) if it does not exist.
   */
  std::optional<std::size_t> select_output(const interface_t &interface, const std::string &display_name);

  /**
   * @brief Fills a display's size and input geometry for the selected output (ext-image-copy-capture).
   * @param frame Size of the captured frames in hardware pixels, in the orientation the compositor delivers them.
   * @details Everything handed to input is in the compositor's logical (scaled) layout space;
   *          see input::touch_port_t for how these are used.
   */
  void apply_output_geometry(platf::display_t &display, const interface_t &interface, std::size_t index, extent_t frame);

  /**
   * @brief Logs the outputs (for the display list in the web UI's troubleshooting log).
   */
  void log_outputs(const interface_t &interface, const std::vector<std::string> &names);

  /**
   * @brief Copies a captured buffer into a system memory BGRA image.
   * @param egl_display Display used to import dma-bufs; its context must be current. Unused for shm buffers.
   * @param y_invert The buffer is stored bottom row first.
   * @return false if the dma-buf could not be imported.
   */
  bool read_buffer(egl::display_t::pointer egl_display, const capture_buffer_t &buffer, platf::img_t &img, bool y_invert);

  /**
   * @brief Hands the ready dma-buf in `slot` to the encoder through an egl::img_descriptor_t.
   * @details The image gets duplicated file descriptors, and the slot is not reused while anyone besides
   *          the capture image pool still holds the image.
   * @return false if the file descriptors could not be duplicated.
   */
  bool hand_over_buffer(buffer_pool_t &pool, std::size_t slot, const std::shared_ptr<platf::img_t> &img, std::uint64_t sequence, bool y_invert);

  /**
   * @brief Options for the ext-image-copy-capture backend.
   */
  struct ext_options_t {
    /// Use shared memory buffers for system memory capture even when dma-bufs are possible.
    bool prefer_shm {false};
  };

  /**
   * @brief Creates an ext-image-copy-capture display (platf::ext_display() with options).
   */
  std::shared_ptr<platf::display_t> make_ext_display(platf::mem_type_e hwdevice_type, const std::string &display_name, const ::video::config_t &config, const ext_options_t &options);

  /**
   * @brief Initialize Wayland registry interfaces required for capture.
   *
   * @return 0 on success; nonzero or negative platform status on failure.
   */
  int init();
}  // namespace wl
#else

struct wl_output;
struct zxdg_output_manager_v1;

namespace wl {
  class monitor_t {
  public:
    monitor_t(wl_output *output);

    monitor_t(monitor_t &&) = delete;
    monitor_t(const monitor_t &) = delete;
    monitor_t &operator=(const monitor_t &) = delete;
    monitor_t &operator=(monitor_t &&) = delete;

    void listen(zxdg_output_manager_v1 *output_manager);

    wl_output *output;
    std::string name;
    std::string description;
    platf::touch_port_t viewport;
  };

  inline std::vector<std::unique_ptr<monitor_t>> monitors(const char *display_name = nullptr) {
    return {};
  }

  inline int init() {
    return -1;
  }
}  // namespace wl
#endif
