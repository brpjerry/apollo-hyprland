/**
 * @file src/platform/linux/wayland.h
 * @brief Declarations for Wayland capture.
 */
#pragma once

// standard includes
#include <bitset>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
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

struct gbm_device;
struct gbm_bo;

namespace platf {
  class display_t;
}

namespace wl {
  using display_internal_t = util::safe_ptr<wl_display, wl_display_disconnect>;

  class display_t;

  /**
   * @brief A GBM device to allocate capture buffers on.
   */
  class gbm_device_t {
  public:
    gbm_device_t() = default;
    ~gbm_device_t();

    gbm_device_t(const gbm_device_t &) = delete;
    gbm_device_t &operator=(const gbm_device_t &) = delete;

    /**
     * @brief Opens the device.
     * @param device dev_t of the DRM node to use (its render node is preferred), or std::nullopt for the
     *        first render node.
     * @return true on success.
     */
    bool open(std::optional<std::uint64_t> device = std::nullopt);

    struct gbm_device *get() const {
      return device;
    }

    /// dev_t of the opened node, if known.
    std::optional<std::uint64_t> dev;

  private:
    struct gbm_device *device {nullptr};
    int fd {-1};
  };

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
   * @brief Buffer constraints of a wlr-screencopy frame (the buffer, linux_dmabuf and buffer_done events).
   */
  struct screencopy_constraints_t {
    struct shm_t {
      std::uint32_t format;
      std::uint32_t width;
      std::uint32_t height;
      std::uint32_t stride;

      friend bool operator==(const shm_t &, const shm_t &) = default;
    };

    struct dmabuf_t {
      std::uint32_t format;
      std::uint32_t width;
      std::uint32_t height;

      friend bool operator==(const dmabuf_t &, const dmabuf_t &) = default;
    };

    std::optional<shm_t> shm;
    std::optional<dmabuf_t> dmabuf;

    friend bool operator==(const screencopy_constraints_t &, const screencopy_constraints_t &) = default;
  };

  /**
   * @brief Buffer spec for wlr-screencopy frames with the given constraints.
   * @param allow_dmabuf A GBM device and linux-dmabuf are available.
   * @param allow_shm Shared memory buffers are acceptable (system memory capture).
   */
  std::optional<buffer_spec_t> screencopy_buffer_spec(const screencopy_constraints_t &constraints, bool allow_dmabuf, bool allow_shm);

  /**
   * @brief wlr-screencopy capture of one output.
   * @details At most one frame is in flight: when snapshot() times out, the frame stays pending and the next
   *          snapshot() keeps waiting for it instead of starting another one. Frames are copied into buffers
   *          from a reused pool.
   */
  class screencopy_t {
  public:
    enum status_e {
      IDLE,  ///< No frame in flight
      WAITING,  ///< Waiting for a frame
      READY,  ///< Frame is ready
      REINIT,  ///< Reinitialize the frame
    };

    explicit screencopy_t(std::size_t max_buffers);
    ~screencopy_t();

    screencopy_t(screencopy_t &&) = delete;
    screencopy_t(const screencopy_t &) = delete;
    screencopy_t &operator=(const screencopy_t &) = delete;
    screencopy_t &operator=(screencopy_t &&) = delete;

    /**
     * @brief Starts capturing a frame unless one is already in flight.
     * @return The buffer pool's answer: `busy` if no buffer is free yet; `failed` sets status to REINIT.
     */
    buffer_pool_t::acquire_e start(display_t &display, zwlr_screencopy_manager_v1 *screencopy_manager, zwp_linux_dmabuf_v1 *linux_dmabuf, wl_shm *shm, struct gbm_device *device, wl_output *output, bool blend_cursor);

    void buffer(zwlr_screencopy_frame_v1 *frame, std::uint32_t format, std::uint32_t width, std::uint32_t height, std::uint32_t stride);
    void linux_dmabuf(zwlr_screencopy_frame_v1 *frame, std::uint32_t format, std::uint32_t width, std::uint32_t height);
    void buffer_done(zwlr_screencopy_frame_v1 *frame);
    void flags(zwlr_screencopy_frame_v1 *frame, std::uint32_t flags);
    void damage(zwlr_screencopy_frame_v1 *frame, std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height);
    void ready(zwlr_screencopy_frame_v1 *frame, std::uint32_t tv_sec_hi, std::uint32_t tv_sec_lo, std::uint32_t tv_nsec);
    void failed(zwlr_screencopy_frame_v1 *frame);

    status_e status {IDLE};
    buffer_pool_t pool;

    /// The y_invert flag of the frame in the ready slot.
    bool y_invert {false};

  private:
    void abort_frame();

    zwlr_screencopy_frame_v1_listener listener;
    zwlr_screencopy_frame_v1 *frame {nullptr};
    std::size_t frame_slot {0};
    screencopy_constraints_t announced;
    bool frame_y_invert {false};
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

  class monitor_t {
  public:
    explicit monitor_t(wl_output *output);

    monitor_t(monitor_t &&) = delete;
    monitor_t(const monitor_t &) = delete;
    monitor_t &operator=(const monitor_t &) = delete;
    monitor_t &operator=(monitor_t &&) = delete;

