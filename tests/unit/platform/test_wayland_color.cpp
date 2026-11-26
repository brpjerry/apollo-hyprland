/**
 * @file tests/unit/platform/test_wayland_color.cpp
 * @brief Test src/platform/linux/wayland_color.h (HDR detection and metadata from wp-color-management-v1).
 */
#include "../../tests_common.h"

#include <src/platform/linux/wayland_color.h>

namespace {
  // What Hyprland 0.57.2 reports for a headless output with
  // hl.monitor({ cm = "hdr", supports_hdr = 1, max_luminance = 1000, min_luminance = 0.005, max_avg_luminance = 400 }).
  wl::image_description_t hyprland_hdr() {
    wl::image_description_t desc;
    desc.primaries = wl::chromaticities_t {{708000, 292000, 170000, 797000, 131000, 46000, 312700, 329000}};
    desc.primaries_named = wl::color::primaries::bt2020;
    desc.tf_named = wl::color::tf::st2084_pq;
    desc.min_lum = 50;
    desc.max_lum = 1000;
    desc.reference_lum = 203;
    desc.target_primaries = desc.primaries;
    desc.target_min_lum = 50;
    desc.target_max_lum = 1000;
    desc.target_max_cll = 1000;
    desc.target_max_fall = 400;
    return desc;
  }

  // What Hyprland 0.57.2 reports for an SDR output (cm = "srgb").
  wl::image_description_t hyprland_sdr() {
    wl::image_description_t desc;
    desc.primaries = wl::chromaticities_t {{640000, 330000, 300000, 600000, 150000, 60000, 312700, 329000}};
    desc.tf_named = wl::color::tf::srgb;
    desc.min_lum = 2000;
    desc.max_lum = 80;
    desc.reference_lum = 80;
    desc.target_primaries = desc.primaries;
    desc.target_min_lum = 2000;
    desc.target_max_lum = 80;
    return desc;
  }
}  // namespace

// Bugs caught: 0, False positives: 0
TEST(WaylandColor, PqWithBt2020IsHdr10) {
  EXPECT_TRUE(wl::is_hdr10(hyprland_hdr()));
  EXPECT_FALSE(wl::is_hdr10(hyprland_sdr()));
}

// Bugs caught: 0, False positives: 0
TEST(WaylandColor, NeedsBothPqAndBt2020) {
  auto desc = hyprland_hdr();
  desc.tf_named = wl::color::tf::gamma22;
  EXPECT_FALSE(wl::is_hdr10(desc)) << "BT.2020 with an SDR transfer function (Hyprland's \"wide\" preset)";

  desc = hyprland_hdr();
  desc.tf_named = wl::color::tf::hlg;
  EXPECT_FALSE(wl::is_hdr10(desc));

  desc = hyprland_hdr();
  desc.primaries_named = wl::color::primaries::srgb;
  EXPECT_FALSE(wl::is_hdr10(desc)) << "PQ with BT.709 primaries";

  desc = hyprland_hdr();
  desc.tf_named.reset();
  desc.tf_power = 22000;
  EXPECT_FALSE(wl::is_hdr10(desc));
}

// Bugs caught: 0, False positives: 0
TEST(WaylandColor, Bt2020ChromaticitiesCountWithoutTheNamedPrimaries) {
  auto desc = hyprland_hdr();
  desc.primaries_named.reset();
  EXPECT_TRUE(wl::is_hdr10(desc));

  // Rounding in the compositor is tolerated...
  desc.primaries->xy[0] += 400;
  EXPECT_TRUE(wl::is_hdr10(desc));

  // ...a different gamut is not.
  desc.primaries = wl::chromaticities_t {{680000, 320000, 265000, 690000, 150000, 60000, 312700, 329000}};  // P3-D65
  EXPECT_FALSE(wl::is_hdr10(desc));

  desc.primaries.reset();
  EXPECT_FALSE(wl::is_hdr10(desc)) << "no primaries at all";
}

