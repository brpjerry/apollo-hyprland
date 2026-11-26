/**
 * @file tests/unit/platform/test_wayland_live.cpp
 * @brief Wayland output enumeration, selection and capture against the running compositor.
 * @details Skipped unless WAYLAND_DISPLAY points at a compositor with wlr-screencopy or ext-image-copy-capture
 *          (e.g. Hyprland, Sway). Tests that need a headless output also need Hyprland (they create and remove
 *          APOLLO-TEST-* outputs).
 *          Set APOLLO_TEST_EXPECT_OUTPUTS to a comma separated list of output names that must be present.
 *          Set APOLLO_TEST_BENCHMARK=1 to measure capture rates of a 3840x2160@120 headless output.
 */
#include "../../tests_common.h"

#if defined(__linux__) && defined(SUNSHINE_BUILD_WAYLAND)

  #include "wayland_test_output.h"

  #include <src/input.h>
  #include <src/platform/common.h>
  #include <src/platform/linux/graphics.h>
  #include <src/platform/linux/hyprland/vdisplay.h>
  #include <src/platform/linux/wayland.h>
  #include <src/video.h>

namespace video {
  input::touch_port_t make_port(platf::display_t *display, const config_t &config);
}  // namespace video

namespace platf {
  std::shared_ptr<display_t> wl_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config);
  std::shared_ptr<display_t> ext_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config);
  std::vector<std::string> ext_display_names();
}  // namespace platf

namespace {
  struct WaylandLiveTest: PlatformTestSuite {
    void SetUp() override {
      if (!std::getenv("WAYLAND_DISPLAY")) {
        GTEST_SKIP() << "WAYLAND_DISPLAY is not set";
      }
      names = platf::display_names(platf::mem_type_e::system);
      if (names.empty()) {
        GTEST_SKIP() << "No outputs available through wlr-screencopy";
      }
    }

    static video::config_t config() {
      video::config_t config {};
      config.width = 1920;
      config.height = 1080;
      config.framerate = 60;
      return config;
    }

    std::vector<std::string> names;
  };

  std::vector<std::string> split(std::string_view list) {
    std::vector<std::string> result;
    while (!list.empty()) {
      auto comma = list.find(',');
      auto item = list.substr(0, comma);
      if (!item.empty()) {
        result.emplace_back(item);
      }
      if (comma == std::string_view::npos) {
        break;
      }
      list.remove_prefix(comma + 1);
    }
    return result;
  }
}  // namespace

// Bugs caught: 0, False positives: 0
TEST_F(WaylandLiveTest, EnumeratesOutputsByConnectorName) {
  for (const auto &name : names) {
    std::cout << "Wayland output: " << name << std::endl;
    EXPECT_FALSE(name.empty());
  }

  if (const char *expected = std::getenv("APOLLO_TEST_EXPECT_OUTPUTS")) {
    for (const auto &name : split(expected)) {
      EXPECT_NE(std::find(names.begin(), names.end(), name), names.end()) << name << " is missing";
    }
  }
}

// Bugs caught: 0, False positives: 0
TEST_F(WaylandLiveTest, SelectsOutputsByNameAndLegacyIndex) {
  for (std::size_t x = 0; x < names.size(); ++x) {
    auto by_name = platf::display(platf::mem_type_e::system, names[x], config());
    ASSERT_TRUE(by_name) << names[x];
    auto by_index = platf::display(platf::mem_type_e::system, std::to_string(x), config());
    ASSERT_TRUE(by_index) << x;

    std::cout << names[x] << ": frame " << by_name->width << 'x' << by_name->height
              << ", logical " << by_name->logical_width << 'x' << by_name->logical_height
              << " at " << by_name->offset_x << ',' << by_name->offset_y
              << ", layout " << by_name->env_width << 'x' << by_name->env_height
              << " at " << by_name->env_offset_x << ',' << by_name->env_offset_y << std::endl;

    EXPECT_GT(by_name->logical_width, 0);
    EXPECT_GT(by_name->logical_height, 0);
    EXPECT_LE(by_name->env_offset_x, by_name->offset_x);
    EXPECT_LE(by_name->env_offset_y, by_name->offset_y);
    EXPECT_GE(by_name->env_offset_x + by_name->env_width, by_name->offset_x + by_name->logical_width);
    EXPECT_GE(by_name->env_offset_y + by_name->env_height, by_name->offset_y + by_name->logical_height);

    EXPECT_EQ(by_index->offset_x, by_name->offset_x);
    EXPECT_EQ(by_index->offset_y, by_name->offset_y);
    EXPECT_EQ(by_index->width, by_name->width);
    EXPECT_EQ(by_index->height, by_name->height);
  }
}

