/**
 * @file tests/unit/platform/test_nvenc_live.cpp
 * @brief Native NVENC on Linux: frames captured from a headless Hyprland output, encoded by the real driver.
 * @details Captures through ext-image-copy-capture (dma-bufs, and the system memory bridge), encodes with the native
 *          encoder and decodes the result with FFmpeg. Checks 4:4:4, 10-bit, and that a lost frame is recovered by
 *          reference frame invalidation rather than an IDR frame. Skipped without a compositor with
 *          ext-image-copy-capture, without a CUDA device, or with APOLLO_SKIP_LIVE_HYPRLAND set.
 */
#include "../../tests_common.h"

#if defined(__linux__) && defined(SUNSHINE_BUILD_WAYLAND) && defined(SUNSHINE_BUILD_CUDA)

  #include "wayland_test_output.h"

  #include <src/nvenc/nvenc_encoder.h>
  #include <src/platform/common.h>
  #include <src/platform/linux/graphics.h>
  #include <src/platform/linux/wayland.h>
  #include <src/video.h>
  #include <src/video_colorspace.h>

extern "C" {
  #include <libavcodec/avcodec.h>
  #include <libavutil/pixdesc.h>
}

namespace platf {
  std::vector<std::string> ext_display_names();
}  // namespace platf

namespace {
  using namespace std::literals;

  /**
   * @brief One stream configuration to encode.
   */
  struct nvenc_case_t {
    const char *name;  ///< Test name suffix.
    int video_format;  ///< 0: H.264, 1: HEVC, 2: AV1.
    int chroma;  ///< 0: 4:2:0, 1: 4:4:4.
    int dynamic_range;  ///< 0: 8-bit, 1: 10-bit.
    bool system_memory;  ///< Capture through the GPU -> RAM -> GPU bridge.
  };

  /**
   * @brief FFmpeg decoder for the encoded stream.
   */
  struct decoder_t {
    /**
     * @brief Open a decoder for a video format.
     *
     * @param video_format 0: H.264, 1: HEVC, 2: AV1.
     */
    explicit decoder_t(int video_format) {
      const AVCodec *codec = video_format == 0 ? avcodec_find_decoder(AV_CODEC_ID_H264) :
                             video_format == 1 ? avcodec_find_decoder(AV_CODEC_ID_HEVC) :
                                                 avcodec_find_decoder_by_name("libdav1d");
      if (!codec) {
        return;
      }
      ctx = avcodec_alloc_context3(codec);
      if (ctx && avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
      }
      frame = av_frame_alloc();
      packet = av_packet_alloc();
    }

    ~decoder_t() {
      av_packet_free(&packet);
      av_frame_free(&frame);
      avcodec_free_context(&ctx);
    }

    decoder_t(const decoder_t &) = delete;
    decoder_t &operator=(const decoder_t &) = delete;

    /**
     * @brief Decode one frame.
     *
     * @param data Encoded frame.
     * @return True when the decoder returned an intact picture for it.
     */
    bool decode(const std::vector<std::uint8_t> &data) {
      packet->data = const_cast<std::uint8_t *>(data.data());
      packet->size = static_cast<int>(data.size());
      if (avcodec_send_packet(ctx, packet) < 0) {
        return false;
      }
      if (avcodec_receive_frame(ctx, frame) < 0) {
        return false;
      }
      return !(frame->flags & AV_FRAME_FLAG_CORRUPT) && frame->decode_error_flags == 0;
    }

    AVCodecContext *ctx = nullptr;  ///< Decoder; null when FFmpeg has none for the format.
    AVFrame *frame = nullptr;  ///< Last decoded picture.
    AVPacket *packet = nullptr;  ///< Packet wrapper for the encoded frames.
  };

  /**
   * @brief Capture one frame of a display.
   *
   * @param disp Display to capture.
   * @return Captured frame, or null on failure.
   */
  std::shared_ptr<platf::img_t> capture_one(platf::display_t &disp) {
    std::shared_ptr<platf::img_t> frame;
    std::vector<std::shared_ptr<platf::img_t>> pool;
    bool cursor = false;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    auto status = disp.capture(
      [&](std::shared_ptr<platf::img_t> &&img, bool captured) {
        if (captured && img) {
          frame = std::move(img);
        }
        return !frame && std::chrono::steady_clock::now() < deadline;
      },
      [&](std::shared_ptr<platf::img_t> &img_out) {
        pool.push_back(disp.alloc_img());
        img_out = pool.back();
        return true;
      },
      &cursor
    );
    return status == platf::capture_e::ok ? frame : nullptr;
  }

  /**
   * @brief Skips the NVENC live tests without a compositor that can capture.
   */
  struct NvencLiveBase: PlatformTestSuite {
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

  /**
   * @brief Encodes one stream configuration.
   */
  struct NvencLiveTest: NvencLiveBase, testing::WithParamInterface<nvenc_case_t> {};

  /**
   * @brief Probes the native encoder like Apollo does at startup.
   */
  struct NvencProbeLiveTest: NvencLiveBase {};

