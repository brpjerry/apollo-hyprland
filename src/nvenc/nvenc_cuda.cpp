/**
 * @file src/nvenc/nvenc_cuda.cpp
 * @brief Definitions for the Linux NVENC encoder that reads CUDA device memory.
 */
#ifdef __linux__
  // this include
  #include "nvenc_cuda.h"

  // standard includes
  #include <bit>

  // platform includes
  #include <dlfcn.h>

  // local includes
  #include "src/logging.h"

namespace NVENC_NAMESPACE {

  namespace {
    /**
     * @brief Resolve a symbol into a typed function pointer.
     *
     * @tparam Fn Function type of the symbol.
     * @param library Handle returned by `dlopen()`.
     * @param name Symbol name.
     * @return Function pointer, or null when the library does not export the symbol.
     */
    template<typename Fn>
    Fn *load_symbol(void *library, const char *name) {
      return std::bit_cast<Fn *>(dlsym(library, name));
    }

    /**
     * @brief Open the first library of a list that loads.
     *
     * @param names Library file names, most specific first.
     * @return Handle, or null when none of them loads.
     */
    void *open_library(std::initializer_list<const char *> names) {
      for (const auto *name : names) {
        if (auto *handle = dlopen(name, RTLD_LAZY | RTLD_LOCAL)) {
          return handle;
        }
      }
      return nullptr;
    }
  }  // namespace

  nvenc_cuda::nvenc_cuda(const ::nvenc::cuda_input_surface_t &surface):
      nvenc_base(NV_ENC_DEVICE_TYPE_CUDA),
      surface(surface) {
    device = static_cast<CUcontext>(surface.context);
  }

  nvenc_cuda::~nvenc_cuda() {
    if (encoder) {
      destroy_encoder();
    }
    // The function list points into the NVENC library, so drop it before unloading.
    nvenc.reset();
    if (nvenc_library) {
      dlclose(nvenc_library);
    }
    if (cuda_library) {
      dlclose(cuda_library);
    }
  }

  bool nvenc_cuda::create_encoder(
    const ::nvenc::nvenc_config &config,
    const video::config_t &client_config,
    const video::sunshine_colorspace_t &colorspace,
    platf::pix_fmt_e buffer_format
  ) {
    // The context guard needs the CUDA entry points, which init_library() loads.
    if (!nvenc && !init_library()) {
      return false;
    }
    const context_guard_t context {*this};
    if (!context.active()) {
      return false;
    }
    return nvenc_base::create_encoder(config, client_config, colorspace, buffer_format);
  }

  void nvenc_cuda::destroy_encoder() {
    if (!encoder) {
      nvenc_base::destroy_encoder();
      return;
    }
    // Destroy the session even if the context can't be made current: leaking it would hold an encoder slot.
    const context_guard_t context {*this};
    nvenc_base::destroy_encoder();
  }

  ::nvenc::nvenc_encoded_frame nvenc_cuda::encode_frame(uint64_t frame_index, bool force_idr) {
    const context_guard_t context {*this};
    if (!context.active()) {
      return {};
    }
    return nvenc_base::encode_frame(frame_index, force_idr);
  }

  bool nvenc_cuda::invalidate_ref_frames(uint64_t first_frame, uint64_t last_frame) {
    const context_guard_t context {*this};
    if (!context.active()) {
      return false;
    }
    return nvenc_base::invalidate_ref_frames(first_frame, last_frame);
  }

