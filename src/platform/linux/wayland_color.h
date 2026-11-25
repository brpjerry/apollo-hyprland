/**
 * @file src/platform/linux/wayland_color.h
 * @brief Pure helpers for wp-color-management-v1 image descriptions: HDR detection and HDR metadata.
 * @details No Wayland dependency, so everything here can be unit tested without a compositor.
 *          The values are in the units the protocol uses (wp_image_description_info_v1 events).
 */
#pragma once

// standard includes
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>

// lib includes
#include <moonlight-common-c/src/Limelight.h>

namespace wl {
  /// wp_color_manager_v1 enum values used here (same values as color-management-v1.xml).
  namespace color {
    namespace tf {
      constexpr std::uint32_t gamma22 = 2;
      constexpr std::uint32_t ext_linear = 5;
      constexpr std::uint32_t srgb = 9;
      constexpr std::uint32_t ext_srgb = 10;
      constexpr std::uint32_t st2084_pq = 11;
      constexpr std::uint32_t hlg = 13;
    }  // namespace tf

    namespace primaries {
      constexpr std::uint32_t srgb = 1;
      constexpr std::uint32_t bt2020 = 6;
    }  // namespace primaries
  }  // namespace color

  /**
   * @brief CIE 1931 xy chromaticities of red, green, blue and white, each coordinate times 1'000'000
   *        (the protocol's encoding).
   */
  struct chromaticities_t {
    std::array<std::int32_t, 8> xy {};  ///< r_x, r_y, g_x, g_y, b_x, b_y, w_x, w_y

    friend bool operator==(const chromaticities_t &, const chromaticities_t &) = default;
  };

  /// BT.2020 primaries with a D65 white point.
  constexpr chromaticities_t bt2020_chromaticities {{708000, 292000, 170000, 797000, 131000, 46000, 312700, 329000}};

  /**
   * @brief What wp_image_description_info_v1 reported about an image description (one batch, up to `done`).
   */
  struct image_description_t {
    std::optional<std::uint32_t> tf_named;  ///< wp_color_manager_v1.transfer_function
    std::optional<std::uint32_t> tf_power;  ///< exponent * 10000
    std::optional<std::uint32_t> primaries_named;  ///< wp_color_manager_v1.primaries
    std::optional<chromaticities_t> primaries;
    bool icc {false};  ///< The description is an ICC profile (no parametric data).

    /// Primary color volume: min * 10000, max and reference white in cd/m².
    std::optional<std::uint32_t> min_lum, max_lum, reference_lum;

    /// Target color volume (the mastering display), if it differs from the primary one.
    std::optional<chromaticities_t> target_primaries;
    std::optional<std::uint32_t> target_min_lum, target_max_lum;  ///< min * 10000, max in cd/m²
    std::optional<std::uint32_t> target_max_cll, target_max_fall;  ///< cd/m²

    friend bool operator==(const image_description_t &, const image_description_t &) = default;
  };

  /**
   * @brief Whether a set of chromaticities is BT.2020 (within 0.001 per coordinate, to tolerate rounding).
   */
  inline bool is_bt2020(const chromaticities_t &c) {
    for (std::size_t x = 0; x < c.xy.size(); ++x) {
      if (std::abs(c.xy[x] - bt2020_chromaticities.xy[x]) > 1000) {
        return false;
      }
    }
    return true;
  }

  /**
   * @brief Whether an image description is HDR10: SMPTE ST 2084 (PQ) transfer function and BT.2020 primaries.
   * @details Hyprland 0.57 sends both the named primaries and their chromaticities; either is accepted.
   */
  inline bool is_hdr10(const image_description_t &desc) {
    if (desc.tf_named != color::tf::st2084_pq) {
      return false;
    }
    if (desc.primaries_named) {
      return *desc.primaries_named == color::primaries::bt2020;
    }
    return desc.primaries && is_bt2020(*desc.primaries);
  }

  namespace detail {
    inline std::uint16_t clamp_u16(std::int64_t value) {
      return (std::uint16_t) std::clamp<std::int64_t>(value, 0, 0xFFFF);
    }

    /// Protocol chromaticity (x 1'000'000) to SS_HDR_METADATA (x 50'000), rounded.
    inline std::uint16_t to_50k(std::int32_t value) {
      return clamp_u16(((std::int64_t) value + 10) / 20);
    }
  }  // namespace detail

  /**
   * @brief Fills Moonlight's HDR metadata (SMPTE ST 2086 mastering display plus content light levels) from an
   *        image description.
   * @details The mastering display is the target color volume when the compositor sent one, otherwise the primary
   *          color volume. Max CLL and max FALL are only filled when the compositor reported them.
   * @return false (and zeroed metadata) if the description is not HDR10 (see is_hdr10()).
   */
  inline bool fill_hdr_metadata(const image_description_t &desc, SS_HDR_METADATA &metadata) {
    std::memset(&metadata, 0, sizeof(metadata));
    if (!is_hdr10(desc)) {
      return false;
    }

    const auto &primaries = desc.target_primaries ? *desc.target_primaries : desc.primaries.value_or(bt2020_chromaticities);
    for (int i = 0; i < 3; ++i) {
      metadata.displayPrimaries[i].x = detail::to_50k(primaries.xy[i * 2]);
      metadata.displayPrimaries[i].y = detail::to_50k(primaries.xy[i * 2 + 1]);
    }
    metadata.whitePoint.x = detail::to_50k(primaries.xy[6]);
    metadata.whitePoint.y = detail::to_50k(primaries.xy[7]);

    // Both use cd/m² for the maximum; the protocol's minimum (x 10000) is already in SS_HDR_METADATA's 1/10000 cd/m².
    const auto max_lum = desc.target_max_lum ? desc.target_max_lum : desc.max_lum;
    const auto min_lum = desc.target_max_lum ? desc.target_min_lum : desc.min_lum;
    metadata.maxDisplayLuminance = detail::clamp_u16(max_lum.value_or(0));
    metadata.minDisplayLuminance = detail::clamp_u16(min_lum.value_or(0));
    metadata.maxContentLightLevel = detail::clamp_u16(desc.target_max_cll.value_or(0));
    metadata.maxFrameAverageLightLevel = detail::clamp_u16(desc.target_max_fall.value_or(0));
    return true;
  }
}  // namespace wl
