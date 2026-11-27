/**
 * @file tests/unit/test_nvenc_cuda.cpp
 * @brief Tests for the Linux NVENC encoder that reads CUDA device memory, against a fake driver.
 */

#ifdef __linux__
  // test includes
  #include "../tests_common.h"

  // standard includes
  #include <algorithm>
  #include <array>
  #include <bit>
  #include <cstdint>
  #include <vector>

  // local includes
  #include "src/nvenc/nvenc_cuda.h"
  #include "src/video_colorspace.h"

namespace {
  using namespace nvenc_1301;

  /**
   * @brief What the fake NVENC and CUDA drivers were asked to do.
   */
  struct fake_driver_t {
    CUresult push_status = CUDA_SUCCESS;  ///< Status cuCtxPushCurrent() returns.
    CUresult sync_status = CUDA_SUCCESS;  ///< Status cuStreamSynchronize() returns.
    int context_depth = 0;  ///< Contexts pushed and not popped yet.
    int max_context_depth = 0;  ///< Deepest nesting seen.
    std::vector<CUcontext> pushed;  ///< Every context pushed.
    std::vector<CUstream> synchronized;  ///< Every stream synchronized.
    int opens = 0;  ///< Sessions opened.
    int destroys = 0;  ///< Sessions destroyed.
    int driver_calls_outside_context = 0;  ///< NVENC calls made without a current context.
    NV_ENC_REGISTER_RESOURCE registered {};  ///< Last resource registration.
    int unregisters = 0;  ///< Resources unregistered.
    std::uint32_t intra_refresh = 0;  ///< enableIntraRefresh of the last HEVC initialization.
    std::uint64_t last_timestamp = 0;  ///< inputTimeStamp of the last encoded picture.
    std::uint32_t last_pic_flags = 0;  ///< encodePicFlags of the last encoded picture.
    std::array<std::uint8_t, 4> bitstream {0, 0, 1, 0x40};  ///< Bytes every encoded frame returns.
  };

  fake_driver_t fake;  ///< Fake driver state; reset by each test.

  /**
   * @brief Record an NVENC call and whether a context was current for it.
   */
  void note_driver_call() {
    if (fake.context_depth == 0) {
      ++fake.driver_calls_outside_context;
    }
  }