  bool nvenc_cuda::init_library() {
    if (nvenc && cuda.cuCtxPushCurrent) {
      return true;
    }

    if (!cuda_library) {
      cuda_library = open_library({"libcuda.so.1", "libcuda.so"});
      if (!cuda_library) {
        BOOST_LOG(error) << "NvEnc: couldn't load the CUDA driver library: " << dlerror();
        return false;
      }
    }
    cuda_driver_t loaded {
      .cuCtxPushCurrent = load_symbol<tcuCtxPushCurrent_v2>(cuda_library, "cuCtxPushCurrent_v2"),
      .cuCtxPopCurrent = load_symbol<tcuCtxPopCurrent_v2>(cuda_library, "cuCtxPopCurrent_v2"),
      .cuStreamSynchronize = load_symbol<tcuStreamSynchronize>(cuda_library, "cuStreamSynchronize"),
    };
    if (!loaded.cuCtxPushCurrent || !loaded.cuCtxPopCurrent || !loaded.cuStreamSynchronize) {
      BOOST_LOG(error) << "NvEnc: the CUDA driver library lacks context or stream functions";
      return false;
    }

    if (!nvenc_library) {
      nvenc_library = open_library({"libnvidia-encode.so.1", "libnvidia-encode.so"});
      if (!nvenc_library) {
        BOOST_LOG(debug) << "NvEnc: couldn't load the NVENC driver library: " << dlerror();
        return false;
      }
    }
    auto get_max_version = load_symbol<NVENCSTATUS NVENCAPI(std::uint32_t *)>(nvenc_library, "NvEncodeAPIGetMaxSupportedVersion");
    auto create_instance = load_symbol<NVENCSTATUS NVENCAPI(NV_ENCODE_API_FUNCTION_LIST *)>(nvenc_library, "NvEncodeAPICreateInstance");
    if (!get_max_version || !create_instance) {
      BOOST_LOG(error) << "NvEnc: the NVENC driver library lacks NvEncodeAPICreateInstance()";
      return false;
    }

    std::uint32_t max_version = 0;
    if (nvenc_failed(get_max_version(&max_version))) {
      BOOST_LOG(error) << "NvEnc: NvEncodeAPIGetMaxSupportedVersion() failed: " << last_nvenc_error_string;
      return false;
    }
    // The driver packs major.minor as (major << 4) | minor.
    constexpr std::uint32_t required = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
    if (max_version < required) {
      BOOST_LOG(error) << "NvEnc: the driver supports NVENC API " << (max_version >> 4) << '.' << (max_version & 0xF)
                       << ", this build needs " << NVENCAPI_MAJOR_VERSION << '.' << NVENCAPI_MINOR_VERSION
                       << "; update the NVIDIA driver";
      return false;
    }

    auto new_nvenc = std::make_shared<NV_ENCODE_API_FUNCTION_LIST>();
    new_nvenc->version = NV_ENCODE_API_FUNCTION_LIST_VER;
    if (nvenc_failed(create_instance(new_nvenc.get()))) {
      BOOST_LOG(error) << "NvEnc: NvEncodeAPICreateInstance() failed: " << last_nvenc_error_string;
      return false;
    }

    cuda = loaded;
    nvenc = std::move(new_nvenc);
    return true;
  }

  bool nvenc_cuda::create_and_register_input_buffer() {
    if (registered_input_buffer) {
      return true;
    }

    NV_ENC_REGISTER_RESOURCE register_resource = {.version = NV_ENC_REGISTER_RESOURCE_VER};
    register_resource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
    register_resource.width = encoder_params.width;
    register_resource.height = encoder_params.height;
    register_resource.pitch = surface.pitch;
    register_resource.resourceToRegister = std::bit_cast<void *>(static_cast<std::uintptr_t>(surface.device_ptr));
    register_resource.bufferFormat = encoder_params.buffer_format;
    register_resource.bufferUsage = NV_ENC_INPUT_IMAGE;

    if (nvenc_failed(nvenc->nvEncRegisterResource(encoder, &register_resource))) {
      BOOST_LOG(error) << "NvEnc: NvEncRegisterResource() failed: " << last_nvenc_error_string;
      return false;
    }

    registered_input_buffer = register_resource.registeredResource;
    return true;
  }

  bool nvenc_cuda::synchronize_input_buffer() {
    return !cuda_failed(cuda.cuStreamSynchronize(static_cast<CUstream>(surface.stream)), "cuStreamSynchronize()");
  }

  bool nvenc_cuda::cuda_failed(CUresult result, const char *what) const {
    if (result == CUDA_SUCCESS) {
      return false;
    }
    BOOST_LOG(error) << "NvEnc: " << what << " failed: CUDA error " << static_cast<int>(result);
    return true;
  }

  nvenc_cuda::context_guard_t::context_guard_t(const nvenc_cuda &parent):
      parent(parent) {
    if (!parent.cuda.cuCtxPushCurrent) {
      return;
    }
    pushed = !parent.cuda_failed(parent.cuda.cuCtxPushCurrent(static_cast<CUcontext>(parent.surface.context)), "cuCtxPushCurrent()");
  }

  nvenc_cuda::context_guard_t::~context_guard_t() {
    if (pushed) {
      CUcontext previous = nullptr;
      parent.cuda_failed(parent.cuda.cuCtxPopCurrent(&previous), "cuCtxPopCurrent()");
    }
  }

}  // namespace NVENC_NAMESPACE

namespace nvenc {

  std::unique_ptr<nvenc_encoder> create_nvenc_cuda(const cuda_input_surface_t &surface) {
    if (!surface.context || !surface.device_ptr || !surface.pitch) {
      BOOST_LOG(error) << "NvEnc: the CUDA input surface has no context or memory";
      return {};
    }
    return std::make_unique<NVENC_NAMESPACE::nvenc_cuda>(surface);
  }

}  // namespace nvenc
#endif
