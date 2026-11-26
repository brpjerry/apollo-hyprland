/**
 * @file src/platform/linux/wlgrab.cpp
 * @brief Definitions for wlr-screencopy capture.
 */
// standard includes
#include <cstring>
#include <thread>

// local includes
#include "cuda.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/platform/vdisplay.h"
#include "src/video.h"
#include "vaapi.h"
#include "wayland.h"

using namespace std::literals;

namespace wl {
  struct img_t: public platf::img_t {
    ~img_t() override {
      delete[] data;
      data = nullptr;
    }
  };

  class wlr_t: public platf::display_t, public capture_stats_source_t {
  public:
    explicit wlr_t(std::size_t max_buffers):
        screencopy {max_buffers} {
    }

    int init(platf::mem_type_e hwdevice_type, const std::string &display_name, const ::video::config_t &config, bool allow_shm) {
      delay = std::chrono::nanoseconds {1s} / config.framerate;
      mem_type = hwdevice_type;

      if (display.init()) {
        return -1;
      }

      if (!enumerate_outputs(display, interface)) {
        return -1;
      }

      if (!interface[wl::interface_t::WLR_EXPORT_DMABUF]) {
        BOOST_LOG(error) << "Missing Wayland wire for wlr-screencopy"sv;
        return -1;
      }

      auto index = select_output(interface, display_name);
      if (!index) {
        return -1;
      }

      output = interface.monitors[*index]->output;

      // HDR state and metadata of the output. Without wp-color-management the output is treated as SDR.
      color.open(output_names(interface)[*index]);

      // Capture buffers are in hardware pixels, in whatever orientation the compositor's screencopy delivers.
      auto constraints = probe_screencopy(display, interface.screencopy_manager, output);
      if (!constraints) {
        BOOST_LOG(error) << "The compositor offered no screencopy buffer for this output"sv;
        return -1;
      }

      const bool allow_dmabuf = interface.dmabuf_interface && gbm.open();
      auto spec = screencopy_buffer_spec(*constraints, allow_dmabuf, allow_shm);
      if (!spec) {
        BOOST_LOG(error) << "No usable screencopy buffer type ("sv << (allow_shm ? "dma-buf or shm"sv : "dma-buf"sv) << " with an 8-bit RGB format)"sv;
        return -1;
      }
      screencopy.pool.configure(*spec);

      apply_output_geometry(*this, interface, *index, extent_t {(int) spec->width, (int) spec->height});

      return 0;
    }

    capture_stats_t capture_stats() const override {
      return stats;
    }

    bool is_hdr() override {
      return hdr_frames && color.is_hdr();
    }

    bool get_hdr_metadata(SS_HDR_METADATA &metadata) override {
      if (!is_hdr()) {
        std::memset(&metadata, 0, sizeof(metadata));
        return false;
      }
      return fill_hdr_metadata(*color.description(), metadata);
    }

    bool hdr_supported() override {
      if (!hdr_frames) {
        return false;
      }
      // Apollo's Hyprland virtual displays can be put in HDR mode (platf::vdisplay::set_hdr()).
      return color.is_hdr() || (color.compositor_supports_hdr10() && platf::vdisplay::status() == platf::vdisplay::status_e::ok);
    }

    int dummy_img(platf::img_t *img) override {
      return 0;
    }

    platf::capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
      auto next_frame = std::chrono::steady_clock::now();

      sleep_overshoot_logger.reset();

