/**
 * @file tests/unit/platform/pq_reference.h
 * @brief Reference computation of the luma the GL converter writes for SDR content in an HDR (PQ) stream.
 */
#pragma once

// standard includes
#include <algorithm>
#include <array>
#include <cmath>

// local includes
#include <src/video_colorspace.h>

namespace test_hdr {

  /**
   * @brief sRGB EOTF.
   *
   * @param c Encoded component, 0 to 1.
   * @return Linear light, 0 to 1.
   */
  inline double srgb_to_linear(double c) {
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
  }

  /**
   * @brief SMPTE ST 2084 inverse EOTF.
   *
   * @param l Luminance in units of 10000 cd/m².
   * @return PQ signal, 0 to 1.
   */
  inline double pq_encode(double l) {
    const double m1 = 0.1593017578125;
    const double m2 = 78.84375;
    const double c1 = 0.8359375;
    const double c2 = 18.8515625;
    const double c3 = 18.6875;
    const double p = std::pow(std::clamp(l, 0.0, 1.0), m1);
    return std::pow((c1 + c2 * p) / (1 + c3 * p), m2);
  }

  /**
   * @brief The Y sample (as stored in a 16-bit texture) the converter should produce for an sRGB color.
   *
   * @param srgb sRGB color, components 0 to 1.
   * @param white Luminance of SDR white in cd/m².
   * @param colorspace Stream colorspace.
   * @return Y sample scaled to 0-65535 (divide by 64 for 10-bit codes).
   */
  inline double expected_y(std::array<double, 3> srgb, double white, const video::sunshine_colorspace_t &colorspace) {
    std::array<double, 3> lin;
    for (int i = 0; i < 3; ++i) {
      lin[i] = srgb_to_linear(srgb[i]) * white / 10000.0;
    }
    // BT.709 -> BT.2020 (ITU-R BT.2087)
    const std::array<double, 3> bt2020 {
      0.6274040 * lin[0] + 0.3292820 * lin[1] + 0.0433136 * lin[2],
      0.0690970 * lin[0] + 0.9195400 * lin[1] + 0.0113612 * lin[2],
      0.0163916 * lin[0] + 0.0880132 * lin[1] + 0.8955950 * lin[2],
    };
    auto vectors = video::color_vectors_from_colorspace(colorspace, true);  // as the GL converter (graphics.cpp)
    // color_vec_y[3] is the limited-range offset; the luma shader adds it since Sunshine #4404.
    double y = vectors->color_vec_y[3];
    for (int i = 0; i < 3; ++i) {
      y += vectors->color_vec_y[i] * pq_encode(bt2020[i]);
    }
    return (y * vectors->range_y[0] + vectors->range_y[1]) * 65535.0;
  }

}  // namespace test_hdr
