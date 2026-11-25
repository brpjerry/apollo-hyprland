/**
 * @file scripts/hyprland/headless-drm-shim.cpp
 * @brief LD_PRELOAD shim that lets Hyprland 0.57 run on aquamarine's headless backend alone.
 * @details Hyprland always creates the headless backend, but aquamarine only builds its buffer allocator from a
 *          backend that has a DRM fd, and the headless backend has none. Without a DRM session (which would take over
 *          real GPU outputs and input devices) or a parent Wayland compositor, Hyprland therefore exits with
 *          "Cannot open backend: no allocator available" (Hyprland's own source has "TODO: headless only").
 *
 *          This shim overrides three virtual methods of Aquamarine::CHeadlessBackend:
 *          - drmFD() / drmRenderNodeFD() return an fd for $APOLLO_HYPRLAND_DRM_NODE (a render node, or a vkms card
 *            node on machines without a GPU), so the allocator and the renderer use that device.
 *          - getRenderFormats() advertises the usual 8-bit formats with the modifier the GPU picks for a
 *            rendering + scanout buffer. Aquamarine 0.16's fallback list uses DRM_FORMAT_INVALID (0, i.e.
 *            DRM_FORMAT_MOD_LINEAR) where it means DRM_FORMAT_MOD_INVALID; NVIDIA cannot render to linear buffers,
 *            and cannot import implicit-modifier buffers into EGL either, so the modifier has to be explicit.
 *
 *          Built and loaded by scripts/hyprland/run-headless-hyprland.sh. Not part of Apollo.
 */
#include <aquamarine/backend/Headless.hpp>
#include <cstdio>
#include <cstdlib>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <unistd.h>

namespace {
  int node_fd() {
    static const int fd = [] {
      const char *node = std::getenv("APOLLO_HYPRLAND_DRM_NODE");
      if (!node || !*node) {
        std::fprintf(stderr, "headless-drm-shim: APOLLO_HYPRLAND_DRM_NODE is not set\n");
        return -1;
      }
      const int fd = open(node, O_RDWR | O_CLOEXEC);
      if (fd < 0) {
        std::perror("headless-drm-shim: open");
      } else {
        std::fprintf(stderr, "headless-drm-shim: headless backend uses %s\n", node);
      }
      return fd;
    }();
    return fd;
  }
}  // namespace

int Aquamarine::CHeadlessBackend::drmFD() {
  return node_fd();
}

int Aquamarine::CHeadlessBackend::drmRenderNodeFD() {
  return node_fd();
}

std::vector<Aquamarine::SDRMFormat> Aquamarine::CHeadlessBackend::getRenderFormats() {
  static const std::vector<SDRMFormat> formats = [] {
    std::vector<SDRMFormat> result;
    const int fd = node_fd() >= 0 ? dup(node_fd()) : -1;
    gbm_device *gbm = fd >= 0 ? gbm_create_device(fd) : nullptr;
    for (uint32_t format : {DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888, DRM_FORMAT_XBGR8888, DRM_FORMAT_ABGR8888}) {
      // Ask the driver which modifier it uses for an output buffer, the way aquamarine allocates them.
      uint64_t modifier = DRM_FORMAT_MOD_INVALID;
      for (uint32_t usage : {uint32_t {GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT}, uint32_t {GBM_BO_USE_RENDERING}}) {
        if (gbm_bo *bo = gbm ? gbm_bo_create(gbm, 64, 64, format, usage) : nullptr) {
          modifier = gbm_bo_get_modifier(bo);
          gbm_bo_destroy(bo);
          break;
        }
      }
      result.push_back({.drmFormat = format, .modifiers = {modifier}});
    }
    if (gbm) {
      gbm_device_destroy(gbm);
    }
    if (fd >= 0) {
      close(fd);
    }
    return result;
  }();
  return formats;
}
