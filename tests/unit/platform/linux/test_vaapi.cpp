/**
 * @file tests/unit/platform/linux/test_vaapi.cpp
 * @brief Tests for choosing a VAAPI device by the codecs it can encode.
 */

#if defined(__linux__) && defined(SUNSHINE_BUILD_VAAPI)
  // test includes
  #include "../../../tests_common.h"

  // standard includes
  #include <set>

// lib includes
extern "C" {
  #include <va/va.h>
}

  // local includes
  #include "src/platform/linux/vaapi.h"

namespace {
  /**
   * @brief Entrypoint query of a device that encodes the given profiles.
   *
   * @param profiles Profiles with an encoding entrypoint.
   * @return Query for va::can_encode_configured_codecs().
   */
  std::function<bool(int)> device_encoding(std::set<int> profiles) {
    return [profiles = std::move(profiles)](int profile) {
      return profiles.contains(profile);
    };
  }
}  // namespace

// Bugs caught: 0, False positives: 0
TEST(VaapiCodecGatingTest, RequiresAv1EncodingOnlyWhenAv1IsRequired) {
  const auto without_av1 = device_encoding({VAProfileH264Main, VAProfileHEVCMain, VAProfileHEVCMain10});
  const auto with_av1 = device_encoding({VAProfileH264Main, VAProfileHEVCMain, VAProfileHEVCMain10, VAProfileAV1Profile0});

  // AV1 auto (0) or disabled (1): not needed.
  EXPECT_TRUE(va::can_encode_configured_codecs(without_av1, 0, 0));
  EXPECT_TRUE(va::can_encode_configured_codecs(without_av1, 0, 1));
  // AV1 required (2), or required with 10-bit (3): only devices that encode it.
  EXPECT_FALSE(va::can_encode_configured_codecs(without_av1, 0, 2));
  EXPECT_FALSE(va::can_encode_configured_codecs(without_av1, 0, 3));
  EXPECT_TRUE(va::can_encode_configured_codecs(with_av1, 0, 2));
  EXPECT_TRUE(va::can_encode_configured_codecs(with_av1, 0, 3));
}

// Bugs caught: 0, False positives: 0
TEST(VaapiCodecGatingTest, KeepsTheH264AndHevcRequirements) {
  EXPECT_FALSE(va::can_encode_configured_codecs(device_encoding({VAProfileHEVCMain}), 0, 0));
  EXPECT_TRUE(va::can_encode_configured_codecs(device_encoding({VAProfileH264Main}), 1, 0));
  EXPECT_FALSE(va::can_encode_configured_codecs(device_encoding({VAProfileH264Main}), 2, 0));
  EXPECT_TRUE(va::can_encode_configured_codecs(device_encoding({VAProfileH264Main, VAProfileHEVCMain}), 2, 0));
  EXPECT_FALSE(va::can_encode_configured_codecs(device_encoding({VAProfileH264Main, VAProfileHEVCMain}), 3, 0));
  EXPECT_TRUE(va::can_encode_configured_codecs(device_encoding({VAProfileH264Main, VAProfileHEVCMain, VAProfileHEVCMain10}), 3, 0));
}
#endif
