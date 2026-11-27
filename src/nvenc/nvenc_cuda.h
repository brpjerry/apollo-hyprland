/**
 * @file src/nvenc/nvenc_cuda.h
 * @brief Declarations for the Linux NVENC encoder that reads CUDA device memory.
 */
#pragma once
#ifdef __linux__

  // local includes
  #include "nvenc_base.h"
  #include "nvenc_cuda_interface.h"

namespace NVENC_NAMESPACE {

  /**
   * @brief CUDA driver entry points used by the encoder.
   */
  struct cuda_driver_t {
    tcuCtxPushCurrent_v2 *cuCtxPushCurrent = nullptr;  ///< Make the surface's context current on this thread.
    tcuCtxPopCurrent_v2 *cuCtxPopCurrent = nullptr;  ///< Restore the previous context.
    tcuStreamSynchronize *cuStreamSynchronize = nullptr;  ///< Wait for the writes to the input surface.
  };

  /**
   * @brief NVENC encoder that reads its input from CUDA device memory (Linux).
   *
   * The caller converts each captured frame into the surface on its CUDA stream; the encoder waits for that stream
   * before it encodes. Every NVENC call runs with the surface's context current, as the driver expects for CUDA
   * sessions.
   */
  class nvenc_cuda: public nvenc_base {
  public:
    /**
     * @brief Create an encoder for a CUDA surface.
     *
     * @param surface Surface the encoder reads every frame from.
     */
    explicit nvenc_cuda(const ::nvenc::cuda_input_surface_t &surface);

    /**
     * @brief Destroy the encoder session, then unload the driver libraries.
     */
    ~nvenc_cuda() override;

    nvenc_cuda(const nvenc_cuda &) = delete;
    nvenc_cuda &operator=(const nvenc_cuda &) = delete;

    /**
     * @brief Create the encoder with the surface's context current.
     *
     * @param config NVENC encoder configuration.
     * @param client_config Stream configuration requested by the client.
     * @param colorspace YUV colorspace.
     * @param buffer_format Format of the surface.
     * @return `true` on success, `false` on error.
     */
    bool create_encoder(
      const ::nvenc::nvenc_config &config,
      const video::config_t &client_config,
      const video::sunshine_colorspace_t &colorspace,
      platf::pix_fmt_e buffer_format
    ) override;

    /**
     * @brief Destroy the encoder session with the surface's context current.
     */
    void destroy_encoder() override;

    /**
     * @brief Encode the surface with its context current.
     *
     * @param frame_index Frame index that uniquely identifies the frame.
     * @param force_idr Whether to encode the frame as a forced IDR.
     * @return Encoded frame, empty on error.
     */
    ::nvenc::nvenc_encoded_frame encode_frame(uint64_t frame_index, bool force_idr) override;

    /**
     * @brief Invalidate reference frames with the surface's context current.
     *
     * @param first_frame First frame index of the invalidation range.
     * @param last_frame Last frame index of the invalidation range.
     * @return `true` on success, `false` when the next frame must be an IDR frame.
     */
    bool invalidate_ref_frames(uint64_t first_frame, uint64_t last_frame) override;

  protected:
    /**
     * @brief Load the NVENC and CUDA driver libraries and fill `nvenc` and `cuda`.
     *
     * @return `true` on success, `false` on error.
     */
    bool init_library() override;

    /**
     * @brief Register the CUDA surface with the encoder session.
     *
     * @return `true` on success, `false` on error.
     */
    bool create_and_register_input_buffer() override;

    /**
     * @brief Wait until the surface's stream has finished writing the frame.
     *
     * @return `true` on success, `false` on error.
     */
    bool synchronize_input_buffer() override;

    /**
     * @brief Check a CUDA status and log it when it is an error.
     *
     * @param result CUDA driver status.
     * @param what Call that returned it.
     * @return `true` when the status is an error.
     */
    bool cuda_failed(CUresult result, const char *what) const;

    /**
     * @brief Context made current for the lifetime of the guard.
     */
    class context_guard_t {
    public:
      /**
       * @brief Push the encoder's context; check `active()` for the result.
       *
       * @param parent Encoder whose context is pushed.
       */
      explicit context_guard_t(const nvenc_cuda &parent);

      /**
       * @brief Pop the context pushed by the constructor.
       */
      ~context_guard_t();

      context_guard_t(const context_guard_t &) = delete;
      context_guard_t &operator=(const context_guard_t &) = delete;

      /**
       * @brief Report whether the context was pushed.
       *
       * @return `true` when the context is current.
       */
      bool active() const {
        return pushed;
      }

    private:
      const nvenc_cuda &parent;
      bool pushed = false;
    };

    ::nvenc::cuda_input_surface_t surface;  ///< Surface the encoder reads.
    cuda_driver_t cuda;  ///< CUDA entry points, filled by `init_library()`.

  private:
    void *nvenc_library = nullptr;
    void *cuda_library = nullptr;
  };

}  // namespace NVENC_NAMESPACE
#endif
