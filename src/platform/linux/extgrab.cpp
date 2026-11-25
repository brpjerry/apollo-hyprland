/**
 * @file src/platform/linux/extgrab.cpp
 * @brief Capture through ext-image-copy-capture-v1 (persistent capture session per output).
 * @details One session is kept open for the streamed output. Frames are copied into a small pool of reused
 *          buffers allocated on the device and with the formats and modifiers the session asks for (GBM
 *          dma-bufs, or shared memory for system memory capture). Capture is driven by damage: the compositor
 *          completes a frame when the output content changes, and the frame's presentation time is used as its
 *          timestamp.
 */
// standard includes
#include <cstring>
#include <thread>

// platform includes
#include <sys/sysmacros.h>

// local includes
#include "cuda.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/video.h"
#include "vaapi.h"
#include "wayland.h"

using namespace std::literals;

namespace wl {
  namespace {
    /**
     * @brief The capture session for one output and the frame currently in flight.
     */
    class ext_session_t {
    public:
      ext_session_t() = default;

      ~ext_session_t() {
        destroy();
      }

      ext_session_t(const ext_session_t &) = delete;
      ext_session_t &operator=(const ext_session_t &) = delete;

      /**
       * @brief Opens a session and waits for its first buffer constraints.
       * @return false if the compositor stopped the session or sent no constraints within the timeout.
       */
      bool open(display_t &display, interface_t &interface, wl_output *output, bool paint_cursors, std::chrono::milliseconds timeout = 1s) {
        destroy();

        this->paint_cursors = paint_cursors;
        source = ext_output_image_capture_source_manager_v1_create_source(interface.ext_output_source_manager, output);
        session = ext_image_copy_capture_manager_v1_create_session(
          interface.ext_copy_manager,
          source,
          paint_cursors ? EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS : 0
        );
        ext_image_copy_capture_session_v1_add_listener(session, &session_listener, this);

        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!have_constraints && !stopped) {
          auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
          if (remaining.count() <= 0 || !display.dispatch(remaining)) {
            BOOST_LOG(error) << "The compositor sent no ext-image-copy-capture buffer constraints"sv;
            return false;
          }
        }

        if (stopped) {
          BOOST_LOG(error) << "The compositor stopped the ext-image-copy-capture session right away"sv;
          return false;
        }

        return true;
      }

      void destroy() {
        destroy_frame();
        if (session) {
          ext_image_copy_capture_session_v1_destroy(session);
          session = nullptr;
        }
        if (source) {
          ext_image_capture_source_v1_destroy(source);
          source = nullptr;
        }
        pending = {};
        constraints = {};
        have_constraints = false;
        constraints_changed = false;
        stopped = false;
      }

      /**
       * @brief Starts capturing into a buffer.
       * @param damage Area of the buffer that is stale, or std::nullopt for all of it.
       */
      void capture(wl_buffer *buffer, std::optional<rect_t> damage) {
        frame = ext_image_copy_capture_session_v1_create_frame(session);
        ext_image_copy_capture_frame_v1_add_listener(frame, &frame_listener, this);
        ext_image_copy_capture_frame_v1_attach_buffer(frame, buffer);

        if (!damage) {
          ext_image_copy_capture_frame_v1_damage_buffer(frame, 0, 0, (std::int32_t) constraints.width, (std::int32_t) constraints.height);
        } else if (!damage->empty()) {
          ext_image_copy_capture_frame_v1_damage_buffer(frame, damage->x, damage->y, damage->width, damage->height);
        }

        frame_state = frame_state_e::capturing;
        frame_damage = {};
        frame_time.reset();
        ext_image_copy_capture_frame_v1_capture(frame);
      }

      void destroy_frame() {
        if (frame) {
          ext_image_copy_capture_frame_v1_destroy(frame);
          frame = nullptr;
        }
        frame_state = frame_state_e::none;
      }

      enum class frame_state_e {
        none,
        capturing,
        ready,
        failed,
      };

      bool paint_cursors {false};

      buffer_constraints_t constraints;
      bool have_constraints {false};
      bool constraints_changed {false};
      bool stopped {false};

