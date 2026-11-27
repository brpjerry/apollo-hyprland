/**
 * @file tests/unit/platform/test_nvenc_live.cpp
 * @brief Native NVENC on Linux: frames captured from a headless Hyprland output, encoded by the real driver.
 * @details Captures through ext-image-copy-capture (dma-bufs, and the system memory bridge), encodes with the native
 *          encoder and decodes the result with FFmpeg. Checks 4:4:4, 10-bit, that a lost frame is recovered by reference
 *          frame invalidation rather than an IDR frame, and that system memory frames of an HDR output stream as HDR10.
 *          Only runs in the private headless Hyprland of scripts/hyprland/test-live.sh. Skipped without
 *          ext-image-copy-capture, without a CUDA device, or with APOLLO_SKIP_LIVE_HYPRLAND set.
 */
#include "../../tests_common.h"

#if defined(__linux__) && defined(SUNSHINE_BUILD_WAYLAND) && defined(SUNSHINE_BUILD_CUDA)

  #include "pq_reference.h"
  #include "wayland_test_output.h"

  #include <algorithm>
  #include <array>
  #include <cstring>
  #include <ffnvcodec/dynlink_loader.h>
  #include <functional>
  #include <src/nvenc/nvenc_encoder.h>
  #include <src/platform/common.h>
  #include <src/platform/linux/graphics.h>
  #include <src/platform/linux/hyprland/vdisplay.h>
  #include <src/platform/linux/wayland.h>
  #include <src/video.h>
  #include <src/video_colorspace.h>
  #include <vector>

extern "C" {
  #include <libavcodec/avcodec.h>
  #include <libavutil/pixdesc.h>
}

