/**
 * @file src/platform/linux/kms_display.h
 * @brief Display enumeration data for one KMS capture backend.
 */
#pragma once

// standard includes
#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// lib includes
#include <xf86drmMode.h>

// local includes
#include "display_geometry.h"

namespace platf::kms {
  /**
   * @brief The DRM connector type a compositor output name starts with (the part before its last dash).
   *
   * The spellings come from libdrm (drmModeGetConnectorTypeName), Weston and Mutter. `Unknown` and `Unknown<number>`
   * (a raw type value) are connector types too; any other name, such as a virtual display's, is not.
   *
   * @param name Type part of the output name, e.g. `HDMI-A` of `HDMI-A-1`.
   * @return The connector type, or no value when the name isn't a connector type.
   */
  inline std::optional<std::uint32_t> connector_type_from_name(const std::string_view name) {
    static constexpr std::array<std::pair<std::string_view, std::uint32_t>, 33> types {{
      {"VGA", DRM_MODE_CONNECTOR_VGA},
      {"DVII", DRM_MODE_CONNECTOR_DVII},
      {"DVI-I", DRM_MODE_CONNECTOR_DVII},
      {"DVID", DRM_MODE_CONNECTOR_DVID},
      {"DVI-D", DRM_MODE_CONNECTOR_DVID},
      {"DVIA", DRM_MODE_CONNECTOR_DVIA},
      {"DVI-A", DRM_MODE_CONNECTOR_DVIA},
      {"Composite", DRM_MODE_CONNECTOR_Composite},
      {"SVIDEO", DRM_MODE_CONNECTOR_SVIDEO},
      {"S-Video", DRM_MODE_CONNECTOR_SVIDEO},
      {"LVDS", DRM_MODE_CONNECTOR_LVDS},
      {"Component", DRM_MODE_CONNECTOR_Component},
      {"9PinDIN", DRM_MODE_CONNECTOR_9PinDIN},
      {"DIN", DRM_MODE_CONNECTOR_9PinDIN},
      {"DisplayPort", DRM_MODE_CONNECTOR_DisplayPort},
      {"DP", DRM_MODE_CONNECTOR_DisplayPort},
      {"HDMIA", DRM_MODE_CONNECTOR_HDMIA},
      {"HDMI-A", DRM_MODE_CONNECTOR_HDMIA},
      {"HDMI", DRM_MODE_CONNECTOR_HDMIA},
      {"HDMIB", DRM_MODE_CONNECTOR_HDMIB},
      {"HDMI-B", DRM_MODE_CONNECTOR_HDMIB},
      {"TV", DRM_MODE_CONNECTOR_TV},
      {"eDP", DRM_MODE_CONNECTOR_eDP},
      {"Meta", DRM_MODE_CONNECTOR_VIRTUAL},  // GNOME Shell (Mutter) virtual monitor
      {"VIRTUAL", DRM_MODE_CONNECTOR_VIRTUAL},
      {"Virtual", DRM_MODE_CONNECTOR_VIRTUAL},
      {"DSI", DRM_MODE_CONNECTOR_DSI},
      {"DPI", DRM_MODE_CONNECTOR_DPI},
      {"WRITEBACK", DRM_MODE_CONNECTOR_WRITEBACK},
      {"Writeback", DRM_MODE_CONNECTOR_WRITEBACK},
      {"SPI", DRM_MODE_CONNECTOR_SPI},
#ifdef DRM_MODE_CONNECTOR_USB
      {"USB", DRM_MODE_CONNECTOR_USB},
#else
      {"", DRM_MODE_CONNECTOR_Unknown},  // Keeps the array size; an empty name is not a connector type below.
#endif
      {"Unknown", DRM_MODE_CONNECTOR_Unknown},
    }};
    if (name.empty()) {
      return std::nullopt;
    }
    for (const auto &[type_name, type] : types) {
      if (name == type_name) {
        return type;
      }
    }

    // "Unknown" followed by the raw type value, all of it digits.
    constexpr auto unknown = std::string_view {"Unknown"};
    if (name.starts_with(unknown) && name.size() > unknown.size()) {
      const auto digits = name.substr(unknown.size());
      std::uint32_t type;
      const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), type);
      if (error == std::errc {} && end == digits.data() + digits.size()) {
        return type;
      }
    }
    return std::nullopt;
  }

  /**
   * @brief Connector identity and geometry for one KMS monitor.
   */
  struct monitor_t {
    std::uint32_t type;  ///< DRM connector type.
    std::uint32_t index;  ///< Index within the connector type.
    std::uint32_t monitor_index;  ///< Index in the capture backend's display list.
    touch_port_t viewport;  ///< Monitor geometry for capture and absolute input.
  };

  /**
   * @brief DRM card and its monitor descriptors indexed by CRTC ID.
   */
  struct card_descriptor_t {
    std::string path;  ///< DRM card device path.
    std::map<std::uint32_t, monitor_t> crtc_to_monitor;  ///< Monitors associated with the card.
  };

  /**
   * @brief A display discovered on a particular card and plane.
   */
  struct output_t {
    std::string name;  ///< DRM connector name.
    std::size_t card_index;  ///< Index into this enumeration's card descriptors.
    std::uint32_t plane_id;  ///< ID of the DRM plane that was scanning out this output.
    std::uint32_t crtc_id;  ///< CRTC the plane was attached to during enumeration.

    /**
     * @brief Whether a name given to this output is a guess rather than its DRM connector name.
     *
     * The KMS backend matches compositor output names to DRM connectors by connector type and index, which can give
     * a connector the name of an unrelated output (an unrecognized type such as APOLLO-00000001 parses as Unknown-1).
     *
     * @param reported Name the capture reports for this output.
     * @return `true` when the name differs from the DRM connector name.
     */
    bool name_inferred(std::string_view reported) const {
      return reported != name;
    }
  };

  /**
   * @brief Display names, card descriptors, and desktop geometry enumerated together for one memory type.
   */
  struct display_enumeration_t {
    std::vector<output_t> outputs;  ///< Displays in capture order.
    std::vector<card_descriptor_t> cards;  ///< DRM cards in capture order.
    desktop_geometry_t desktop;  ///< Geometry of this enumeration's available outputs.

    /**
     * @brief Map a connector name or a legacy numeric display name to a capture index.
     *
     * @param name Display name from the stream or host configuration.
     * @return Display index, zero for the default name, or no value for an unknown connector.
     */
    std::optional<std::size_t> display_index(std::string_view name) const {
      if (name.empty()) {
        return 0;
      }

      std::size_t index;
      const auto [end, parse_error] = std::from_chars(name.data(), name.data() + name.size(), index);
      if (end == name.data() + name.size() && parse_error == std::errc {}) {
        return index;
      }

      auto position = std::ranges::find(outputs, name, &output_t::name);
      if (position == outputs.end()) {
        return std::nullopt;
      }
      return static_cast<std::size_t>(position - outputs.begin());
    }
  };
}  // namespace platf::kms