  /**
   * @brief Fill an NVENC function list that encodes HEVC 4:2:0 and 4:4:4 up to 4096x4096.
   *
   * @param list Function list to fill.
   */
  void install_fake_nvenc(NV_ENCODE_API_FUNCTION_LIST &list) {
    list.nvEncOpenEncodeSessionEx = [](NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS *, void **encoder) {
      note_driver_call();
      ++fake.opens;
      *encoder = &fake;
      return NV_ENC_SUCCESS;
    };
    list.nvEncGetEncodeGUIDCount = [](void *, std::uint32_t *count) {
      *count = 1;
      return NV_ENC_SUCCESS;
    };
    list.nvEncGetEncodeGUIDs = [](void *, GUID *guids, std::uint32_t, std::uint32_t *count) {
      *guids = NV_ENC_CODEC_HEVC_GUID;
      *count = 1;
      return NV_ENC_SUCCESS;
    };
    list.nvEncGetEncodeCaps = [](void *, GUID, NV_ENC_CAPS_PARAM *params, int *value) {
      switch (params->capsToQuery) {
        case NV_ENC_CAPS_WIDTH_MAX:
        case NV_ENC_CAPS_HEIGHT_MAX:
          *value = 4096;
          break;
        case NV_ENC_CAPS_SUPPORT_INTRA_REFRESH:
        case NV_ENC_CAPS_SUPPORT_REF_PIC_INVALIDATION:
        case NV_ENC_CAPS_SUPPORT_MULTIPLE_REF_FRAMES:
        case NV_ENC_CAPS_SUPPORT_YUV444_ENCODE:
          *value = 1;
          break;
        default:
          *value = 0;
      }
      return NV_ENC_SUCCESS;
    };
    list.nvEncGetEncodePresetConfigEx = [](void *, GUID, GUID, NV_ENC_TUNING_INFO, NV_ENC_PRESET_CONFIG *) {
      return NV_ENC_SUCCESS;
    };
    list.nvEncInitializeEncoder = [](void *, NV_ENC_INITIALIZE_PARAMS *params) {
      note_driver_call();
      fake.intra_refresh = params->encodeConfig->encodeCodecConfig.hevcConfig.enableIntraRefresh;
      return NV_ENC_SUCCESS;
    };
    list.nvEncCreateBitstreamBuffer = [](void *, NV_ENC_CREATE_BITSTREAM_BUFFER *params) {
      params->bitstreamBuffer = &fake.bitstream;
      return NV_ENC_SUCCESS;
    };
    list.nvEncRegisterResource = [](void *, NV_ENC_REGISTER_RESOURCE *params) {
      note_driver_call();
      fake.registered = *params;
      params->registeredResource = &fake.registered;
      return NV_ENC_SUCCESS;
    };
    list.nvEncMapInputResource = [](void *, NV_ENC_MAP_INPUT_RESOURCE *params) {
      note_driver_call();
      params->mappedResource = params->registeredResource;
      params->mappedBufferFmt = fake.registered.bufferFormat;
      return NV_ENC_SUCCESS;
    };
    list.nvEncUnmapInputResource = [](void *, NV_ENC_INPUT_PTR) {
      return NV_ENC_SUCCESS;
    };
    list.nvEncEncodePicture = [](void *, NV_ENC_PIC_PARAMS *params) {
      note_driver_call();
      fake.last_timestamp = params->inputTimeStamp;
      fake.last_pic_flags = params->encodePicFlags;
      return NV_ENC_SUCCESS;
    };
    list.nvEncLockBitstream = [](void *, NV_ENC_LOCK_BITSTREAM *params) {
      note_driver_call();
      params->bitstreamBufferPtr = fake.bitstream.data();
      params->bitstreamSizeInBytes = fake.bitstream.size();
      params->outputTimeStamp = fake.last_timestamp;
      params->pictureType = (fake.last_pic_flags & NV_ENC_PIC_FLAG_FORCEIDR) ? NV_ENC_PIC_TYPE_IDR : NV_ENC_PIC_TYPE_P;
      return NV_ENC_SUCCESS;
    };
    list.nvEncUnlockBitstream = [](void *, NV_ENC_OUTPUT_PTR) {
      return NV_ENC_SUCCESS;
    };
    list.nvEncInvalidateRefFrames = [](void *, std::uint64_t) {
      note_driver_call();
      return NV_ENC_SUCCESS;
    };
    list.nvEncUnregisterResource = [](void *, NV_ENC_REGISTERED_PTR) {
      note_driver_call();
      ++fake.unregisters;
      return NV_ENC_SUCCESS;
    };
    list.nvEncDestroyBitstreamBuffer = [](void *, NV_ENC_OUTPUT_PTR) {
      return NV_ENC_SUCCESS;
    };
    list.nvEncDestroyEncoder = [](void *) {
      note_driver_call();
      ++fake.destroys;
      return NV_ENC_SUCCESS;
    };
  }

  /**
   * @brief CUDA encoder whose libraries are the fake drivers.
   */
  class fake_nvenc_cuda_t: public nvenc_cuda {
  public:
    using nvenc_cuda::nvenc_cuda;

  protected:
    bool init_library() override {
      auto list = std::make_shared<NV_ENCODE_API_FUNCTION_LIST>();
      install_fake_nvenc(*list);
      nvenc = list;
      cuda.cuCtxPushCurrent = [](CUcontext context) {
        if (fake.push_status != CUDA_SUCCESS) {
          return fake.push_status;
        }
        fake.pushed.push_back(context);
        fake.max_context_depth = std::max(fake.max_context_depth, ++fake.context_depth);
        return CUDA_SUCCESS;
      };
      cuda.cuCtxPopCurrent = [](CUcontext *) {
        --fake.context_depth;
        return CUDA_SUCCESS;
      };
      cuda.cuStreamSynchronize = [](CUstream stream) {
        fake.synchronized.push_back(stream);
        return fake.sync_status;
      };
      return true;
    }
  };