namespace platf {
  std::vector<std::string> ext_display_names();
  std::shared_ptr<display_t> wl_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config);
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
   * @brief What the GPU's NVENC can do, asked of the driver directly rather than through the code under test.
   */
  struct nvenc_caps_t {
    bool available = false;  ///< An NVENC session opens on CUDA device 0.
    std::array<bool, 3> codec {};  ///< H.264, HEVC and AV1 can be encoded.
    std::array<bool, 3> yuv444 {};  ///< 4:4:4 per codec.
    std::array<bool, 3> ten_bit {};  ///< 10-bit per codec.
    std::array<bool, 3> invalidation {};  ///< Reference picture invalidation with multiple reference frames, per codec.
  };

  /**
   * @brief Query one capability of a codec.
   *
   * @param api NVENC functions.
   * @param session Open session.
   * @param codec Codec GUID.
   * @param cap Capability.
   * @return Its value, 0 on error.
   */
  int encode_cap(NV_ENCODE_API_FUNCTION_LIST &api, void *session, GUID codec, NV_ENC_CAPS cap) {
    NV_ENC_CAPS_PARAM param {.version = NV_ENC_CAPS_PARAM_VER};
    param.capsToQuery = cap;
    int value = 0;
    return api.nvEncGetEncodeCaps(session, codec, &param, &value) == NV_ENC_SUCCESS ? value : 0;
  }

  /**
   * @brief Open an NVENC session of its own on CUDA device 0 and read the GPU's capabilities.
   *
   * @return Capabilities; `available` is false without a CUDA device or NVENC driver.
   */
  nvenc_caps_t query_nvenc_caps() {
    nvenc_caps_t caps;
    CudaFunctions *cu = nullptr;
    NvencFunctions *nv = nullptr;
    if (cuda_load_functions(&cu, nullptr) == 0 && nvenc_load_functions(&nv, nullptr) == 0) {
      CUdevice device = 0;
      CUcontext context = nullptr;
      int count = 0;
      if (cu->cuInit(0) == CUDA_SUCCESS && cu->cuDeviceGetCount(&count) == CUDA_SUCCESS && count > 0 && cu->cuDeviceGet(&device, 0) == CUDA_SUCCESS && cu->cuCtxCreate(&context, 0, device) == CUDA_SUCCESS) {
        NV_ENCODE_API_FUNCTION_LIST api {.version = NV_ENCODE_API_FUNCTION_LIST_VER};
        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params {.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER};
        params.device = context;
        params.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
        params.apiVersion = NVENCAPI_VERSION;
        void *session = nullptr;
        if (nv->NvEncodeAPICreateInstance(&api) == NV_ENC_SUCCESS && api.nvEncOpenEncodeSessionEx(&params, &session) == NV_ENC_SUCCESS) {
          caps.available = true;
          std::uint32_t guid_count = 0;
          api.nvEncGetEncodeGUIDCount(session, &guid_count);
          std::vector<GUID> guids(guid_count);
          api.nvEncGetEncodeGUIDs(session, guids.data(), guid_count, &guid_count);
          const std::array<GUID, 3> codecs {NV_ENC_CODEC_H264_GUID, NV_ENC_CODEC_HEVC_GUID, NV_ENC_CODEC_AV1_GUID};
          for (std::size_t x = 0; x < codecs.size(); ++x) {
            caps.codec[x] = std::ranges::any_of(guids, [&](const GUID &guid) {
              return std::memcmp(&guid, &codecs[x], sizeof(GUID)) == 0;
            });
            if (caps.codec[x]) {
              caps.yuv444[x] = encode_cap(api, session, codecs[x], NV_ENC_CAPS_SUPPORT_YUV444_ENCODE);
              caps.ten_bit[x] = encode_cap(api, session, codecs[x], NV_ENC_CAPS_SUPPORT_10BIT_ENCODE);
              caps.invalidation[x] = encode_cap(api, session, codecs[x], NV_ENC_CAPS_SUPPORT_REF_PIC_INVALIDATION) &&
                                     encode_cap(api, session, codecs[x], NV_ENC_CAPS_SUPPORT_MULTIPLE_REF_FRAMES);
            }
          }
          api.nvEncDestroyEncoder(session);
        }
        cu->cuCtxDestroy(context);
      }
    }
    nvenc_free_functions(&nv);
    cuda_free_functions(&cu);
    return caps;
  }

  /**
   * @brief The GPU's NVENC capabilities, queried once.
   *
   * @return Capabilities of CUDA device 0.
   */
  const nvenc_caps_t &nvenc_caps() {
    static const auto caps = query_nvenc_caps();
    return caps;
  }

  /**
   * @brief Capture frames of a display into fresh buffers until one is wanted.
   *
   * @param disp Display to capture.
   * @param wanted Whether a captured frame is the one to return; any frame when empty.
   * @return The wanted frame, or null on failure or after 5 seconds.
   */
  std::shared_ptr<platf::img_t> capture_first(platf::display_t &disp, const std::function<bool(const platf::img_t &)> &wanted = {}) {
    std::shared_ptr<platf::img_t> frame;
    std::vector<std::shared_ptr<platf::img_t>> pool;
    bool cursor = false;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    auto status = disp.capture(
      [&](std::shared_ptr<platf::img_t> &&img, bool captured) {
        if (captured && img && (!wanted || wanted(*img))) {
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
      // These tests create and reconfigure outputs: never on the desktop session.
      if (!test_wl::in_private_hyprland()) {
        GTEST_SKIP() << "Needs the private headless Hyprland of scripts/hyprland/test-live.sh";
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
   * @brief Streams an HDR output.
   */
  struct NvencHdrLiveTest: NvencLiveBase {};

  /**
   * @brief Checks the system memory capture displays the bridge uses.
   */
  struct SystemMemoryCaptureLiveTest: NvencLiveBase {};

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
  const auto &caps = nvenc_caps();
  if (!caps.available) {
    GTEST_SKIP() << "No NVENC";
  }
  const auto format = static_cast<std::size_t>(param.video_format);
  if (!caps.codec[format] || (param.chroma && !caps.yuv444[format]) || (param.dynamic_range && !caps.ten_bit[format])) {
    GTEST_SKIP() << "The GPU can't encode this format";
  }
  if (!param.system_memory && !wl::use_vram_capture(platf::mem_type_e::cuda)) {
    GTEST_SKIP() << "The capture GPU is not the CUDA device, so frames can't stay in video memory";
  }
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
  auto frame = capture_first(*disp);
  ASSERT_TRUE(frame);
  EXPECT_EQ(param.system_memory, dynamic_cast<egl::img_descriptor_t *>(frame.get()) == nullptr);

  const auto colorspace = video::colorspace_from_client_config(config, disp->is_hdr());
  const bool ten_bit = colorspace.bit_depth == 10;
  ASSERT_EQ(ten_bit, param.dynamic_range == 1);
  const auto pix_fmt = param.chroma ? (ten_bit ? platf::pix_fmt_e::yuv444p16 : platf::pix_fmt_e::yuv444p) :
                                      (ten_bit ? platf::pix_fmt_e::p010 : platf::pix_fmt_e::nv12);
  auto device = disp->make_nvenc_encode_device(pix_fmt);
  ASSERT_TRUE(device);
  device->colorspace = colorspace;
  ASSERT_TRUE(device->init_encoder(config, colorspace));
  ASSERT_TRUE(device->nvenc);
  EXPECT_EQ(device->nvenc->supports_ref_frame_invalidation(), caps.invalidation[format]);

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

  if (!caps.invalidation[format]) {
    // Apollo doesn't advertise invalidation then (NvencProbeLiveTest); the client asks for an IDR frame instead.
    EXPECT_FALSE(device->nvenc->invalidate_ref_frames(frames - 1, frames));
    return;
  }
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
TEST_F(NvencHdrLiveTest, SystemMemoryFramesOfAnHdrOutputStreamAsHdr10) {
  if (!nvenc_caps().ten_bit[1]) {
    GTEST_SKIP() << "No NVENC with 10-bit HEVC";
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

  video::config_t config {};
  config.width = 1280;
  config.height = 720;
  config.framerate = 60;
  config.bitrate = 20000;
  config.videoFormat = 1;
  config.dynamicRange = 1;
  auto disp = wl::make_ext_display(platf::mem_type_e::cuda, output->name, config, {.system_memory = true});
  ASSERT_TRUE(disp);
  EXPECT_TRUE(disp->is_hdr());
  EXPECT_TRUE(disp->hdr_supported());
  SS_HDR_METADATA metadata {};
  EXPECT_TRUE(disp->get_hdr_metadata(metadata));

  auto frame = capture_first(*disp);
  ASSERT_TRUE(frame);
  auto *ram = dynamic_cast<egl::ram_img_t *>(frame.get());
  ASSERT_TRUE(ram) << "not a system memory frame";
  EXPECT_FLOAT_EQ(ram->sdr_white_nits, 203.0f);
  // The damage source paints the output red or blue; frames are BGRA. Black would pass the luma check below either way.
  const auto *pixel = ram->data + 360 * ram->row_pitch + 640 * 4;
  ASSERT_TRUE((pixel[2] > 200 && pixel[0] < 60) || (pixel[0] > 200 && pixel[2] < 60)) << "neither the red nor the blue damage surface";
  const std::array<double, 3> captured {pixel[2] / 255.0, pixel[1] / 255.0, pixel[0] / 255.0};

  const auto colorspace = video::colorspace_from_client_config(config, disp->is_hdr());
  ASSERT_EQ(colorspace.colorspace, video::colorspace_e::bt2020);
  auto device = disp->make_nvenc_encode_device(platf::pix_fmt_e::p010);
  ASSERT_TRUE(device);
  device->colorspace = colorspace;
  ASSERT_TRUE(device->init_encoder(config, colorspace));

  decoder_t decoder {1};
  ASSERT_TRUE(decoder.ctx);
  for (std::uint64_t index = 1; index <= 3; ++index) {
    ASSERT_EQ(device->convert(*frame), 0);
    const auto encoded = device->nvenc->encode_frame(index, index == 1);
    ASSERT_FALSE(encoded.data.empty());
    ASSERT_TRUE(decoder.decode(encoded.data));
  }
  ASSERT_EQ(decoder.frame->format, AV_PIX_FMT_YUV420P10);
  EXPECT_EQ(decoder.frame->color_trc, AVCOL_TRC_SMPTE2084);
  EXPECT_EQ(decoder.frame->color_primaries, AVCOL_PRI_BT2020);

  const auto decoded = reinterpret_cast<const std::uint16_t *>(decoder.frame->data[0] + 360 * decoder.frame->linesize[0])[640];
  const auto expected = test_hdr::expected_y(captured, 203, colorspace) / 64;
  const auto unconverted = test_hdr::expected_y(captured, 10000, colorspace) / 64;
  std::cout << "captured RGB " << (int) pixel[2] << ',' << (int) pixel[1] << ',' << (int) pixel[0] << ": decoded Y " << decoded
            << ", expected " << expected << std::endl;
  // A flat color survives the encode nearly exactly.
  EXPECT_NEAR(decoded, expected, 3) << "unconverted would be about " << unconverted;
}

// Bugs caught: 0, False positives: 0
TEST_F(SystemMemoryCaptureLiveTest, DummyFramesAreBlack) {
  auto output = test_wl::headless_output_t::create("640x480@60");
  ASSERT_TRUE(output) << "Couldn't create a headless output (is this Hyprland?)";
  video::config_t config {};
  config.width = 640;
  config.height = 480;
  config.framerate = 60;
  // The encoder sends a dummy frame before the first capture (and for input-only sessions).
  const std::array<std::shared_ptr<platf::display_t>, 2> displays {
    wl::make_ext_display(platf::mem_type_e::cuda, output->name, config, {.system_memory = true}),
    platf::wl_display(platf::mem_type_e::system, output->name, config),
  };
  for (const auto &disp : displays) {
    ASSERT_TRUE(disp);
    auto img = disp->alloc_img();
    ASSERT_TRUE(img && img->data);
    const auto size = static_cast<std::size_t>(img->row_pitch) * img->height;
    std::memset(img->data, 0xA5, size);
    ASSERT_EQ(disp->dummy_img(img.get()), 0);
    EXPECT_TRUE(std::all_of(img->data, img->data + size, [](std::uint8_t byte) {
      return byte == 0;
    }))
      << "the dummy frame shows stale memory";
  }
}

// Bugs caught: 0, False positives: 1
TEST_F(NvencProbeLiveTest, AdvertisesWhatTheGpuSupports) {
  const auto &caps = nvenc_caps();
  if (!caps.available) {
    GTEST_SKIP() << "No NVENC";
  }
  // On a GPU with NVENC the native encoder must pass. The probe captures the default output: one of the private
  // instance's headless outputs.
  ASSERT_TRUE(video::validate_encoder(video::nvenc, false));
  video::record_encoder_probe_results(video::nvenc);

  const std::array codecs {&video::nvenc.h264, &video::nvenc.hevc, &video::nvenc.av1};
  bool every_codec_invalidates = true;
  for (std::size_t x = 0; x < codecs.size(); ++x) {
    const auto &codec = *codecs[x];
    EXPECT_EQ(codec[video::encoder_t::PASSED], caps.codec[x]) << codec.name;
    if (!caps.codec[x]) {
      continue;
    }
    EXPECT_EQ(codec[video::encoder_t::REF_FRAME_INVALIDATION], caps.invalidation[x]) << codec.name;
    EXPECT_EQ(video::last_encoder_probe_supported_yuv444_for_codec[x], caps.yuv444[x]) << codec.name;
    every_codec_invalidates = every_codec_invalidates && caps.invalidation[x];
  }
  // HDR profiles also need a display that can deliver HDR (display_t::hdr_supported()), so only check the GPU's part.
  if (video::nvenc.hevc[video::encoder_t::DYNAMIC_RANGE]) {
    EXPECT_TRUE(caps.ten_bit[1]);
  }
  // Advertised only when every offered codec can do it; otherwise each request would cost an IDR frame.
  EXPECT_EQ(video::last_encoder_probe_supported_ref_frames_invalidation, every_codec_invalidates);
  video::reset_encoder_probe_results();
}

INSTANTIATE_TEST_SUITE_P(Wayland, NvencLiveTest, testing::ValuesIn(nvenc_cases), [](const auto &info) {
  return std::string {info.param.name};
});

#endif