      while (true) {
        auto now = std::chrono::steady_clock::now();

        if (next_frame > now) {
          std::this_thread::sleep_for(next_frame - now);
          sleep_overshoot_logger.first_point(next_frame);
          sleep_overshoot_logger.second_point_now_and_log();
        }

        next_frame += delay;
        if (next_frame < now) {  // some major slowdown happened; we couldn't keep up
          next_frame = now + delay;
        }

        // The output switched between SDR and HDR (or its HDR metadata changed): the encoder has to be recreated
        // with the new colorspace.
        if (color.poll_changed()) {
          BOOST_LOG(info) << "Output colors changed; reinitializing capture"sv;
          return platf::capture_e::reinit;
        }

        std::shared_ptr<platf::img_t> img_out;
        auto status = snapshot(pull_free_image_cb, img_out, 1000ms, *cursor);
        switch (status) {
          case platf::capture_e::reinit:
          case platf::capture_e::error:
          case platf::capture_e::interrupted:
            return status;
          case platf::capture_e::timeout:
            if (!push_captured_image_cb(std::move(img_out), false)) {
              return platf::capture_e::ok;
            }
            break;
          case platf::capture_e::ok:
            ++stats.frames_delivered;
            if (!push_captured_image_cb(std::move(img_out), true)) {
              return platf::capture_e::ok;
            }
            break;
          default:
            BOOST_LOG(error) << "Unrecognized capture status ["sv << (int) status << ']';
            return status;
        }
      }

      return platf::capture_e::ok;
    }

  protected:
    /**
     * @brief Waits until a frame has been copied into the pool's ready slot.
     * @details Only one frame is ever in flight. If the timeout expires, the frame stays pending and the next
     *          call keeps waiting for it.
     */
    platf::capture_e wait_for_frame(std::chrono::milliseconds timeout, bool cursor) {
      auto deadline = std::chrono::steady_clock::now() + timeout;

      while (true) {
        if (screencopy.status != screencopy_t::WAITING) {
          auto result = screencopy.start(display, interface.screencopy_manager, interface.dmabuf_interface, interface.shm, gbm.get(), output, cursor);
          stats.buffers_allocated = screencopy.pool.allocations;
          if (result == buffer_pool_t::acquire_e::failed) {
            return platf::capture_e::reinit;
          }
          if (result == buffer_pool_t::acquire_e::busy) {
            // Every buffer is still being encoded.
            if (std::chrono::steady_clock::now() >= deadline) {
              return platf::capture_e::timeout;
            }
            std::this_thread::sleep_for(1ms);
            continue;
          }
        }

        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining.count() < 0 || !display.dispatch(remaining)) {
          return platf::capture_e::timeout;
        }

        if (screencopy.status == screencopy_t::REINIT) {
          return platf::capture_e::reinit;
        }
        if (screencopy.status == screencopy_t::READY) {
          ++stats.frames_captured;
          screencopy.status = screencopy_t::IDLE;
          return platf::capture_e::ok;
        }
      }
    }

    virtual platf::capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor) = 0;

    platf::mem_type_e mem_type;

    std::chrono::nanoseconds delay;

    wl::display_t display;
    interface_t interface;
    gbm_device_t gbm;
    screencopy_t screencopy;
    capture_stats_t stats;

    /// The output's color description (HDR state and metadata).
    output_color_t color;
    /// The encode path of this backend turns captured frames into PQ for HDR streams. Until it does, an HDR
    /// output is streamed as SDR.
    bool hdr_frames {false};

    wl_output *output;
  };

  class wlr_ram_t: public wlr_t {
  public:
    // Frames are copied out right away, so one buffer is enough.
    wlr_ram_t():
        wlr_t {1} {
    }

    platf::capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor) override {
      auto status = wait_for_frame(timeout, cursor);
      if (status != platf::capture_e::ok) {
        return status;
      }
      auto frame_timestamp = std::chrono::steady_clock::now();

      auto slot = *screencopy.pool.slots.ready_slot();
      auto &buffer = screencopy.pool[slot];
      if ((int) buffer.spec.width != width || (int) buffer.spec.height != height) {
        return platf::capture_e::reinit;
      }

      if (!pull_free_image_cb(img_out)) {
        return platf::capture_e::interrupted;
      }

      if (!read_buffer(egl_display.get(), buffer, *img_out, screencopy.y_invert)) {
        return platf::capture_e::reinit;
      }
      screencopy.pool.slots.consumed(slot);

      img_out->frame_timestamp = frame_timestamp;

      return platf::capture_e::ok;
    }

    int init(platf::mem_type_e hwdevice_type, const std::string &display_name, const ::video::config_t &config) {
      if (wlr_t::init(hwdevice_type, display_name, config, true)) {
        return -1;
      }

      egl_display = egl::make_display(display.get());
      if (!egl_display) {
        return -1;
      }

      auto ctx_opt = egl::make_ctx(egl_display.get());
      if (!ctx_opt) {
        return -1;
      }

      ctx = std::move(*ctx_opt);

      return 0;
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

    std::shared_ptr<platf::img_t> alloc_img() override {
      auto img = std::make_shared<img_t>();
      img->width = width;
      img->height = height;
      img->pixel_pitch = 4;
      img->row_pitch = img->pixel_pitch * width;
      img->data = new std::uint8_t[height * img->row_pitch];

      return img;
    }

    egl::display_t egl_display;
    egl::ctx_t ctx;
  };

  class wlr_vram_t: public wlr_t {
  public:
    // One buffer being captured into plus up to two the encoder may still read.
    wlr_vram_t():
        wlr_t {3} {
      // The GL converter turns the SDR frames of an HDR output into PQ (output_color_t::sdr_white_nits()).
      hdr_frames = true;
    }

    int init(platf::mem_type_e hwdevice_type, const std::string &display_name, const ::video::config_t &config) {
      return wlr_t::init(hwdevice_type, display_name, config, false);
    }

    platf::capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor) override {
      auto status = wait_for_frame(timeout, cursor);
      if (status != platf::capture_e::ok) {
        return status;
      }
      auto frame_timestamp = std::chrono::steady_clock::now();

      auto slot = *screencopy.pool.slots.ready_slot();
      const auto &spec = screencopy.pool[slot].spec;
      if ((int) spec.width != width || (int) spec.height != height) {
        return platf::capture_e::reinit;
      }

      if (!pull_free_image_cb(img_out)) {
        return platf::capture_e::interrupted;
      }

      if (!hand_over_buffer(screencopy.pool, slot, img_out, ++sequence, screencopy.y_invert)) {
        return platf::capture_e::reinit;
      }
      static_cast<egl::img_descriptor_t *>(img_out.get())->sdr_white_nits = color.sdr_white_nits();
      img_out->frame_timestamp = frame_timestamp;

      return platf::capture_e::ok;
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

    int dummy_img(platf::img_t *img) override {
      // Empty images are recognized as dummies by the zero sequence number
      return 0;
    }

    std::uint64_t sequence {};
  };

}  // namespace wl