// Bugs caught: 0, False positives: 0
TEST(WaylandColor, FillsMasteringMetadataInMoonlightUnits) {
  SS_HDR_METADATA metadata;
  ASSERT_TRUE(wl::fill_hdr_metadata(hyprland_hdr(), metadata));

  // Chromaticities: protocol x 1'000'000 -> SMPTE ST 2086 x 50'000.
  EXPECT_EQ(metadata.displayPrimaries[0].x, 35400);
  EXPECT_EQ(metadata.displayPrimaries[0].y, 14600);
  EXPECT_EQ(metadata.displayPrimaries[1].x, 8500);
  EXPECT_EQ(metadata.displayPrimaries[1].y, 39850);
  EXPECT_EQ(metadata.displayPrimaries[2].x, 6550);
  EXPECT_EQ(metadata.displayPrimaries[2].y, 2300);
  EXPECT_EQ(metadata.whitePoint.x, 15635);
  EXPECT_EQ(metadata.whitePoint.y, 16450);

  EXPECT_EQ(metadata.maxDisplayLuminance, 1000);
  EXPECT_EQ(metadata.minDisplayLuminance, 50);  // 0.005 cd/m² in 1/10000 cd/m²
  EXPECT_EQ(metadata.maxContentLightLevel, 1000);
  EXPECT_EQ(metadata.maxFrameAverageLightLevel, 400);
}

// Bugs caught: 0, False positives: 0
TEST(WaylandColor, PrefersTheTargetVolumeForTheMasteringDisplay) {
  auto desc = hyprland_hdr();
  desc.target_primaries = wl::chromaticities_t {{680000, 320000, 265000, 690000, 150000, 60000, 312700, 329000}};
  desc.target_min_lum = 1;
  desc.target_max_lum = 600;
  desc.target_max_cll.reset();
  desc.target_max_fall.reset();

  SS_HDR_METADATA metadata;
  ASSERT_TRUE(wl::fill_hdr_metadata(desc, metadata));
  EXPECT_EQ(metadata.displayPrimaries[0].x, 34000);
  EXPECT_EQ(metadata.maxDisplayLuminance, 600);
  EXPECT_EQ(metadata.minDisplayLuminance, 1);
  EXPECT_EQ(metadata.maxContentLightLevel, 0) << "unknown content light levels stay 0";
  EXPECT_EQ(metadata.maxFrameAverageLightLevel, 0);

  // Without a target volume the primary one describes the display.
  desc.target_primaries.reset();
  desc.target_min_lum.reset();
  desc.target_max_lum.reset();
  ASSERT_TRUE(wl::fill_hdr_metadata(desc, metadata));
  EXPECT_EQ(metadata.displayPrimaries[0].x, 35400);
  EXPECT_EQ(metadata.maxDisplayLuminance, 1000);
  EXPECT_EQ(metadata.minDisplayLuminance, 50);
}

// Bugs caught: 0, False positives: 0
TEST(WaylandColor, SdrHasNoMetadata) {
  SS_HDR_METADATA metadata;
  std::memset(&metadata, 0xff, sizeof(metadata));
  EXPECT_FALSE(wl::fill_hdr_metadata(hyprland_sdr(), metadata));
  EXPECT_EQ(metadata.maxDisplayLuminance, 0);
  EXPECT_EQ(metadata.displayPrimaries[0].x, 0);
}

// Bugs caught: 0, False positives: 0
TEST(WaylandColor, ClampsLuminancesThatDoNotFit) {
  auto desc = hyprland_hdr();
  desc.target_max_lum = 100000;
  desc.target_min_lum = 1'000'000;
  SS_HDR_METADATA metadata;
  ASSERT_TRUE(wl::fill_hdr_metadata(desc, metadata));
  EXPECT_EQ(metadata.maxDisplayLuminance, 0xFFFF);
  EXPECT_EQ(metadata.minDisplayLuminance, 0xFFFF);
}