  constexpr std::uintptr_t context_address = 0x1000;  ///< Fake CUcontext.
  constexpr std::uintptr_t stream_address = 0x2000;  ///< Fake CUstream.
  constexpr std::uint64_t surface_address = 0x7f0000000000;  ///< Fake CUdeviceptr.

  /**
   * @brief Surface the tests encode from.
   *
   * @return Surface with fake context, stream and memory.
   */
  ::nvenc::cuda_input_surface_t test_surface() {
    return {
      .context = std::bit_cast<void *>(context_address),
      .stream = std::bit_cast<void *>(stream_address),
      .device_ptr = surface_address,
      .pitch = 2048,
    };
  }

  /**
   * @brief HEVC stream configuration for a 1920x1080 client.
   *
   * @return Client configuration.
   */
  video::config_t hevc_client() {
    video::config_t config {};
    config.width = 1920;
    config.height = 1080;
    config.framerate = 60;
    config.bitrate = 20000;
    config.videoFormat = 1;
    config.numRefFrames = 0;
    return config;
  }

  /**
   * @brief Reset the fake drivers before each test.
   */
  struct NvencCudaTest: testing::Test {
    void SetUp() override {
      fake = {};
    }
  };
}  // namespace

// Bugs caught: 0, False positives: 0
TEST_F(NvencCudaTest, RegistersTheCudaSurfaceAndCallsTheDriverWithItsContextCurrent) {
  const auto client = hevc_client();
  {
    fake_nvenc_cuda_t encoder {test_surface()};
    ASSERT_TRUE(encoder.create_encoder({}, client, video::colorspace_from_client_config(client, false), platf::pix_fmt_e::nv12));

    EXPECT_EQ(fake.registered.resourceType, NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR);
    EXPECT_EQ(std::bit_cast<std::uintptr_t>(fake.registered.resourceToRegister), surface_address);
    EXPECT_EQ(fake.registered.pitch, 2048u);
    EXPECT_EQ(fake.registered.width, 1920u);
    EXPECT_EQ(fake.registered.height, 1080u);
    EXPECT_EQ(fake.registered.bufferFormat, NV_ENC_BUFFER_FORMAT_NV12);
    EXPECT_EQ(fake.registered.bufferUsage, NV_ENC_INPUT_IMAGE);

    const auto frame = encoder.encode_frame(7, true);
    EXPECT_EQ(frame.data.size(), fake.bitstream.size());
    EXPECT_EQ(frame.frame_index, 7u);
    EXPECT_TRUE(frame.idr);
    // The conversion runs on the surface's stream; the encoder waits for it before reading the frame.
    ASSERT_EQ(fake.synchronized.size(), 1u);
    EXPECT_EQ(std::bit_cast<std::uintptr_t>(fake.synchronized[0]), stream_address);
  }

  EXPECT_EQ(fake.opens, 1);
  EXPECT_EQ(fake.destroys, 1);
  EXPECT_EQ(fake.unregisters, 1);
  EXPECT_EQ(fake.driver_calls_outside_context, 0);
  EXPECT_EQ(fake.context_depth, 0);
  ASSERT_FALSE(fake.pushed.empty());
  for (auto *context : fake.pushed) {
    EXPECT_EQ(std::bit_cast<std::uintptr_t>(context), context_address);
  }
}

