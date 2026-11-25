/**
 * @file src/platform/display_selection.h
 * @brief Platform-neutral helpers for picking a display from an enumerated list of names.
 */
#pragma once

// standard includes
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace platf {
  /**
   * @brief Parse a legacy numeric display index such as "0" or "12".
   * @param value The configured display name.
   * @return The index, or `std::nullopt` if `value` is not made only of decimal digits.
   */
  inline std::optional<std::size_t> parse_display_index(std::string_view value) {
    if (value.empty() || value.size() > 9) {
      return std::nullopt;
    }
    if (!std::all_of(value.begin(), value.end(), [](unsigned char c) {
          return std::isdigit(c) != 0;
        })) {
      return std::nullopt;
    }

    std::size_t index = 0;
    for (const char c : value) {
      index = index * 10 + static_cast<std::size_t>(c - '0');
    }
    return index;
  }

  /**
   * @brief Find a display in an enumerated list.
   * @details An exact name match always wins, so a display that is really called "1" is found by name.
   *          Otherwise, if `allow_legacy_index` is set and `requested` is a decimal number below `names.size()`,
   *          it is treated as an index into `names` (the format older Linux configs used).
   * @param names The enumerated display names, in enumeration order.
   * @param requested The requested display name (must not be empty).
   * @param allow_legacy_index Whether a numeric index is accepted when no name matches.
   * @return The index into `names`, or `std::nullopt` if nothing matches.
   */
  inline std::optional<std::size_t> find_display(const std::vector<std::string> &names, std::string_view requested, bool allow_legacy_index) {
    if (requested.empty()) {
      return std::nullopt;
    }

    for (std::size_t x = 0; x < names.size(); ++x) {
      if (names[x] == requested) {
        return x;
      }
    }

    if (allow_legacy_index) {
      if (auto index = parse_display_index(requested); index && *index < names.size()) {
        return index;
      }
    }

    return std::nullopt;
  }

  /**
   * @brief Join display names for log messages, e.g. "DP-3, HDMI-A-1".
   * @param names The display names.
   * @return The comma separated names, or "(none)" if the list is empty.
   */
  inline std::string join_display_names(const std::vector<std::string> &names) {
    if (names.empty()) {
      return "(none)";
    }

    std::string result;
    for (const auto &name : names) {
      if (!result.empty()) {
        result += ", ";
      }
      result += name;
    }
    return result;
  }
}  // namespace platf