// Bugs caught: 0, False positives: 0
TEST_F(WaylandLiveTest, MissingOutputIsAnError) {
  EXPECT_FALSE(platf::display(platf::mem_type_e::system, "APOLLO-TEST-does-not-exist", config()));
  EXPECT_FALSE(platf::display(platf::mem_type_e::system, std::to_string(names.size()), config()));
  // wlr-screencopy too: Sunshine's version fell back to the first monitor, which would stream the host's desktop.
  EXPECT_FALSE(platf::wl_display(platf::mem_type_e::system, "APOLLO-TEST-does-not-exist", config()));
}

// Bugs caught: 1, False positives: 0
TEST_F(WaylandLiveTest, CapturedFrameMatchesReportedSize) {
  for (const auto &name : names) {
    auto disp = platf::display(platf::mem_type_e::system, name, config());
    ASSERT_TRUE(disp) << name;

    bool got_frame = false;
    bool cursor = false;
    auto status = disp->capture(
      [&](std::shared_ptr<platf::img_t> &&, bool frame_captured) {
        got_frame = frame_captured;
        return false;
      },
      [&](std::shared_ptr<platf::img_t> &img_out) {
        img_out = disp->alloc_img();
        return true;
      },
      &cursor
    );

    std::cout << name << ": capture status " << (int) status << ", frame " << (got_frame ? "captured" : "timed out") << std::endl;
    EXPECT_EQ(status, platf::capture_e::ok) << name << ": screencopy frame size differs from " << disp->width << 'x' << disp->height;
  }
}

namespace {
  using namespace std::literals;

  video::config_t config_at(int framerate) {
    video::config_t config {};
    config.width = 1920;
    config.height = 1080;
    config.framerate = framerate;
    return config;
  }

  bool is_vram(platf::mem_type_e mem_type) {
    return mem_type != platf::mem_type_e::system;
  }

  struct capture_run_t {
    platf::capture_e status {platf::capture_e::capture_error};
    int frames {0};  ///< Images pushed with frame_captured = true.
    int wrong_size {0};
    int non_monotonic {0};
    int bad_pixels {0};  ///< System memory frames whose center pixel is neither of the damage source's colors.
    int missing_fds {0};  ///< Video memory frames without a dma-buf.
    std::chrono::nanoseconds elapsed {};
    std::chrono::nanoseconds cpu {};  ///< CPU time of the capturing thread.
    std::chrono::nanoseconds latency_sum {};  ///< Sum over frames of (push time - frame timestamp).
    wl::capture_stats_t stats;

    double fps() const {
      return elapsed.count() ? frames * 1e9 / elapsed.count() : 0.0;
    }
  };

