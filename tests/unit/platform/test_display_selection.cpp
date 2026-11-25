/**
 * @file tests/unit/platform/test_display_selection.cpp
 * @brief Test src/platform/display_selection.h.
 */
#include "../../tests_common.h"

#include <src/platform/display_selection.h>

namespace {
  const std::vector<std::string> wayland_names {"DP-3", "HDMI-A-1", "HEADLESS-2"};
}  // namespace

// Bugs caught: 0, False positives: 0
TEST(DisplaySelectionTest, MatchesConnectorName) {
  EXPECT_EQ(platf::find_display(wayland_names, "DP-3", true), 0u);
  EXPECT_EQ(platf::find_display(wayland_names, "HDMI-A-1", true), 1u);
  EXPECT_EQ(platf::find_display(wayland_names, "HEADLESS-2", false), 2u);
}

// Bugs caught: 0, False positives: 0
TEST(DisplaySelectionTest, NameMatchIsIndependentOfEnumerationOrder) {
  const std::vector<std::string> reordered {"HEADLESS-2", "HDMI-A-1", "DP-3"};
  EXPECT_EQ(platf::find_display(reordered, "DP-3", true), 2u);
}

// Bugs caught: 0, False positives: 0
TEST(DisplaySelectionTest, AcceptsLegacyIndex) {
  EXPECT_EQ(platf::find_display(wayland_names, "0", true), 0u);
  EXPECT_EQ(platf::find_display(wayland_names, "1", true), 1u);
  EXPECT_EQ(platf::find_display(wayland_names, "2", true), 2u);
}

// Bugs caught: 0, False positives: 0
TEST(DisplaySelectionTest, LegacyIndexCanBeDisabled) {
  EXPECT_EQ(platf::find_display(wayland_names, "1", false), std::nullopt);
}

// Bugs caught: 0, False positives: 0
TEST(DisplaySelectionTest, ExactNameBeatsLegacyIndex) {
  // X11/KMS names are themselves numbers; "1" must select the display named "1", not index 1.
  const std::vector<std::string> numeric {"2", "1", "0"};
  EXPECT_EQ(platf::find_display(numeric, "1", true), 1u);
  EXPECT_EQ(platf::find_display(numeric, "0", true), 2u);
}

// Bugs caught: 0, False positives: 0
TEST(DisplaySelectionTest, MissingDisplayIsNotFound) {
  EXPECT_EQ(platf::find_display(wayland_names, "DP-1", true), std::nullopt);
  EXPECT_EQ(platf::find_display(wayland_names, "dp-3", true), std::nullopt);
  EXPECT_EQ(platf::find_display(wayland_names, "3", true), std::nullopt);
  EXPECT_EQ(platf::find_display(wayland_names, "-1", true), std::nullopt);
  EXPECT_EQ(platf::find_display(wayland_names, "1x", true), std::nullopt);
  EXPECT_EQ(platf::find_display(wayland_names, "99999999999999999999", true), std::nullopt);
  EXPECT_EQ(platf::find_display(wayland_names, "", true), std::nullopt);
  EXPECT_EQ(platf::find_display({}, "0", true), std::nullopt);
}

// Bugs caught: 0, False positives: 0
TEST(DisplaySelectionTest, JoinsNamesForLogs) {
  EXPECT_EQ(platf::join_display_names(wayland_names), "DP-3, HDMI-A-1, HEADLESS-2");
  EXPECT_EQ(platf::join_display_names({}), "(none)");
}