      ext_image_copy_capture_frame_v1 *frame {nullptr};
      frame_state_e frame_state {frame_state_e::none};
      std::uint32_t failure_reason {0};
      rect_t frame_damage;
      std::optional<std::chrono::steady_clock::time_point> frame_time;
      std::uint32_t frame_transform {0};

    private:
      // Session events
      static void on_buffer_size(void *data, ext_image_copy_capture_session_v1 *, std::uint32_t width, std::uint32_t height) {
        auto self = static_cast<ext_session_t *>(data);
        self->pending.width = width;
        self->pending.height = height;
      }

      static void on_shm_format(void *data, ext_image_copy_capture_session_v1 *, std::uint32_t format) {
        static_cast<ext_session_t *>(data)->pending.shm_formats.push_back(format);
      }

      static void on_dmabuf_device(void *data, ext_image_copy_capture_session_v1 *, wl_array *device) {
        auto self = static_cast<ext_session_t *>(data);
        if (device->size == sizeof(dev_t)) {
          dev_t dev;
          std::memcpy(&dev, device->data, sizeof(dev));
          self->pending.dmabuf_device = (std::uint64_t) dev;
        }
      }

      static void on_dmabuf_format(void *data, ext_image_copy_capture_session_v1 *, std::uint32_t format, wl_array *modifiers) {
        dmabuf_format_t entry {format, {}};
        const auto count = modifiers->size / sizeof(std::uint64_t);
        entry.modifiers.resize(count);
        std::memcpy(entry.modifiers.data(), modifiers->data, count * sizeof(std::uint64_t));
        static_cast<ext_session_t *>(data)->pending.dmabuf_formats.push_back(std::move(entry));
      }

      static void on_done(void *data, ext_image_copy_capture_session_v1 *) {
        auto self = static_cast<ext_session_t *>(data);
        if (self->have_constraints && self->pending != self->constraints) {
          self->constraints_changed = true;
        }
        self->constraints = std::move(self->pending);
        self->pending = {};
        self->have_constraints = true;
      }

      static void on_stopped(void *data, ext_image_copy_capture_session_v1 *) {
        static_cast<ext_session_t *>(data)->stopped = true;
      }

      // Frame events
      static void on_transform(void *data, ext_image_copy_capture_frame_v1 *, std::uint32_t transform) {
        static_cast<ext_session_t *>(data)->frame_transform = transform;
      }

      static void on_damage(void *data, ext_image_copy_capture_frame_v1 *, std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) {
        auto self = static_cast<ext_session_t *>(data);
        auto damage = clip(rect_t {x, y, width, height}, (int) self->constraints.width, (int) self->constraints.height);
        self->frame_damage = bounding_union(self->frame_damage, damage);
      }

      static void on_presentation_time(void *data, ext_image_copy_capture_frame_v1 *, std::uint32_t tv_sec_hi, std::uint32_t tv_sec_lo, std::uint32_t tv_nsec) {
        static_cast<ext_session_t *>(data)->frame_time = presentation_time_to_steady(tv_sec_hi, tv_sec_lo, tv_nsec, std::chrono::steady_clock::now());
      }

      static void on_ready(void *data, ext_image_copy_capture_frame_v1 *) {
        static_cast<ext_session_t *>(data)->frame_state = frame_state_e::ready;
      }

      static void on_failed(void *data, ext_image_copy_capture_frame_v1 *, std::uint32_t reason) {
        auto self = static_cast<ext_session_t *>(data);
        self->frame_state = frame_state_e::failed;
        self->failure_reason = reason;
      }

      static constexpr ext_image_copy_capture_session_v1_listener session_listener {
        .buffer_size = on_buffer_size,
        .shm_format = on_shm_format,
        .dmabuf_device = on_dmabuf_device,
        .dmabuf_format = on_dmabuf_format,
        .done = on_done,
        .stopped = on_stopped,
      };