  /**
   * @brief Runs disp->capture() like video.cpp does: a pool of reused images, and an "encoder" that holds on to the
   *        newest image until the next one arrives.
   * @param on_frame Called after every pushed frame with the frame count.
   */
  capture_run_t run_capture(platf::display_t &disp, platf::mem_type_e mem_type, int frames_wanted, std::chrono::milliseconds max_time, bool cursor = true, bool check_colors = false, const std::function<void(int)> &on_frame = {}) {
    capture_run_t run;
    std::vector<std::shared_ptr<platf::img_t>> pool(12);
    std::shared_ptr<platf::img_t> encoder_holds;
    std::optional<std::chrono::steady_clock::time_point> last_timestamp;

    auto thread_cpu = []() {
      timespec ts {};
      clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
      return std::chrono::seconds(ts.tv_sec) + std::chrono::nanoseconds(ts.tv_nsec);
    };

    const auto start = std::chrono::steady_clock::now();
    const auto cpu_start = thread_cpu();
    const auto deadline = start + max_time;

    run.status = disp.capture(
      [&](std::shared_ptr<platf::img_t> &&img, bool frame_captured) {
        if (frame_captured && img) {
          ++run.frames;
          if (img->width != disp.width || img->height != disp.height) {
            ++run.wrong_size;
          }
          if (!img->frame_timestamp || (last_timestamp && *img->frame_timestamp <= *last_timestamp)) {
            ++run.non_monotonic;
          }
          if (img->frame_timestamp) {
            last_timestamp = img->frame_timestamp;
            run.latency_sum += std::chrono::steady_clock::now() - *img->frame_timestamp;
          }

          if (is_vram(mem_type)) {
            auto descriptor = static_cast<egl::img_descriptor_t *>(img.get());
            if (descriptor->sd.fds[0] < 0 || descriptor->sd.width != disp.width || descriptor->sd.height != disp.height) {
              ++run.missing_fds;
            }
          } else if (check_colors) {
            // The damage source paints the output red or blue; frames are BGRA.
            auto pixel = img->data + (img->height / 2) * img->row_pitch + (img->width / 2) * 4;
            const bool red = pixel[0] < 16 && pixel[1] < 16 && pixel[2] > 240;
            const bool blue = pixel[0] > 240 && pixel[1] < 16 && pixel[2] < 16;
            if (!red && !blue) {
              if (!run.bad_pixels) {
                std::cout << "frame " << run.frames << " center pixel BGRA " << (int) pixel[0] << ',' << (int) pixel[1] << ',' << (int) pixel[2] << ',' << (int) pixel[3] << std::endl;
              }
              ++run.bad_pixels;
            }
          }

          encoder_holds = std::move(img);
          if (on_frame) {
            on_frame(run.frames);
          }
        }
        return run.frames < frames_wanted && std::chrono::steady_clock::now() < deadline;
      },
      [&](std::shared_ptr<platf::img_t> &img_out) {
        for (auto &img : pool) {
          if (img && img.use_count() == 1) {
            img_out = img;
            img_out->frame_timestamp.reset();
            return true;
          }
        }
        for (auto &img : pool) {
          if (!img) {
            img = disp.alloc_img();
            img_out = img;
            return true;
          }
        }
        return false;
      },
      &cursor
    );

    run.elapsed = std::chrono::steady_clock::now() - start;
    run.cpu = thread_cpu() - cpu_start;
    if (auto source = dynamic_cast<wl::capture_stats_source_t *>(&disp)) {
      run.stats = source->capture_stats();
    }
    return run;
  }

  struct ext_case_t {
    const char *name;
    platf::mem_type_e mem_type;
    bool prefer_shm;
  };

  const ext_case_t ext_cases[] {
    {"ram_dmabuf", platf::mem_type_e::system, false},
    {"ram_shm", platf::mem_type_e::system, true},
    {"vram", platf::mem_type_e::cuda, false},
  };

  struct ExtCaptureLiveTest: PlatformTestSuite, testing::WithParamInterface<ext_case_t> {
    void SetUp() override {
      if (!std::getenv("WAYLAND_DISPLAY")) {
        GTEST_SKIP() << "WAYLAND_DISPLAY is not set";
      }
      if (platf::ext_display_names().empty()) {
        GTEST_SKIP() << "The compositor does not support ext-image-copy-capture";
      }
      // Since Sunshine #5873, a capture GPU that isn't the CUDA device hands CUDA frames over through system memory,
      // so there are no dma-bufs to check (CI: vkms and llvmpipe, no NVIDIA GPU).
      if (is_vram(GetParam().mem_type) && !wl::use_vram_capture(GetParam().mem_type)) {
        GTEST_SKIP() << "The capture GPU is not the CUDA device; video memory frames go through the RAM bridge";
      }
    }

    std::shared_ptr<platf::display_t> make(const std::string &name, int framerate = 120) {
      wl::ext_options_t options;
      options.prefer_shm = GetParam().prefer_shm;
      return wl::make_ext_display(GetParam().mem_type, name, config_at(framerate), options);
    }

    std::unique_ptr<test_wl::headless_output_t> headless(const std::string &mode) {
      auto output = test_wl::headless_output_t::create(mode);
      if (!output) {
        ADD_FAILURE() << "Couldn't create a headless output (is this Hyprland?)";
      }
      return output;
    }
  };

  std::string ext_case_name(const testing::TestParamInfo<ext_case_t> &info) {
    return info.param.name;
  }
}  // namespace