  const auto nvenc_cases = std::to_array<nvenc_case_t>({
    {"h264", 0, 0, 0, false},
    {"hevc", 1, 0, 0, false},
    {"hevc_444", 1, 1, 0, false},
    {"hevc_main10", 1, 0, 1, false},
    {"hevc_444_10bit", 1, 1, 1, false},
    {"av1", 2, 0, 0, false},
    {"av1_10bit", 2, 0, 1, false},
    {"h264_444_ram", 0, 1, 0, true},
    {"hevc_ram", 1, 0, 0, true},
  });
}  // namespace

// Bugs caught: 0, False positives: 0
TEST_P(NvencLiveTest, EncodesCapturedFramesAndRecoversALostFrameWithoutAnIdrFrame) {
  const auto &param = GetParam();
  auto output = test_wl::headless_output_t::create("1280x720@60");
  ASSERT_TRUE(output) << "Couldn't create a headless output (is this Hyprland?)";
  test_wl::damage_source_t damage {output->name};
  ASSERT_TRUE(damage.ok());

  video::config_t config {};
  config.width = 1280;
  config.height = 720;
  config.framerate = 60;
  config.bitrate = 10000;
  config.videoFormat = param.video_format;
  config.chromaSamplingType = param.chroma;
  config.dynamicRange = param.dynamic_range;
  auto disp = wl::make_ext_display(platf::mem_type_e::cuda, output->name, config, {.system_memory = param.system_memory});
  ASSERT_TRUE(disp);
  auto frame = capture_one(*disp);
  ASSERT_TRUE(frame);
  EXPECT_EQ(param.system_memory, dynamic_cast<egl::img_descriptor_t *>(frame.get()) == nullptr);

  const auto colorspace = video::colorspace_from_client_config(config, disp->is_hdr());
  const bool ten_bit = colorspace.bit_depth == 10;
  ASSERT_EQ(ten_bit, param.dynamic_range == 1);
  const auto pix_fmt = param.chroma ? (ten_bit ? platf::pix_fmt_e::yuv444p16 : platf::pix_fmt_e::yuv444p) :
                                      (ten_bit ? platf::pix_fmt_e::p010 : platf::pix_fmt_e::nv12);
  auto device = disp->make_nvenc_encode_device(pix_fmt);
  if (!device) {
    GTEST_SKIP() << "No CUDA device";
  }
  device->colorspace = colorspace;
  ASSERT_TRUE(device->init_encoder(config, colorspace));
  ASSERT_TRUE(device->nvenc);

  decoder_t decoder {param.video_format};
  ASSERT_TRUE(decoder.ctx) << "FFmpeg can't decode format " << param.video_format;

  // Frames 1-8 reach the client; 9 and 10 are lost.
  constexpr std::uint64_t frames = 10;
  for (std::uint64_t index = 1; index <= frames; ++index) {
    ASSERT_EQ(device->convert(*frame), 0);
    const auto encoded = device->nvenc->encode_frame(index, index == 1);
    ASSERT_FALSE(encoded.data.empty()) << "frame " << index;
    EXPECT_EQ(encoded.frame_index, index);
    EXPECT_EQ(encoded.idr, index == 1) << "frame " << index;
    if (index <= frames - 2) {
      EXPECT_TRUE(decoder.decode(encoded.data)) << "frame " << index;
    }
  }
  const auto *decoded_format = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(decoder.frame->format));
  ASSERT_TRUE(decoded_format);
  EXPECT_EQ(decoded_format->log2_chroma_w, param.chroma ? 0 : 1) << decoded_format->name;
  EXPECT_EQ(decoded_format->comp[0].depth, ten_bit ? 10 : 8) << decoded_format->name;
  EXPECT_EQ(decoder.frame->width, 1280);

  // The client reports the loss; the next frame references only frames it has.
  ASSERT_TRUE(device->nvenc->invalidate_ref_frames(frames - 1, frames));
  ASSERT_EQ(device->convert(*frame), 0);
  const auto recovery = device->nvenc->encode_frame(frames + 1, false);
  ASSERT_FALSE(recovery.data.empty());
  EXPECT_FALSE(recovery.idr);
  EXPECT_TRUE(recovery.after_ref_frame_invalidation);
  // Without the invalidation FFmpeg's HEVC and AV1 decoders reject this frame (checked when this test was written);
  // its H.264 decoder conceals the frame_num gap silently, so for H.264 this only checks that the frame decodes.
  EXPECT_TRUE(decoder.decode(recovery.data)) << "the recovery frame references a lost frame";
}

// Bugs caught: 0, False positives: 0
TEST_F(NvencProbeLiveTest, AdvertisesReferenceFrameInvalidationAndYuv444) {
  // The probe captures the default output: one of the private instance's headless outputs.
  if (!video::validate_encoder(video::nvenc, false)) {
    GTEST_SKIP() << "No NVENC";
  }
  video::record_encoder_probe_results(video::nvenc);
  EXPECT_TRUE(video::last_encoder_probe_supported_ref_frames_invalidation);
  EXPECT_TRUE(video::last_encoder_probe_supported_yuv444_for_codec[0]) << "H.264";
  EXPECT_TRUE(video::last_encoder_probe_supported_yuv444_for_codec[1]) << "HEVC";
  EXPECT_TRUE(video::nvenc.hevc[video::encoder_t::PASSED]);
  EXPECT_TRUE(video::nvenc.hevc[video::encoder_t::DYNAMIC_RANGE]);
  video::reset_encoder_probe_results();
}

INSTANTIATE_TEST_SUITE_P(Wayland, NvencLiveTest, testing::ValuesIn(nvenc_cases), [](const auto &info) {
  return std::string {info.param.name};
});

#endif
