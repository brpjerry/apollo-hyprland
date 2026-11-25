/**
 * @file src/platform/linux/wlgrab.cpp
 * @brief Definitions for wlgrab capture.
 */
// standard includes
#include <thread>

// local includes
#include "cuda.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/platform/display_selection.h"
#include "src/video.h"
#include "vaapi.h"
#include "wayland.h"

using namespace std::literals;

namespace wl {
  /**
   * @brief Names used to select outputs: the connector name, or the enumeration index if the compositor
   *        did not report a name.
   */
  static std::vector<std::string> output_names(const interface_t &interface) {
    std::vector<std::string> names;
    names.reserve(interface.monitors.size());
    for (std::size_t x = 0; x < interface.monitors.size(); ++x) {
      const auto &name = interface.monitors[x]->name;
      names.emplace_back(name.empty() ? std::to_string(x) : name);
    }
    return names;
  }

  static std::vector<output_geometry_t> output_geometries(const interface_t &interface) {
    std::vector<output_geometry_t> geometries;
    geometries.reserve(interface.monitors.size());
    for (const auto &monitor : interface.monitors) {
      geometries.emplace_back(monitor->geometry);
    }
    return geometries;
  }

  struct img_t: public platf::img_t {
    ~img_t() override {
      delete[] data;
      data = nullptr;
    }
  };

  class wlr_t: public platf::display_t {
  public:
    int init(platf::mem_type_e hwdevice_type, const std::string &display_name, const ::video::config_t &config) {
      delay = std::chrono::nanoseconds {1s} / config.framerate;
      mem_type = hwdevice_type;

      if (display.init()) {
        return -1;
      }

      interface.listen(display.registry());

      display.roundtrip();

      if (!interface[wl::interface_t::XDG_OUTPUT]) {
        BOOST_LOG(error) << "Missing Wayland wire for xdg_output"sv;
        return -1;
      }

      if (!interface[wl::interface_t::WLR_EXPORT_DMABUF]) {
        BOOST_LOG(error) << "Missing Wayland wire for wlr-export-dmabuf"sv;
        return -1;
      }

      if (interface.monitors.empty()) {
        BOOST_LOG(error) << "The Wayland compositor reported no outputs"sv;
        return -1;
      }

      // Names and the layout bounding box need every output, not just the one we stream.
      for (auto &monitor : interface.monitors) {
        monitor->listen(interface.output_manager);
      }

      display.roundtrip();

      const auto names = output_names(interface);

      std::size_t index = 0;
      if (!display_name.empty()) {
        auto found = platf::find_display(names, display_name, true);
        if (!found) {
          BOOST_LOG(error) << "Wayland output ["sv << display_name << "] not found; available outputs: "sv << platf::join_display_names(names);
          return -1;
        }
        index = *found;

        if (names[index] != display_name) {
          BOOST_LOG(warning) << "Selecting Wayland output by index ["sv << display_name << "] is deprecated; use its name ["sv << names[index] << "] instead"sv;
        }
      }

      auto monitor = interface.monitors[index].get();
      const auto &geometry = monitor->geometry;

      output = monitor->output;

      // Capture buffers are in hardware pixels, in whatever orientation the compositor's screencopy delivers.
      const auto upright = transformed_mode_size(geometry);
      const auto frame = probe_frame_size(display, interface.screencopy_manager, output).value_or(upright);
      width = frame.width;
      height = frame.height;

      // Everything handed to input is in the compositor's logical (scaled) layout space.
      // See input::touch_port_t for how these are used.
      const auto logical = logical_size(geometry);
      offset_x = geometry.logical_x;
      offset_y = geometry.logical_y;
      logical_width = logical.width;
      logical_height = logical.height;

      const auto bounds = layout_bounds(output_geometries(interface));
      env_x = bounds.min_x;
      env_y = bounds.min_y;
      env_width = bounds.width;
      env_height = bounds.height;

      BOOST_LOG(info) << "Selected Wayland output ["sv << names[index] << "] ("sv << monitor->description << ") for streaming"sv;
      BOOST_LOG(debug) << "Mode: "sv << geometry.mode_width << 'x' << geometry.mode_height << ", frame "sv << width << 'x' << height << ", transform "sv << geometry.transform << ", scale "sv << effective_scale(geometry);
      BOOST_LOG(debug) << "Logical geometry: "sv << logical_width << 'x' << logical_height << " at "sv << offset_x << ',' << offset_y;
      BOOST_LOG(debug) << "Desktop layout: "sv << env_width << 'x' << env_height << " at "sv << env_x << ',' << env_y;

      if (frame != upright) {
        BOOST_LOG(warning) << "Wayland output ["sv << names[index] << "] has transform "sv << geometry.transform
                           << " but the compositor delivers "sv << frame.width << 'x' << frame.height
                           << " frames; the stream will not be rotated to match"sv;
      }

      return 0;
    }

    int dummy_img(platf::img_t *img) override {
      return 0;
    }