// Bugs caught: 0, False positives: 3
TEST_P(ExtCaptureLiveTest, CapturesHeadlessOutputIntoReusedBuffers) {
  auto output = headless("1920x1080@120");
  ASSERT_TRUE(output);
  test_wl::damage_source_t damage {output->name};
  ASSERT_TRUE(damage.ok()) << "layer-shell damage source failed on " << output->name;

  auto disp = make(output->name);
  ASSERT_TRUE(disp);
  EXPECT_EQ(disp->width, 1920);
  EXPECT_EQ(disp->height, 1080);

  // Hide the cursor: the session is reopened without paint_cursors.
  // The time limit only catches a stalled capture: CI renders in software (llvmpipe) and manages 20-30 fps there.
  auto run = run_capture(*disp, GetParam().mem_type, 240, 30s, false, true);
  std::cout << output->name << ' ' << GetParam().name << ": " << run.frames << " frames in " << run.elapsed.count() / 1e6 << " ms ("
            << run.fps() << " fps), captured " << run.stats.frames_captured << ", buffers allocated " << run.stats.buffers_allocated << std::endl;

  EXPECT_EQ(run.status, platf::capture_e::ok);
  EXPECT_EQ(run.frames, 240);
  EXPECT_EQ(run.wrong_size, 0);
  EXPECT_EQ(run.non_monotonic, 0);
  EXPECT_EQ(run.bad_pixels, 0);
  EXPECT_EQ(run.missing_fds, 0);
  // The output changes every frame, so (nearly) every delivered frame is a new capture, not a repeat.
  EXPECT_GE(run.stats.frames_captured, 200u);
  // A small pool, reused: no allocation per frame.
  EXPECT_GE(run.stats.buffers_allocated, 1u);
  EXPECT_LE(run.stats.buffers_allocated, is_vram(GetParam().mem_type) ? 4u : 2u);
}

// Bugs caught: 0, False positives: 0
TEST_P(ExtCaptureLiveTest, CapturesDP3) {
  auto names = platf::ext_display_names();
  if (std::find(names.begin(), names.end(), "DP-3") == names.end()) {
    GTEST_SKIP() << "No DP-3 output";
  }

  auto disp = make("DP-3");
  ASSERT_TRUE(disp);

  // DP-3 may be static: frames are then repeats of the last capture (every 100 ms).
  auto run = run_capture(*disp, GetParam().mem_type, 10, 5s);
  std::cout << "DP-3 " << GetParam().name << ": " << disp->width << 'x' << disp->height << ", " << run.frames << " frames, captured "
            << run.stats.frames_captured << ", buffers allocated " << run.stats.buffers_allocated << std::endl;

  EXPECT_EQ(run.status, platf::capture_e::ok);
  EXPECT_EQ(run.frames, 10);
  EXPECT_EQ(run.wrong_size, 0);
  EXPECT_EQ(run.non_monotonic, 0);
  EXPECT_EQ(run.missing_fds, 0);
  EXPECT_GE(run.stats.frames_captured, 1u);
  EXPECT_LE(run.stats.buffers_allocated, is_vram(GetParam().mem_type) ? 4u : 2u);
}

// Bugs caught: 0, False positives: 0
TEST_P(ExtCaptureLiveTest, ReinitializesWhenTheOutputIsRemoved) {
  auto output = headless("1280x720@60");
  ASSERT_TRUE(output);
  test_wl::damage_source_t damage {output->name};
  ASSERT_TRUE(damage.ok());

  auto disp = make(output->name, 60);
  ASSERT_TRUE(disp);

  std::optional<std::chrono::steady_clock::time_point> removed_at;
  auto run = run_capture(*disp, GetParam().mem_type, 100000, 10s, true, false, [&](int frames) {
    if (frames == 10 && !removed_at) {
      output->remove();
      removed_at = std::chrono::steady_clock::now();
    }
  });

  ASSERT_TRUE(removed_at);
  auto reaction = std::chrono::steady_clock::now() - *removed_at;
  std::cout << GetParam().name << ": capture returned " << (int) run.status << " "
            << std::chrono::duration_cast<std::chrono::milliseconds>(reaction).count() << " ms after the output was removed" << std::endl;
  EXPECT_EQ(run.status, platf::capture_e::reinit);
  EXPECT_LT(reaction, 2s);

  // The output is gone, so a new display for it cannot be created.
  EXPECT_FALSE(make(output->name));
}