    void listen(zxdg_output_manager_v1 *output_manager);
    void xdg_name(zxdg_output_v1 *, const char *name);
    void xdg_description(zxdg_output_v1 *, const char *description);
    void xdg_position(zxdg_output_v1 *, std::int32_t x, std::int32_t y);
    void xdg_size(zxdg_output_v1 *, std::int32_t width, std::int32_t height);

    void xdg_done(zxdg_output_v1 *) {}

    void wl_geometry(wl_output *wl_output, std::int32_t x, std::int32_t y, std::int32_t physical_width, std::int32_t physical_height, std::int32_t subpixel, const char *make, const char *model, std::int32_t transform);

    void wl_mode(wl_output *wl_output, std::uint32_t flags, std::int32_t width, std::int32_t height, std::int32_t refresh);

    void wl_done(wl_output *wl_output) {}

    void wl_scale(wl_output *wl_output, std::int32_t factor);

    void wl_name(wl_output *wl_output, const char *name);

    void wl_description(wl_output *wl_output, const char *description);

    wl_output *output;

    /// Connector name (e.g. "DP-3", "HEADLESS-2"), from wl_output.name (v4) or else zxdg_output_v1.name.
    /// Empty if the compositor supports neither.
    std::string name;
    std::string description;

    /// Logical position/size, mode size, transform and scale of this output.
    output_geometry_t geometry;

    wl_output_listener wl_listener;
    zxdg_output_v1_listener xdg_listener;

  private:
    bool has_wl_name {false};
  };

  class interface_t {
    struct bind_t {
      std::uint32_t id;
      std::uint32_t version;
    };

  public:
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

    void listen(wl_registry *registry);

    bool operator[](interface_e bit) const {
      return interface[bit];
    }

    std::vector<std::unique_ptr<monitor_t>> monitors;
    zwlr_screencopy_manager_v1 *screencopy_manager {nullptr};
    zwp_linux_dmabuf_v1 *dmabuf_interface {nullptr};
    zxdg_output_manager_v1 *output_manager {nullptr};
    ext_image_copy_capture_manager_v1 *ext_copy_manager {nullptr};
    ext_output_image_capture_source_manager_v1 *ext_output_source_manager {nullptr};
    wl_shm *shm {nullptr};

    /**
     * @brief Bind wp_color_manager_v1. Set before listen().
     * @details Off by default: Hyprland treats every client that bound the color manager as color-management aware
     *          and then hands it captured frames in the output's own color space instead of converting them to sRGB
     *          (ScreenshareFrame.cpp, `cmBackToSRGB`). Capture connections therefore leave it unbound; output_color_t
     *          uses a connection of its own.
     */
    bool bind_color_manager {false};
    wp_color_manager_v1 *color_manager {nullptr};
    /// What the color manager advertised (supported_tf_named, supported_primaries_named, supported_feature).
    std::vector<std::uint32_t> color_tfs, color_primaries, color_features;

  private:
    void add_interface(wl_registry *registry, std::uint32_t id, const char *interface, std::uint32_t version);
    void del_interface(wl_registry *registry, uint32_t id);

    std::bitset<MAX_INTERFACES> interface;
    wl_registry_listener listener;
  };

  class display_t {
  public:
    /**
     * @brief Initialize display.
     * If display_name == nullptr -> display_name = std::getenv("WAYLAND_DISPLAY")
     * @param display_name The name of the display.
     * @return 0 on success, -1 on failure.
     */
    int init(const char *display_name = nullptr);

    // Roundtrip with Wayland connection
    void roundtrip();

    // Wait up to the timeout to read and dispatch new events
    bool dispatch(std::chrono::milliseconds timeout);

    // Get the registry associated with the display
    // No need to manually free the registry
    wl_registry *registry();

    inline display_internal_t::pointer get() {
      return display_internal.get();
    }

  private:
    display_internal_t display_internal;
  };

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
   * @brief Ask the compositor what size screencopy frames of an output will have, without copying a frame.
   * @details Compositors differ: Hyprland delivers frames of rotated outputs already rotated (e.g. 2160x3840 for a
   *          3840x2160 mode with transform 1), while others deliver the untransformed mode size.
   * @return The frame size, or `std::nullopt` if the compositor did not announce a buffer.
   */
  std::optional<extent_t> probe_frame_size(display_t &display, zwlr_screencopy_manager_v1 *screencopy_manager, wl_output *output);

  /**
   * @brief Ask the compositor which buffers screencopy frames of an output need, without copying a frame.
   * @return The constraints, or `std::nullopt` if the compositor did not announce any buffer.
   */
  std::optional<screencopy_constraints_t> probe_screencopy(display_t &display, zwlr_screencopy_manager_v1 *screencopy_manager, wl_output *output);

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
   * @brief Fills a display's size and input geometry for the selected output.
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
    output_geometry_t geometry;
  };

  inline std::vector<std::unique_ptr<monitor_t>> monitors(const char *display_name = nullptr) {
    return {};
  }

  inline int init() {
    return -1;
  }
}  // namespace wl
#endif