namespace platf {
  std::shared_ptr<display_t> wl_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    if (hwdevice_type != platf::mem_type_e::system && hwdevice_type != platf::mem_type_e::vaapi && hwdevice_type != platf::mem_type_e::cuda) {
      BOOST_LOG(error) << "Could not initialize display with the given hw device type."sv;
      return nullptr;
    }

    if (hwdevice_type == platf::mem_type_e::vaapi || hwdevice_type == platf::mem_type_e::cuda) {
      auto wlr = std::make_shared<wl::wlr_vram_t>();
      if (wlr->init(hwdevice_type, display_name, config)) {
        return nullptr;
      }

      return wlr;
    }

    auto wlr = std::make_shared<wl::wlr_ram_t>();
    if (wlr->init(hwdevice_type, display_name, config)) {
      return nullptr;
    }

    return wlr;
  }

  bool wl_capture_supported() {
    wl::display_t display;
    if (display.init()) {
      return false;
    }
    wl::interface_t interface;
    interface.listen(display.registry());
    display.roundtrip();
    return interface[wl::interface_t::XDG_OUTPUT] && interface[wl::interface_t::WLR_EXPORT_DMABUF];
  }

  std::vector<std::string> wl_display_names() {
    wl::display_t display;
    if (display.init()) {
      return {};
    }

    wl::interface_t interface;
    if (!wl::enumerate_outputs(display, interface)) {
      return {};
    }

    if (!interface[wl::interface_t::WLR_EXPORT_DMABUF]) {
      BOOST_LOG(warning) << "Missing Wayland wire for wlr-screencopy"sv;
      return {};
    }

    auto display_names = wl::output_names(interface);
    wl::log_outputs(interface, display_names);

    return display_names;
  }

}  // namespace platf