namespace {
  /// Switches a headless test output between HDR and SDR with the rule Apollo uses for its virtual displays.
  bool set_test_output_hdr(test_wl::headless_output_t &output, bool hdr) {
    hyprland::monitor_rule_t rule {.output = output.name};
    hyprland::apply_hdr_fields(rule, hdr);
    return output.ipc->set_monitor(rule);
  }
}  // namespace

// Bugs caught: 0, False positives: 0
TEST_P(ExtCaptureLiveTest, ReinitializesWhenTheOutputSwitchesToHdr) {
  if (std::getenv("APOLLO_SKIP_LIVE_HYPRLAND")) {
    GTEST_SKIP() << "APOLLO_SKIP_LIVE_HYPRLAND is set";
  }
  auto output = headless("1280x720@60");
  ASSERT_TRUE(output);
  test_wl::damage_source_t damage {output->name};
  ASSERT_TRUE(damage.ok());

  auto disp = make(output->name, 60);
  ASSERT_TRUE(disp);
  EXPECT_FALSE(disp->is_hdr());

  std::optional<std::chrono::steady_clock::time_point> switched_at;
  auto run = run_capture(*disp, GetParam().mem_type, 100000, 10s, true, false, [&](int frames) {
    if (frames == 10 && !switched_at) {
      ASSERT_TRUE(set_test_output_hdr(*output, true));
      switched_at = std::chrono::steady_clock::now();
    }
  });

  ASSERT_TRUE(switched_at);
  auto reaction = std::chrono::steady_clock::now() - *switched_at;
  std::cout << GetParam().name << ": capture returned " << (int) run.status << " "
            << std::chrono::duration_cast<std::chrono::milliseconds>(reaction).count() << " ms after HDR was switched on" << std::endl;
  EXPECT_EQ(run.status, platf::capture_e::reinit);
  EXPECT_LT(reaction, 2s);

  // The new display reports HDR (and metadata) exactly when its encode path can produce PQ frames.
  disp = make(output->name, 60);
  ASSERT_TRUE(disp);
  SS_HDR_METADATA metadata;
  EXPECT_EQ(disp->get_hdr_metadata(metadata), disp->is_hdr());
  if (disp->is_hdr()) {
    EXPECT_EQ(metadata.maxDisplayLuminance, 1000);
    EXPECT_EQ(metadata.minDisplayLuminance, 50);
    EXPECT_EQ(metadata.maxContentLightLevel, 1000);
    EXPECT_EQ(metadata.maxFrameAverageLightLevel, 400);
  }
  EXPECT_EQ(disp->hdr_supported(), disp->is_hdr());
}

INSTANTIATE_TEST_SUITE_P(Wayland, ExtCaptureLiveTest, testing::ValuesIn(ext_cases), ext_case_name);

// Bugs caught: 0, False positives: 0
TEST(WaylandColorLiveTest, ReadsHdrStateAndMetadataOfAnOutput) {
  if (!std::getenv("WAYLAND_DISPLAY")) {
    GTEST_SKIP() << "WAYLAND_DISPLAY is not set";
  }
  if (std::getenv("APOLLO_SKIP_LIVE_HYPRLAND")) {
    GTEST_SKIP() << "APOLLO_SKIP_LIVE_HYPRLAND is set";
  }
  auto output = test_wl::headless_output_t::create("1280x720@60");
  if (!output) {
    GTEST_SKIP() << "Couldn't create a headless output (is this Hyprland?)";
  }

  wl::output_color_t color;
  if (!color.open(output->name)) {
    GTEST_SKIP() << "The compositor does not describe output colors (no wp-color-management-v1)";
  }
  ASSERT_TRUE(color.description());
  std::cout << output->name << " (SDR): " << wl::describe(*color.description()) << std::endl;
  EXPECT_FALSE(color.is_hdr());
  EXPECT_TRUE(color.compositor_supports_hdr10());
  EXPECT_FALSE(color.poll_changed());

  auto wait_for_change = [&]() {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
      if (color.poll_changed()) {
        return true;
      }
      std::this_thread::sleep_for(10ms);
    }
    return false;
  };

  ASSERT_TRUE(set_test_output_hdr(*output, true));
  ASSERT_TRUE(wait_for_change());
  ASSERT_TRUE(color.description());
  std::cout << output->name << " (HDR): " << wl::describe(*color.description()) << std::endl;
  EXPECT_TRUE(color.is_hdr());
  SS_HDR_METADATA metadata;
  ASSERT_TRUE(wl::fill_hdr_metadata(*color.description(), metadata));
  EXPECT_EQ(metadata.displayPrimaries[0].x, 35400);
  EXPECT_EQ(metadata.whitePoint.x, 15635);
  EXPECT_EQ(metadata.maxDisplayLuminance, 1000);
  EXPECT_EQ(metadata.minDisplayLuminance, 50);
  EXPECT_EQ(metadata.maxContentLightLevel, 1000);
  EXPECT_EQ(metadata.maxFrameAverageLightLevel, 400);

  ASSERT_TRUE(set_test_output_hdr(*output, false));
  ASSERT_TRUE(wait_for_change());
  EXPECT_FALSE(color.is_hdr());
}