    inline platf::capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor) {
      auto to = std::chrono::steady_clock::now() + timeout;

      // Dispatch events until we get a new frame or the timeout expires
      dmabuf.listen(interface.screencopy_manager, interface.dmabuf_interface, output, cursor);
      do {
        auto remaining_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(to - std::chrono::steady_clock::now());
        if (remaining_time_ms.count() < 0 || !display.dispatch(remaining_time_ms)) {
          return platf::capture_e::timeout;
        }
      } while (dmabuf.status == dmabuf_t::WAITING);

      auto current_frame = dmabuf.current_frame;

      if (
        dmabuf.status == dmabuf_t::REINIT ||
        current_frame->sd.width != width ||
        current_frame->sd.height != height
      ) {
        return platf::capture_e::reinit;
      }

      return platf::capture_e::ok;
    }

    platf::mem_type_e mem_type;

    std::chrono::nanoseconds delay;

    wl::display_t display;
    interface_t interface;
    dmabuf_t dmabuf;

    wl_output *output;
  };

  class wlr_ram_t: public wlr_t {
  public:
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

    platf::capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor) {
      auto status = wlr_t::snapshot(pull_free_image_cb, img_out, timeout, cursor);
      if (status != platf::capture_e::ok) {
        return status;
      }
      auto frame_timestamp = std::chrono::steady_clock::now();

      auto current_frame = dmabuf.current_frame;

      auto rgb_opt = egl::import_source(egl_display.get(), current_frame->sd);

      if (!rgb_opt) {
        return platf::capture_e::reinit;
      }

      if (!pull_free_image_cb(img_out)) {
        return platf::capture_e::interrupted;
      }

      gl::ctx.BindTexture(GL_TEXTURE_2D, (*rgb_opt)->tex[0]);

      // Don't remove these lines, see https://github.com/LizardByte/Sunshine/issues/453
      int w, h;
      gl::ctx.GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
      gl::ctx.GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
      BOOST_LOG(debug) << "width and height: w "sv << w << " h "sv << h;

      gl::ctx.GetTextureSubImage((*rgb_opt)->tex[0], 0, 0, 0, 0, width, height, 1, GL_BGRA, GL_UNSIGNED_BYTE, img_out->height * img_out->row_pitch, img_out->data);
      gl::ctx.BindTexture(GL_TEXTURE_2D, 0);
      img_out->frame_timestamp = frame_timestamp;

      return platf::capture_e::ok;
    }

    int init(platf::mem_type_e hwdevice_type, const std::string &display_name, const ::video::config_t &config) {
      if (wlr_t::init(hwdevice_type, display_name, config)) {
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

    platf::capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor) {
      auto status = wlr_t::snapshot(pull_free_image_cb, img_out, timeout, cursor);
      if (status != platf::capture_e::ok) {
        return status;
      }
      auto frame_timestamp = std::chrono::steady_clock::now();

      if (!pull_free_image_cb(img_out)) {
        return platf::capture_e::interrupted;
      }
      auto img = (egl::img_descriptor_t *) img_out.get();
      img->reset();

      auto current_frame = dmabuf.current_frame;

      ++sequence;
      img->sequence = sequence;

      img->sd = current_frame->sd;
      img->frame_timestamp = frame_timestamp;

      // Prevent dmabuf from closing the file descriptors.
      std::fill_n(current_frame->sd.fds, 4, -1);

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

  std::vector<std::string> wl_display_names() {
    std::vector<std::string> display_names;

    wl::display_t display;
    if (display.init()) {
      return {};
    }

    wl::interface_t interface;
    interface.listen(display.registry());

    display.roundtrip();

    if (!interface[wl::interface_t::XDG_OUTPUT]) {
      BOOST_LOG(warning) << "Missing Wayland wire for xdg_output"sv;
      return {};
    }

    if (!interface[wl::interface_t::WLR_EXPORT_DMABUF]) {
      BOOST_LOG(warning) << "Missing Wayland wire for wlr-export-dmabuf"sv;
      return {};
    }

    for (auto &monitor : interface.monitors) {
      monitor->listen(interface.output_manager);
    }

    display.roundtrip();

    display_names = wl::output_names(interface);

    BOOST_LOG(info) << "-------- Start of Wayland monitor list --------"sv;

    for (std::size_t x = 0; x < interface.monitors.size(); ++x) {
      auto monitor = interface.monitors[x].get();
      const auto &geometry = monitor->geometry;
      const auto logical = wl::logical_size(geometry);

      BOOST_LOG(info) << "Monitor "sv << x << " is "sv << display_names[x] << ": "sv << monitor->description
                      << " ("sv << geometry.mode_width << 'x' << geometry.mode_height
                      << ", logical "sv << logical.width << 'x' << logical.height << " at "sv << geometry.logical_x << ',' << geometry.logical_y
                      << ", scale "sv << wl::effective_scale(geometry) << ", transform "sv << geometry.transform << ')';
    }

    BOOST_LOG(info) << "--------- End of Wayland monitor list ---------"sv;

    return display_names;
  }

}  // namespace platf