// Bugs caught: 0, False positives: 0
TEST_F(NvencCudaTest, RegistersPlanarYuv444Surfaces) {
  const auto client = hevc_client();
  fake_nvenc_cuda_t encoder {test_surface()};
  ASSERT_TRUE(encoder.create_encoder({}, client, video::colorspace_from_client_config(client, false), platf::pix_fmt_e::yuv444p));
  EXPECT_EQ(fake.registered.bufferFormat, NV_ENC_BUFFER_FORMAT_YUV444);
}

// Bugs caught: 0, False positives: 0
TEST_F(NvencCudaTest, InvalidatesReferenceFramesInsteadOfRequestingAnIdrFrame) {
  auto client = hevc_client();
  client.numRefFrames = 0;
  fake_nvenc_cuda_t encoder {test_surface()};
  ASSERT_TRUE(encoder.create_encoder({}, client, video::colorspace_from_client_config(client, false), platf::pix_fmt_e::nv12));
  for (std::uint64_t frame = 1; frame <= 3; ++frame) {
    ASSERT_FALSE(encoder.encode_frame(frame, frame == 1).data.empty());
  }

  EXPECT_TRUE(encoder.invalidate_ref_frames(3, 3));
  const auto recovered = encoder.encode_frame(4, false);
  EXPECT_FALSE(recovered.idr);
  EXPECT_TRUE(recovered.after_ref_frame_invalidation);
  EXPECT_EQ(fake.driver_calls_outside_context, 0);
}

// Bugs caught: 0, False positives: 0
TEST_F(NvencCudaTest, IntraRefreshFollowsTheClientOrTheServerSetting) {
  struct case_t {
    int client_asks;
    bool server_forces;
    std::uint32_t expected;
  };

  for (const auto &[client_asks, server_forces, expected] : std::to_array<case_t>({{0, false, 0}, {1, false, 1}, {0, true, 1}})) {
    fake = {};
    auto client = hevc_client();
    client.enableIntraRefresh = client_asks;
    ::nvenc::nvenc_config config {};
    config.intra_refresh = server_forces;
    fake_nvenc_cuda_t encoder {test_surface()};
    ASSERT_TRUE(encoder.create_encoder(config, client, video::colorspace_from_client_config(client, false), platf::pix_fmt_e::nv12));
    EXPECT_EQ(fake.intra_refresh, expected) << "client " << client_asks << ", server " << server_forces;
  }
}

// Bugs caught: 0, False positives: 0
TEST_F(NvencCudaTest, DoesNotOpenASessionWhenTheContextCannotBeMadeCurrent) {
  fake.push_status = CUDA_ERROR_INVALID_VALUE;
  const auto client = hevc_client();
  fake_nvenc_cuda_t encoder {test_surface()};
  EXPECT_FALSE(encoder.create_encoder({}, client, video::colorspace_from_client_config(client, false), platf::pix_fmt_e::nv12));
  EXPECT_EQ(fake.opens, 0);
}

// Bugs caught: 0, False positives: 0
TEST_F(NvencCudaTest, ReturnsNoFrameWhenTheConversionStreamFails) {
  const auto client = hevc_client();
  fake_nvenc_cuda_t encoder {test_surface()};
  ASSERT_TRUE(encoder.create_encoder({}, client, video::colorspace_from_client_config(client, false), platf::pix_fmt_e::nv12));
  fake.sync_status = CUDA_ERROR_NOT_READY;
  EXPECT_TRUE(encoder.encode_frame(1, true).data.empty());
  EXPECT_EQ(fake.context_depth, 0);
}

// Bugs caught: 0, False positives: 0
TEST(NvencCudaFactoryTest, RejectsSurfacesWithoutContextOrMemory) {
  EXPECT_FALSE(::nvenc::create_nvenc_cuda({}));
  auto surface = test_surface();
  surface.device_ptr = 0;
  EXPECT_FALSE(::nvenc::create_nvenc_cuda(surface));
  surface = test_surface();
  surface.pitch = 0;
  EXPECT_FALSE(::nvenc::create_nvenc_cuda(surface));
  EXPECT_TRUE(::nvenc::create_nvenc_cuda(test_surface()));
}
#endif