// Bugs caught: 0, False positives: 0
TEST_F(WaylandLiveTest, ExtAndWlrReportTheSameGeometry) {
  if (platf::ext_display_names().empty()) {
    GTEST_SKIP() << "The compositor does not support ext-image-copy-capture";
  }

  for (const auto &name : names) {
    auto wlr = platf::wl_display(platf::mem_type_e::system, name, config());
    auto ext = platf::ext_display(platf::mem_type_e::system, name, config());
    ASSERT_TRUE(wlr) << name;
    ASSERT_TRUE(ext) << name;

    EXPECT_EQ(ext->width, wlr->width) << name;
    EXPECT_EQ(ext->height, wlr->height) << name;
    EXPECT_EQ(ext->offset_x, wlr->offset_x) << name;
    EXPECT_EQ(ext->offset_y, wlr->offset_y) << name;
    EXPECT_EQ(ext->logical_width, wlr->logical_width) << name;
    EXPECT_EQ(ext->logical_height, wlr->logical_height) << name;
    EXPECT_EQ(ext->env_offset_x, wlr->env_offset_x) << name;
    EXPECT_EQ(ext->env_offset_y, wlr->env_offset_y) << name;
    // Absolute input uses the logical layout. env_width/env_height differ on scaled outputs: the wlr backend
    // (upstream) unions logical offsets with mode sizes, the ext backend keeps the whole layout logical.
    EXPECT_EQ(ext->env_logical_width, wlr->env_logical_width) << name;
    EXPECT_EQ(ext->env_logical_height, wlr->env_logical_height) << name;

    auto config = WaylandLiveTest::config();
    auto wlr_port = video::make_port(wlr.get(), config);
    auto ext_port = video::make_port(ext.get(), config);
    EXPECT_EQ(ext_port.offset_x, wlr_port.offset_x) << name;
    EXPECT_EQ(ext_port.offset_y, wlr_port.offset_y) << name;
    EXPECT_EQ(ext_port.width, wlr_port.width) << name;
    EXPECT_EQ(ext_port.height, wlr_port.height) << name;
    EXPECT_EQ(ext_port.env_offset_x, wlr_port.env_offset_x) << name;
    EXPECT_EQ(ext_port.env_offset_y, wlr_port.env_offset_y) << name;
    EXPECT_EQ(ext_port.env_logical_width, wlr_port.env_logical_width) << name;
    EXPECT_EQ(ext_port.env_logical_height, wlr_port.env_logical_height) << name;
    EXPECT_FLOAT_EQ(ext_port.scalar_inv, wlr_port.scalar_inv) << name;
    EXPECT_FLOAT_EQ(ext_port.client_offsetX, wlr_port.client_offsetX) << name;
    EXPECT_FLOAT_EQ(ext_port.client_offsetY, wlr_port.client_offsetY) << name;
  }
}

namespace {
  struct WlrCaptureLiveTest: PlatformTestSuite, testing::WithParamInterface<platf::mem_type_e> {
    void SetUp() override {
      if (!std::getenv("WAYLAND_DISPLAY")) {
        GTEST_SKIP() << "WAYLAND_DISPLAY is not set";
      }
    }
  };
}  // namespace

