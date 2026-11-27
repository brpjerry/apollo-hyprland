/**
 * @file tests/unit/platform/test_wayland_hdr_live.cpp
 * @brief HDR streaming from a Hyprland HDR output: the video memory path converts captured frames to PQ.
 * @details Captures a headless output in HDR mode through ext-image-copy-capture into dma-bufs, runs the frame
 *          through the same GL converter the NVENC (CUDA) and VAAPI paths use, and checks the Y plane against a
 *          reference computation. Skipped without a compositor, without ext-image-copy-capture or wp-color-management,
 *          or with APOLLO_SKIP_LIVE_HYPRLAND set.
 */
#include "../../tests_common.h"

#if defined(__linux__) && defined(SUNSHINE_BUILD_WAYLAND)

  #include "pq_reference.h"
  #include "wayland_test_output.h"

  #include <cmath>
  #include <fcntl.h>
  #include <unistd.h>
  #include <xf86drm.h>

  #include <src/platform/common.h>
  #include <src/platform/linux/graphics.h>
  #include <src/platform/linux/hyprland/vdisplay.h>
  #include <src/platform/linux/wayland.h>
  #include <src/video.h>
  #include <src/video_colorspace.h>

extern "C" {
  #include <libavutil/pixfmt.h>
}

namespace platf {
  std::vector<std::string> ext_display_names();
}  // namespace platf

namespace {
  using namespace std::literals;
  using test_hdr::expected_y;

  struct WaylandHdrLiveTest: PlatformTestSuite {
    void SetUp() override {
      if (!std::getenv("WAYLAND_DISPLAY")) {
        GTEST_SKIP() << "WAYLAND_DISPLAY is not set";
      }
      if (std::getenv("APOLLO_SKIP_LIVE_HYPRLAND")) {
        GTEST_SKIP() << "APOLLO_SKIP_LIVE_HYPRLAND is set";
      }
      if (platf::ext_display_names().empty()) {
        GTEST_SKIP() << "The compositor does not support ext-image-copy-capture";
      }
    }
  };
}  // namespace

