/**
 * @file src/platform/linux/wayland_geometry.h
 * @brief Pure geometry helpers for Wayland outputs (no Wayland dependency, so they can be unit tested anywhere).
 */
#pragma once

// standard includes
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace wl {
  /**
   * @brief Geometry of one wl_output as announced by the compositor.
   */
  struct output_geometry_t {
    /// Top-left of the output in the compositor's global layout space, in logical pixels
    /// (zxdg_output_v1.logical_position). This is the space Wayland pointer/touch positions and Hyprland
    /// monitor positions use. It can be negative.
    int logical_x {0};
    int logical_y {0};

    /// Size of the output in logical pixels, after transform and scale (zxdg_output_v1.logical_size).
    /// 0 until the compositor has sent it.
    int logical_width {0};
    int logical_height {0};

    /// Current mode in hardware pixels, before the output transform is applied (wl_output.mode).
    int mode_width {0};
    int mode_height {0};

    /// wl_output_transform (0 = normal, 1 = 90, 2 = 180, 3 = 270, 4..7 = flipped variants).
    int transform {0};

    /// wl_output.scale: integer buffer scale (fractional scales are rounded up by the compositor).
    int integer_scale {1};
  };

  struct extent_t {
    int width;
    int height;

    friend bool operator==(const extent_t &, const extent_t &) = default;
  };

  /**
   * @brief Bounding box of all outputs in the global layout space, in logical pixels.
   */
  struct layout_bounds_t {
    int min_x;  ///< Left edge of the left-most output (can be negative).
    int min_y;  ///< Top edge of the top-most output (can be negative).
    int width;  ///< max(logical_x + logical_width) - min_x.
    int height;  ///< max(logical_y + logical_height) - min_y.

    friend bool operator==(const layout_bounds_t &, const layout_bounds_t &) = default;
  };

  /**
   * @brief Whether a wl_output_transform rotates by 90 or 270 degrees (swapping width and height).
   */
  constexpr bool transform_swaps_axes(int transform) {
    return (transform & 1) != 0;
  }

  /**
   * @brief Size of the current mode after the output transform, in hardware pixels.
   */
  constexpr extent_t transformed_mode_size(const output_geometry_t &geometry) {
    if (transform_swaps_axes(geometry.transform)) {
      return {geometry.mode_height, geometry.mode_width};
    }
    return {geometry.mode_width, geometry.mode_height};
  }

  /**
   * @brief Logical size of an output.
   * @details Uses the xdg-output logical size when known; otherwise derives it from the transformed mode and the
   *          integer scale (exact for integer scales, which is all a compositor without xdg-output can express).
   */
  constexpr extent_t logical_size(const output_geometry_t &geometry) {
    if (geometry.logical_width > 0 && geometry.logical_height > 0) {
      return {geometry.logical_width, geometry.logical_height};
    }

    auto size = transformed_mode_size(geometry);
    const int scale = std::max(1, geometry.integer_scale);
    return {size.width / scale, size.height / scale};
  }

  /**
   * @brief Effective (possibly fractional) scale: hardware pixels per logical pixel.
   * @return 1.0 if the geometry is not known yet.
   */
  inline double effective_scale(const output_geometry_t &geometry) {
    const auto logical = logical_size(geometry);
    const auto hw = transformed_mode_size(geometry);
    if (logical.width <= 0 || hw.width <= 0) {
      return 1.0;
    }
    return static_cast<double>(hw.width) / logical.width;
  }

  /**
   * @brief Bounding box of the logical rectangles of all outputs.
   * @details Outputs without a known logical size are skipped. Returns all zeros if no output qualifies.
   */
  inline layout_bounds_t layout_bounds(const std::vector<output_geometry_t> &outputs) {
    int min_x = std::numeric_limits<int>::max();
    int min_y = std::numeric_limits<int>::max();
    int max_x = std::numeric_limits<int>::min();
    int max_y = std::numeric_limits<int>::min();
    bool any = false;

    for (const auto &output : outputs) {
      const auto size = logical_size(output);
      if (size.width <= 0 || size.height <= 0) {
        continue;
      }
      any = true;
      min_x = std::min(min_x, output.logical_x);
      min_y = std::min(min_y, output.logical_y);
      max_x = std::max(max_x, output.logical_x + size.width);
      max_y = std::max(max_y, output.logical_y + size.height);
    }

    if (!any) {
      return {0, 0, 0, 0};
    }
    return {min_x, min_y, max_x - min_x, max_y - min_y};
  }
}  // namespace wl