// Bugs caught: 0, False positives: 2
TEST_P(WlrCaptureLiveTest, ReusesBuffersAcrossFrames) {
  if (is_vram(GetParam()) && test_wl::drm_render_node().empty()) {
    GTEST_SKIP() << "No DRM render node (no GPU) to allocate dma-bufs on";
  }
  auto output = test_wl::headless_output_t::create("1920x1080@120");
  ASSERT_TRUE(output) << "Couldn't create a headless output (is this Hyprland?)";
  test_wl::damage_source_t damage {output->name};
  ASSERT_TRUE(damage.ok());

  auto disp = platf::wl_display(GetParam(), output->name, config_at(120));
  ASSERT_TRUE(disp);

  auto run = run_capture(*disp, GetParam(), 120, 10s, true, true);
  std::cout << "wlr " << (is_vram(GetParam()) ? "vram" : "ram") << ": " << run.frames << " frames (" << run.fps() << " fps), buffers allocated "
            << run.stats.buffers_allocated << std::endl;

  EXPECT_EQ(run.status, platf::capture_e::ok);
  EXPECT_EQ(run.frames, 120);
  EXPECT_EQ(run.wrong_size, 0);
  EXPECT_EQ(run.bad_pixels, 0);
  EXPECT_EQ(run.missing_fds, 0);
  EXPECT_LE(run.stats.buffers_allocated, is_vram(GetParam()) ? 3u : 1u);
}

INSTANTIATE_TEST_SUITE_P(Wayland, WlrCaptureLiveTest, testing::Values(platf::mem_type_e::system, platf::mem_type_e::cuda), [](const auto &info) {
  return std::string {info.param == platf::mem_type_e::system ? "ram" : "vram"};
});

// Bugs caught: 0, False positives: 0
TEST(WaylandCaptureBenchmark, Headless4K120) {
  if (!std::getenv("APOLLO_TEST_BENCHMARK") || !std::getenv("WAYLAND_DISPLAY")) {
    GTEST_SKIP() << "Set APOLLO_TEST_BENCHMARK=1 to run";
  }
  auto platform = platf::init();
  ASSERT_TRUE(platform);

  auto output = test_wl::headless_output_t::create("3840x2160@120");
  ASSERT_TRUE(output);
  test_wl::damage_source_t damage {output->name};
  ASSERT_TRUE(damage.ok());

  struct backend_t {
    const char *name;
    std::function<std::shared_ptr<platf::display_t>()> make;
    platf::mem_type_e mem_type;
  };

  const auto config = config_at(120);
  const std::vector<backend_t> backends {
    {"wlr ram", [&]() {
       return platf::wl_display(platf::mem_type_e::system, output->name, config);
     },
     platf::mem_type_e::system},
    {"wlr vram", [&]() {
       return platf::wl_display(platf::mem_type_e::cuda, output->name, config);
     },
     platf::mem_type_e::cuda},
    {"ext ram (dma-buf)", [&]() {
       return wl::make_ext_display(platf::mem_type_e::system, output->name, config, {});
     },
     platf::mem_type_e::system},
    {"ext ram (shm)", [&]() {
       return wl::make_ext_display(platf::mem_type_e::system, output->name, config, {true});
     },
     platf::mem_type_e::system},
    {"ext vram", [&]() {
       return wl::make_ext_display(platf::mem_type_e::cuda, output->name, config, {});
     },
     platf::mem_type_e::cuda},
  };

  for (const auto &backend : backends) {
    auto disp = backend.make();
    ASSERT_TRUE(disp) << backend.name;

    // Warm up, then measure 5 s.
    run_capture(*disp, backend.mem_type, 60, 2s);
    const auto damage_before = damage.frames.load();
    auto run = run_capture(*disp, backend.mem_type, 100000, 5s);
    const auto damage_fps = (damage.frames.load() - damage_before) * 1e9 / run.elapsed.count();
    std::cout << "BENCHMARK " << backend.name << ": " << run.fps() << " fps delivered (" << run.frames << " frames), "
              << run.stats.frames_captured << " captured, " << run.stats.buffers_allocated << " buffers allocated, capture thread CPU "
              << 100.0 * run.cpu.count() / run.elapsed.count() << "%, frame timestamp to delivery "
              << (run.frames ? run.latency_sum.count() / 1e6 / run.frames : 0.0) << " ms; damage source at " << damage_fps << " fps" << std::endl;
    EXPECT_EQ(run.status, platf::capture_e::ok);
  }
}

#endif