// Bugs caught: 0, False positives: 1
TEST_F(WaylandHdrLiveTest, VideoMemoryFramesOfAnHdrOutputBecomePq) {
  const auto node = test_wl::drm_render_node();
  if (node.empty()) {
    GTEST_SKIP() << "No DRM render node (no GPU) for the GL converter";
  }
  auto output = test_wl::headless_output_t::create("1280x720@60");
  ASSERT_TRUE(output) << "Couldn't create a headless output (is this Hyprland?)";
  test_wl::damage_source_t damage {output->name};
  ASSERT_TRUE(damage.ok());

  hyprland::monitor_rule_t rule {.output = output->name};
  hyprland::apply_hdr_fields(rule, true);
  ASSERT_TRUE(output->ipc->set_monitor(rule));
  wl::output_color_t color;
  if (!color.open(output->name)) {
    GTEST_SKIP() << "The compositor does not describe output colors (no wp-color-management-v1)";
  }
  for (int x = 0; x < 100 && !color.is_hdr(); ++x) {
    std::this_thread::sleep_for(20ms);
    color.poll_changed();
  }
  ASSERT_TRUE(color.is_hdr());
  EXPECT_FLOAT_EQ(color.sdr_white_nits(), 203.0f);

  video::config_t config {};
  config.width = 1280;
  config.height = 720;
  config.framerate = 60;
  auto disp = wl::make_ext_display(platf::mem_type_e::cuda, output->name, config, {});
  ASSERT_TRUE(disp);
  EXPECT_TRUE(disp->is_hdr());
  EXPECT_TRUE(disp->hdr_supported());

  // One captured frame.
  std::shared_ptr<platf::img_t> frame;
  std::vector<std::shared_ptr<platf::img_t>> pool;
  bool cursor = false;
  auto status = disp->capture(
    [&](std::shared_ptr<platf::img_t> &&img, bool captured) {
      if (captured && img) {
        frame = std::move(img);
      }
      return !frame;
    },
    [&](std::shared_ptr<platf::img_t> &img_out) {
      pool.push_back(disp->alloc_img());
      img_out = pool.back();
      return true;
    },
    &cursor
  );
  ASSERT_EQ(status, platf::capture_e::ok);
  ASSERT_TRUE(frame);
  auto &descriptor = static_cast<egl::img_descriptor_t &>(*frame);
  EXPECT_FLOAT_EQ(descriptor.sdr_white_nits, 203.0f);
  std::cout << output->name << ": captured format " << util::hex(descriptor.sd.fourcc).to_string_view() << std::endl;

  // The GL converter, set up like the CUDA path does.
  int fd = ::open(node.c_str(), O_RDWR | O_CLOEXEC);
  ASSERT_GE(fd, 0);
  {
    gbm::gbm_t gbm;
    gbm.reset(gbm::create_device(fd));
    ASSERT_TRUE(gbm);
    auto display = egl::make_display(gbm.get());
    ASSERT_TRUE(display);
    auto ctx = egl::make_ctx(display.get());
    ASSERT_TRUE(ctx);

    auto nv12 = egl::create_nv12_target(1280, 720, AV_PIX_FMT_P010);
    ASSERT_TRUE(nv12);
    auto sws = egl::sws_t::make(1280, 720, 1280, 720, AV_PIX_FMT_P010, false);
    ASSERT_TRUE(sws);
    const video::sunshine_colorspace_t colorspace {video::colorspace_e::bt2020, false, 10};
    sws->apply_colorspace(colorspace, false);

    auto rgb = egl::import_source(display.get(), descriptor.sd);
    ASSERT_TRUE(rgb);
    sws->load_vram(descriptor, 0, 0, (*rgb)->tex[0], false);
    ASSERT_EQ(sws->convert_nv12((*nv12)->buf), 0);

    std::vector<std::uint16_t> y_plane(1280 * 720);
    gl::ctx.GetTextureSubImage((*nv12)->tex[0], 0, 0, 0, 0, 1280, 720, 1, GL_RED, GL_UNSIGNED_SHORT, y_plane.size() * 2, y_plane.data());
    const double center = y_plane[360 * 1280 + 640];

    // The captured pixel (the damage source paints opaque red or blue; Hyprland's round trip through the HDR output
    // may shift it a little), and what the converter must make of it.
    std::vector<std::uint8_t> rgba(1280 * 720 * 4);
    gl::ctx.GetTextureSubImage((*rgb)->tex[0], 0, 0, 0, 0, 1280, 720, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba.size(), rgba.data());
    const auto *pixel = &rgba[(360 * 1280 + 640) * 4];
    const std::array<double, 3> captured {pixel[0] / 255.0, pixel[1] / 255.0, pixel[2] / 255.0};
    EXPECT_TRUE((pixel[0] > 200 && pixel[2] < 60) || (pixel[2] > 200 && pixel[0] < 60)) << "neither red nor blue";

    const auto expected = expected_y(captured, 203, colorspace);
    const auto unconverted = expected_y(captured, 10000, colorspace);  // roughly what 1.0 = 10000 cd/m2 would give
    std::cout << "captured RGB " << (int) pixel[0] << ',' << (int) pixel[1] << ',' << (int) pixel[2] << ": Y " << center / 64
              << " (10-bit), expected " << expected / 64 << std::endl;
    // Within one 10-bit code.
    EXPECT_NEAR(center, expected, 64) << "unconverted would be about " << unconverted / 64;

    // An SDR output's frames are not converted, even in an HDR stream (e.g. a PQ framebuffer from KMS).
    descriptor.sdr_white_nits = 0;
    sws->load_vram(descriptor, 0, 0, (*rgb)->tex[0], false);
    ASSERT_EQ(sws->convert_nv12((*nv12)->buf), 0);
    gl::ctx.GetTextureSubImage((*nv12)->tex[0], 0, 0, 0, 0, 1280, 720, 1, GL_RED, GL_UNSIGNED_SHORT, y_plane.size() * 2, y_plane.data());
    EXPECT_GT(std::abs(y_plane[360 * 1280 + 640] - center), 128) << "the conversion did not switch off";
  }
  ::close(fd);
}

#endif