      static constexpr ext_image_copy_capture_frame_v1_listener frame_listener {
        .transform = on_transform,
        .damage = on_damage,
        .presentation_time = on_presentation_time,
        .ready = on_ready,
        .failed = on_failed,
      };

      ext_image_capture_source_v1 *source {nullptr};
      ext_image_copy_capture_session_v1 *session {nullptr};
      buffer_constraints_t pending;
    };

    /// While the screen does not change, the newest frame is handed to the encoder again at this interval,
    /// so that an encoder that just joined gets a picture and display switch requests are noticed.
    constexpr auto idle_repeat_interval = 100ms;

    /// Consecutive failed frames (with reason "unknown") before the capture is reinitialized.
    constexpr int max_consecutive_failures = 10;
  }  // namespace

  class ext_t: public platf::display_t, public capture_stats_source_t {
  public:
    ext_t(std::size_t max_buffers, bool vram):
        pool {max_buffers},
        vram {vram} {
    }

    int init(platf::mem_type_e hwdevice_type, const std::string &display_name, const ::video::config_t &config, const ext_options_t &options) {
      delay = std::chrono::nanoseconds {1s} / config.framerate;
      mem_type = hwdevice_type;
      this->options = options;

      if (display.init()) {
        return -1;
      }

      if (!enumerate_outputs(display, interface)) {
        return -1;
      }

      if (!interface[interface_t::EXT_IMAGE_COPY_CAPTURE] || !interface[interface_t::EXT_OUTPUT_CAPTURE_SOURCE]) {
        BOOST_LOG(error) << "Missing Wayland wire for ext-image-copy-capture"sv;
        return -1;
      }

      auto index = select_output(interface, display_name);
      if (!index) {
        return -1;
      }
      output = interface.monitors[*index]->output;

      // The cursor is painted by default; capture() reopens the session if the stream wants it hidden.
      if (!session.open(display, interface, output, true)) {
        return -1;
      }

      if (!configure_pool()) {
        return -1;
      }

      // The buffer size is in hardware pixels, in the orientation the compositor delivers frames.
      apply_output_geometry(*this, interface, *index, extent_t {(int) pool.spec.width, (int) pool.spec.height});

      BOOST_LOG(info) << "Capturing with ext-image-copy-capture into "sv << (pool.spec.shm ? "shared memory"sv : "dma-bufs"sv)
                      << " (format "sv << util::hex(pool.spec.format).to_string_view() << ')';

      return 0;
    }

    capture_stats_t capture_stats() const override {
      return stats;
    }

    platf::capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
      frame_pacer_t pacer {delay};
      auto idle_until = std::chrono::steady_clock::now() + idle_repeat_interval;
      int failures = 0;

      while (true) {
        if (*cursor != session.paint_cursors) {
          // The option is fixed per session: open a new one. Buffers stay valid if the constraints did not change.
          auto old_constraints = session.constraints;
          abandon_frame();
          if (!session.open(display, interface, output, *cursor)) {
            return platf::capture_e::reinit;
          }
          if (session.constraints != old_constraints) {
            BOOST_LOG(info) << "ext-image-copy-capture buffer constraints changed; reinitializing capture"sv;
            return platf::capture_e::reinit;
          }
        }

        if (session.stopped) {
          BOOST_LOG(info) << "ext-image-copy-capture session stopped (output removed?); reinitializing capture"sv;
          return platf::capture_e::reinit;
        }
        if (session.constraints_changed) {
          BOOST_LOG(info) << "ext-image-copy-capture buffer constraints changed; reinitializing capture"sv;
          return platf::capture_e::reinit;
        }

        // Keep a capture in flight.
        if (!session.frame) {
          std::size_t slot;
          auto result = pool.acquire(display, interface.dmabuf_interface, interface.shm, gbm.get(), slot);
          stats.buffers_allocated = pool.allocations;
          if (result == buffer_pool_t::acquire_e::failed) {
            return platf::capture_e::reinit;
          }
          if (result == buffer_pool_t::acquire_e::ok) {
            frame_slot = slot;
            session.capture(pool[slot].buffer, pool.slots.damage_for(slot));
          }
        }

        auto now = std::chrono::steady_clock::now();

        // A frame held back by the pacer is due.
        if (auto ready = pool.slots.ready_slot(); ready && now >= pacer.deadline()) {
          if (auto status = deliver(*ready, push_captured_image_cb, pull_free_image_cb); status != platf::capture_e::ok) {
            return status == platf::capture_e::timeout ? platf::capture_e::ok : status;
          }
          pacer.delivered(now);
          idle_until = now + idle_repeat_interval;
          continue;
        }

        if (now >= idle_until) {
          if (auto status = repeat(push_captured_image_cb, pull_free_image_cb); status != platf::capture_e::ok) {
            return status == platf::capture_e::timeout ? platf::capture_e::ok : status;
          }
          idle_until = now + idle_repeat_interval;
          continue;
        }

        auto wake = pool.slots.ready_slot() ? std::min(pacer.deadline(), idle_until) : idle_until;
        if (!session.frame) {
          // No buffer was free: retry soon.
          wake = std::min(wake, now + 1ms);
        }
        auto timeout = std::chrono::ceil<std::chrono::milliseconds>(wake - now);
        display.dispatch(std::max(timeout, 0ms));

        if (session.frame_state == ext_session_t::frame_state_e::failed) {
          auto reason = session.failure_reason;
          abandon_frame();
          if (reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED) {
            BOOST_LOG(info) << "ext-image-copy-capture session stopped (output removed?); reinitializing capture"sv;
            return platf::capture_e::reinit;
          }
          if (reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS) {
            BOOST_LOG(info) << "Capture buffer no longer matches the session constraints; reinitializing capture"sv;
            return platf::capture_e::reinit;
          }
          if (++failures > max_consecutive_failures) {
            BOOST_LOG(error) << "ext-image-copy-capture keeps failing; reinitializing capture"sv;
            return platf::capture_e::reinit;
          }
          continue;
        }

        if (session.frame_state == ext_session_t::frame_state_e::ready) {
          failures = 0;
          ++stats.frames_captured;
          if (session.frame_transform != transform_logged) {
            BOOST_LOG(debug) << "ext-image-copy-capture frame transform: "sv << session.frame_transform;
            transform_logged = session.frame_transform;
          }

          auto damage = session.frame_damage;
          if (timestamps.size() < pool.slots.size()) {
            timestamps.resize(pool.slots.size());
          }
          timestamps[frame_slot] = session.frame_time;
          session.destroy_frame();
          pool.slots.completed(frame_slot, damage);

          now = std::chrono::steady_clock::now();
          if (pacer.may_deliver(now)) {
            if (auto status = deliver(frame_slot, push_captured_image_cb, pull_free_image_cb); status != platf::capture_e::ok) {
              return status == platf::capture_e::timeout ? platf::capture_e::ok : status;
            }
            pacer.delivered(now);
            idle_until = now + idle_repeat_interval;
          }
        }
      }
    }

    int dummy_img(platf::img_t *img) override {
      return 0;
    }

  protected:
    bool configure_pool() {
      const auto &constraints = session.constraints;

      const bool allow_dmabuf = interface.dmabuf_interface && !constraints.dmabuf_formats.empty() && gbm.open(constraints.dmabuf_device);
      auto spec = choose_buffer_spec(constraints, allow_dmabuf, !vram, options.prefer_shm);
      if (!spec) {
        BOOST_LOG(error) << "No usable ext-image-copy-capture buffer type ("sv << (vram ? "dma-buf"sv : "dma-buf or shm"sv) << " with an 8-bit RGB format)"sv;
        return false;
      }

      pool.configure(*spec);
      return true;
    }

    void abandon_frame() {
      if (session.frame) {
        session.destroy_frame();
        pool.slots.abandoned(frame_slot);
      }
    }

    /**
     * @brief Hands the frame in `slot` to the encoder.
     * @return ok, interrupted, reinit, or timeout if the encoder asked to stop.
     */
    platf::capture_e deliver(std::size_t slot, const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb) {
      std::shared_ptr<platf::img_t> img_out;
      if (!pull_free_image_cb(img_out)) {
        return platf::capture_e::interrupted;
      }

      if (vram) {
        if (!hand_over_buffer(pool, slot, img_out, ++sequence, false)) {
          return platf::capture_e::reinit;
        }
        last_slot = slot;
      } else {
        if (!read_buffer(egl_display.get(), pool[slot], *img_out, false)) {
          return platf::capture_e::reinit;
        }
        pool.slots.consumed(slot);
      }

      auto timestamp = slot < timestamps.size() ? timestamps[slot] : std::nullopt;
      img_out->frame_timestamp = next_timestamp(timestamp.value_or(std::chrono::steady_clock::now()));

      // Kept to repeat it while the screen does not change (this also keeps its dma-buf from being reused).
      last_img = img_out;

      ++stats.frames_delivered;
      if (!push_captured_image_cb(std::move(img_out), true)) {
        return platf::capture_e::timeout;
      }
      return platf::capture_e::ok;
    }

    /**
     * @brief Hands the newest frame to the encoder again with a current timestamp.
     */
    platf::capture_e repeat(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb) {
      if (!last_img) {
        if (!push_captured_image_cb(nullptr, false)) {
          return platf::capture_e::timeout;
        }
        return platf::capture_e::ok;
      }

      std::shared_ptr<platf::img_t> img_out;
      if (!pull_free_image_cb(img_out)) {
        return platf::capture_e::interrupted;
      }

      if (vram) {
        if (!hand_over_buffer(pool, last_slot, img_out, ++sequence, false)) {
          return platf::capture_e::reinit;
        }
      } else {
        copy_rows(last_img->data, last_img->row_pitch, img_out->data, img_out->row_pitch, (std::size_t) width * 4, (std::size_t) height, false);
      }
      img_out->frame_timestamp = next_timestamp(std::chrono::steady_clock::now());
      last_img = img_out;

      if (!push_captured_image_cb(std::move(img_out), true)) {
        return platf::capture_e::timeout;
      }
      return platf::capture_e::ok;
    }

    /**
     * @brief Keeps frame timestamps strictly increasing. A repeat is stamped with the current time, and a frame
     *        captured right after it may have been presented slightly earlier.
     */
    std::chrono::steady_clock::time_point next_timestamp(std::chrono::steady_clock::time_point timestamp) {
      if (last_timestamp && timestamp <= *last_timestamp) {
        timestamp = *last_timestamp + 1us;
      }
      last_timestamp = timestamp;
      return timestamp;
    }

    platf::mem_type_e mem_type;
    std::chrono::nanoseconds delay;
    ext_options_t options;

    wl::display_t display;
    interface_t interface;
    gbm_device_t gbm;
    buffer_pool_t pool;
    ext_session_t session;
    wl_output *output {nullptr};

    bool vram;
    std::size_t frame_slot {0};
    std::size_t last_slot {0};
    std::vector<std::optional<std::chrono::steady_clock::time_point>> timestamps;
    std::shared_ptr<platf::img_t> last_img;
    std::optional<std::chrono::steady_clock::time_point> last_timestamp;
    std::uint64_t sequence {0};
    std::uint32_t transform_logged {0};
    capture_stats_t stats;

    // System memory path only.
    egl::display_t egl_display;
    egl::ctx_t ctx;
  };

  struct ext_ram_img_t: public platf::img_t {
    ~ext_ram_img_t() override {
      delete[] data;
      data = nullptr;
    }
  };

  class ext_ram_t: public ext_t {
  public:
    // One buffer being captured into and one holding the newest frame.
    ext_ram_t():
        ext_t {2, false} {
    }

    int init(platf::mem_type_e hwdevice_type, const std::string &display_name, const ::video::config_t &config, const ext_options_t &options) {
      if (ext_t::init(hwdevice_type, display_name, config, options)) {
        return -1;
      }

      if (!pool.spec.shm) {
        egl_display = egl::make_display(display.get());
        if (!egl_display) {
          return -1;
        }

        auto ctx_opt = egl::make_ctx(egl_display.get());
        if (!ctx_opt) {
          return -1;
        }

        ctx = std::move(*ctx_opt);
      }

      return 0;
    }

    std::shared_ptr<platf::img_t> alloc_img() override {
      auto img = std::make_shared<ext_ram_img_t>();
      img->width = width;
      img->height = height;
      img->pixel_pitch = 4;
      img->row_pitch = img->pixel_pitch * width;
      img->data = new std::uint8_t[height * img->row_pitch];

      return img;
    }

    std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(platf::pix_fmt_e pix_fmt) override {
#ifdef SUNSHINE_BUILD_VAAPI
      if (mem_type == platf::mem_type_e::vaapi) {
        return va::make_avcodec_encode_device(width, height, false);
      }
#endif

#ifdef SUNSHINE_BUILD_CUDA
      if (mem_type == platf::mem_type_e::cuda) {
        return cuda::make_avcodec_encode_device(width, height, false);
      }
#endif

      return std::make_unique<platf::avcodec_encode_device_t>();
    }
  };

  class ext_vram_t: public ext_t {
  public:
    // One buffer being captured into, one holding the newest frame, and two the encoder may still read
    // (one of them kept to repeat it while the screen is static).
    ext_vram_t():
        ext_t {4, true} {
    }

    std::shared_ptr<platf::img_t> alloc_img() override {
      auto img = std::make_shared<egl::img_descriptor_t>();

      img->width = width;
      img->height = height;
      img->sequence = 0;
      img->serial = std::numeric_limits<decltype(img->serial)>::max();
      img->data = nullptr;

      // File descriptors aren't open
      std::fill_n(img->sd.fds, 4, -1);

      return img;
    }

    std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(platf::pix_fmt_e pix_fmt) override {
#ifdef SUNSHINE_BUILD_VAAPI
      if (mem_type == platf::mem_type_e::vaapi) {
        return va::make_avcodec_encode_device(width, height, 0, 0, true);
      }
#endif

#ifdef SUNSHINE_BUILD_CUDA
      if (mem_type == platf::mem_type_e::cuda) {
        return cuda::make_avcodec_gl_encode_device(width, height, 0, 0);
      }
#endif

      return std::make_unique<platf::avcodec_encode_device_t>();
    }
  };

  std::shared_ptr<platf::display_t> make_ext_display(platf::mem_type_e hwdevice_type, const std::string &display_name, const ::video::config_t &config, const ext_options_t &options) {
    if (hwdevice_type != platf::mem_type_e::system && hwdevice_type != platf::mem_type_e::vaapi && hwdevice_type != platf::mem_type_e::cuda) {
      BOOST_LOG(error) << "Could not initialize display with the given hw device type."sv;
      return nullptr;
    }

    if (hwdevice_type == platf::mem_type_e::vaapi || hwdevice_type == platf::mem_type_e::cuda) {
      auto disp = std::make_shared<ext_vram_t>();
      if (disp->init(hwdevice_type, display_name, config, options)) {
        return nullptr;
      }
      return disp;
    }

    auto disp = std::make_shared<ext_ram_t>();
    if (disp->init(hwdevice_type, display_name, config, options)) {
      return nullptr;
    }
    return disp;
  }
}  // namespace wl

namespace platf {
  std::shared_ptr<display_t> ext_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    return wl::make_ext_display(hwdevice_type, display_name, config, {});
  }

  std::vector<std::string> ext_display_names() {
    wl::display_t display;
    if (display.init()) {
      return {};
    }

    wl::interface_t interface;
    if (!wl::enumerate_outputs(display, interface)) {
      return {};
    }

    if (!interface[wl::interface_t::EXT_IMAGE_COPY_CAPTURE] || !interface[wl::interface_t::EXT_OUTPUT_CAPTURE_SOURCE]) {
      BOOST_LOG(info) << "The compositor does not support ext-image-copy-capture"sv;
      return {};
    }

    auto display_names = wl::output_names(interface);
    wl::log_outputs(interface, display_names);

    return display_names;
  }
}  // namespace platf
