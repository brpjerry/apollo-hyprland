/**
 * @file src/platform/linux/input/unicode_keymap.cpp
 * @brief Definitions for the temporary Unicode keymaps.
 */
// local includes
#include "unicode_keymap.h"

// standard includes
#include <cstdio>
#include <format>
#include <unordered_map>

// lib includes
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

namespace platf::keyboard::unicode_keymap {
  std::u32string decode_utf8(std::string_view utf8) {
    std::u32string out;
    out.reserve(utf8.size());
    std::size_t i = 0;
    while (i < utf8.size()) {
      auto b0 = static_cast<unsigned char>(utf8[i]);
      std::size_t len;
      char32_t cp;
      if (b0 < 0x80) {
        len = 1;
        cp = b0;
      } else if (b0 >= 0xC2 && b0 <= 0xDF) {
        len = 2;
        cp = b0 & 0x1F;
      } else if (b0 >= 0xE0 && b0 <= 0xEF) {
        len = 3;
        cp = b0 & 0x0F;
      } else if (b0 >= 0xF0 && b0 <= 0xF4) {
        len = 4;
        cp = b0 & 0x07;
      } else {
        ++i;  // Stray continuation byte or invalid lead byte
        continue;
      }
      if (i + len > utf8.size()) {
        break;  // Truncated sequence at the end
      }
      bool valid = true;
      for (std::size_t k = 1; k < len; ++k) {
        auto b = static_cast<unsigned char>(utf8[i + k]);
        if ((b & 0xC0) != 0x80) {
          valid = false;
          break;
        }
        cp = (cp << 6) | (b & 0x3F);
      }
      // Reject overlong forms, surrogates and values past U+10FFFF.
      static constexpr char32_t min_for_len[] {0, 0, 0x80, 0x800, 0x10000};
      if (!valid || cp < min_for_len[len] || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
        ++i;
        continue;
      }
      out.push_back(cp);
      i += len;
    }
    return out;
  }

  std::uint32_t keysym_for(char32_t cp) {
    switch (cp) {
      case U'\n':
      case U'\r':
        return XKB_KEY_Return;
      case U'\t':
        return XKB_KEY_Tab;
      case U'\b':
        return XKB_KEY_BackSpace;
      case 0x1B:
        return XKB_KEY_Escape;
      case 0x7F:
        return XKB_KEY_Delete;
      default:
        break;
    }
    if (cp < 0x20 || (cp >= 0x80 && cp < 0xA0) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
      return XKB_KEY_NoSymbol;
    }
    return xkb_utf32_to_keysym(cp);
  }

  std::vector<batch_t> plan(std::u32string_view text, std::size_t limit) {
    std::vector<batch_t> batches;
    if (limit == 0) {
      return batches;
    }
    std::unordered_map<std::uint32_t, std::uint32_t> key_of;  // keysym -> evdev key code in the current batch
    for (auto cp : text) {
      auto sym = keysym_for(cp);
      if (sym == XKB_KEY_NoSymbol) {
        continue;
      }
      if (batches.empty()) {
        batches.emplace_back();
      }
      auto it = key_of.find(sym);
      if (it == key_of.end()) {
        if (batches.back().keysyms.size() == limit) {
          batches.emplace_back();
          key_of.clear();
        }
        auto &batch = batches.back();
        batch.keysyms.push_back(sym);
        it = key_of.emplace(sym, static_cast<std::uint32_t>(batch.keysyms.size())).first;
      }
      batches.back().keys.push_back(it->second);
    }
    return batches;
  }

  std::string keymap_text(const std::vector<std::uint32_t> &keysyms) {
    // Same layout as wtype's keymap: one key per keysym, XKB keycode = evdev key code + 8.
    std::string out = std::format("xkb_keymap {{\nxkb_keycodes \"{}\" {{\nminimum = 8;\nmaximum = 255;\n", section_name);
    for (std::size_t i = 0; i < keysyms.size(); ++i) {
      out += std::format("<K{}> = {};\n", i + 1, i + 9);
    }
    out += "};\nxkb_types \"(unnamed)\" { include \"complete\" };\nxkb_compatibility \"(unnamed)\" { include \"complete\" };\n";
    out += "xkb_symbols \"(unnamed)\" {\n";
    for (std::size_t i = 0; i < keysyms.size(); ++i) {
      char name[64];
      if (xkb_keysym_get_name(keysyms[i], name, sizeof(name)) < 0) {
        std::snprintf(name, sizeof(name), "0x%08x", keysyms[i]);
      }
      out += std::format("key <K{}> {{[ {} ]}};\n", i + 1, name);
    }
    out += "};\n};\n";
    return out;
  }

  bool is_ours(std::string_view keymap) {
    return keymap.find(std::format("\"{}\"", section_name)) != std::string_view::npos;
  }
}  // namespace platf::keyboard::unicode_keymap
