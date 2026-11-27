/**
 * @file src/nvenc/nvenc_cuda_interface.h
 * @brief Declarations for the SDK-neutral Linux NVENC encoder that reads CUDA device memory.
 */
#pragma once
#ifdef __linux__

  // standard includes
  #include <cstdint>
  #include <memory>

  // local includes
  #include "nvenc_encoder.h"

namespace nvenc {

  /**
   * @brief CUDA device memory that holds the frame to encode.
   *
   * The planes are laid out the way NVENC reads a `CUDADEVICEPTR` input: each plane follows the previous one at
   * `pitch * height` bytes and every plane has the same pitch (NV12/P010: luma, then interleaved chroma; YUV 4:4:4:
   * three full-size planes). FFmpeg's CUDA frame pool allocates frames this way.
   */
  struct cuda_input_surface_t {
    void *context = nullptr;  ///< `CUcontext` that owns the memory; the encoder session runs on it.
    void *stream = nullptr;  ///< `CUstream` that writes the surface; it is synchronized before each frame is encoded.
    std::uint64_t device_ptr = 0;  ///< `CUdeviceptr` of the first plane.
    std::uint32_t pitch = 0;  ///< Bytes between the starts of two rows, in every plane.
  };

  /**
   * @brief Create an NVENC encoder that encodes the given CUDA surface.
   *
   * The NVENC and CUDA driver libraries are loaded when `create_encoder()` is called, so this only fails on bad input.
   *
   * @param surface Surface the encoder reads every frame from. It must stay allocated while the encoder exists.
   * @return SDK-neutral encoder, or an empty pointer when `surface` has no context or memory.
   */
  std::unique_ptr<nvenc_encoder> create_nvenc_cuda(const cuda_input_surface_t &surface);

}  // namespace nvenc
#endif
