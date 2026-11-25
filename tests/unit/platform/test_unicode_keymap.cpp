/**
 * @file tests/unit/platform/test_unicode_keymap.cpp
 * @brief Test the temporary Unicode keymaps in src/platform/linux/input/unicode_keymap.*.
 */
#include "../../tests_common.h"

#if defined(__linux__) && defined(SUNSHINE_BUILD_WAYLAND)

  #include <src/platform/linux/input/unicode_keymap.h>

  #include <cstdlib>
  #include <memory>
  #include <xkbcommon/xkbcommon-keysyms.h>
  #include <xkbcommon/xkbcommon.h>

using namespace platf::keyboard::unicode_keymap;

namespace {
  struct xkb_deleter_t {
    void operator()(xkb_context *c) const {
      xkb_context_unref(c);
    }

    void operator()(xkb_keymap *k) const {
      xkb_keymap_unref(k);
    }

    void operator()(xkb_state *s) const {
      xkb_state_unref(s);
    }
  };

  /**
   * @brief Types @p keys (evdev codes) on a keymap compiled from @p text and returns the UTF-8 the keys produce,
   *        as a Wayland client would.
   */
  std::string type_on(const std::string &text, const std::vector<std::uint32_t> &keys) {
    std::unique_ptr<xkb_context, xkb_deleter_t> ctx {xkb_context_new(XKB_CONTEXT_NO_FLAGS)};
    std::unique_ptr<xkb_keymap, xkb_deleter_t> keymap {xkb_keymap_new_from_string(ctx.get(), text.c_str(), XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS)};
    if (!keymap) {
      return "<keymap does not compile>";
    }
    std::unique_ptr<xkb_state, xkb_deleter_t> state {xkb_state_new(keymap.get())};
    std::string out;
    for (auto key : keys) {
      char buf[16] {};
      xkb_state_key_get_utf8(state.get(), key + 8, buf, sizeof(buf));
      out += buf;
    }
    return out;
  }
}  // namespace

// Bugs caught: 0, False positives: 0
TEST(UnicodeKeymapTest, DecodesUtf8AndDropsInvalidSequences) {
  EXPECT_EQ(decode_utf8("h\xc3\xa9llo \xe4\xb8\x96\xe7\x95\x8c \xf0\x9f\x8e\xae"), U"héllo 世界 🎮");
  // Overlong '/', a UTF-16 surrogate, a stray continuation byte, a code point past U+10FFFF, a truncated tail.
  EXPECT_EQ(decode_utf8("a\xc0\xaf" "b\xed\xa0\x80" "c\x80" "d\xf4\x90\x80\x80" "e\xe4\xb8"), U"abcde");
  EXPECT_EQ(decode_utf8(""), U"");
}

// Bugs caught: 0, False positives: 0
TEST(UnicodeKeymapTest, ChoosesKeysyms) {
  EXPECT_EQ(keysym_for(U'a'), (std::uint32_t) XKB_KEY_a);
  EXPECT_EQ(keysym_for(U' '), (std::uint32_t) XKB_KEY_space);
  EXPECT_EQ(keysym_for(U'é'), (std::uint32_t) XKB_KEY_eacute);
  EXPECT_EQ(keysym_for(U'世'), 0x01004E16u);
  EXPECT_EQ(keysym_for(U'🎮'), 0x0101F3AEu);
  EXPECT_EQ(keysym_for(U'\n'), (std::uint32_t) XKB_KEY_Return);
  EXPECT_EQ(keysym_for(U'\r'), (std::uint32_t) XKB_KEY_Return);
  EXPECT_EQ(keysym_for(U'\t'), (std::uint32_t) XKB_KEY_Tab);
  EXPECT_EQ(keysym_for(U'\b'), (std::uint32_t) XKB_KEY_BackSpace);
  EXPECT_EQ(keysym_for(0x01), (std::uint32_t) XKB_KEY_NoSymbol);
  EXPECT_EQ(keysym_for(0x85), (std::uint32_t) XKB_KEY_NoSymbol);
  EXPECT_EQ(keysym_for(0xD800), (std::uint32_t) XKB_KEY_NoSymbol);
}

// Bugs caught: 0, False positives: 0
TEST(UnicodeKeymapTest, OneEventIsOneKeymap) {
  auto batches = plan(U"héllo 世界 🎮");
  ASSERT_EQ(batches.size(), 1u);
  // h é l o ␠ 世 界 🎮: repeated characters reuse their key.
  EXPECT_EQ(batches[0].keysyms.size(), 8u);
  EXPECT_EQ(batches[0].keys, (std::vector<std::uint32_t> {1, 2, 3, 3, 4, 5, 6, 7, 5, 8}));
  EXPECT_EQ(type_on(keymap_text(batches[0].keysyms), batches[0].keys), "héllo 世界 🎮");
}

// Bugs caught: 0, False positives: 0
TEST(UnicodeKeymapTest, SkipsUntypableCharacters) {
  EXPECT_TRUE(plan(U"\x01\x02").empty());
  auto batches = plan(U"a\x01" U"b");
  ASSERT_EQ(batches.size(), 1u);
  EXPECT_EQ(type_on(keymap_text(batches[0].keysyms), batches[0].keys), "ab");
}

// Bugs caught: 0, False positives: 0
TEST(UnicodeKeymapTest, SplitsWhenKeysRunOut) {
  // 300 distinct CJK characters, then the first one again.
  std::u32string text;
  for (char32_t c = 0x4E00; c < 0x4E00 + 300; ++c) {
    text += c;
  }
  text += char32_t {0x4E00};

  auto batches = plan(text);
  ASSERT_EQ(batches.size(), 2u);
  EXPECT_EQ(batches[0].keysyms.size(), max_keys);
  EXPECT_EQ(batches[1].keysyms.size(), 300 - max_keys + 1);

  std::string typed;
  for (auto &batch : batches) {
    for (auto key : batch.keys) {
      EXPECT_GE(key, 1u);
      EXPECT_LE(key + 8, 255u);  // Xwayland cannot use keycodes above 255
    }
    typed += type_on(keymap_text(batch.keysyms), batch.keys);
  }
  std::string expected;
  for (auto c : text) {
    char buf[8] {};
    xkb_keysym_to_utf8(xkb_utf32_to_keysym(c), buf, sizeof(buf));
    expected += buf;
  }
  EXPECT_EQ(typed, expected);

  // A smaller limit splits sooner.
  EXPECT_EQ(plan(U"abcabcd", 3).size(), 2u);
}

// Bugs caught: 0, False positives: 0
TEST(UnicodeKeymapTest, RecognisesOwnKeymaps) {
  EXPECT_TRUE(is_ours(keymap_text({XKB_KEY_a})));

  // The compositor may hand our keymap back re-serialised by xkbcommon; it must still be recognised.
  std::unique_ptr<xkb_context, xkb_deleter_t> ctx {xkb_context_new(XKB_CONTEXT_NO_FLAGS)};
  auto text = keymap_text({XKB_KEY_a, 0x01004E16});
  std::unique_ptr<xkb_keymap, xkb_deleter_t> ours {xkb_keymap_new_from_string(ctx.get(), text.c_str(), XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS)};
  ASSERT_TRUE(ours);
  char *reserialised = xkb_keymap_get_as_string(ours.get(), XKB_KEYMAP_FORMAT_TEXT_V1);
  ASSERT_NE(reserialised, nullptr);
  EXPECT_TRUE(is_ours(reserialised));
  std::free(reserialised);

  xkb_rule_names us {.rules = nullptr, .model = nullptr, .layout = "us", .variant = nullptr, .options = nullptr};
  std::unique_ptr<xkb_keymap, xkb_deleter_t> normal {xkb_keymap_new_from_names(ctx.get(), &us, XKB_KEYMAP_COMPILE_NO_FLAGS)};
  ASSERT_TRUE(normal);
  char *normal_text = xkb_keymap_get_as_string(normal.get(), XKB_KEYMAP_FORMAT_TEXT_V1);
  ASSERT_NE(normal_text, nullptr);
  EXPECT_FALSE(is_ours(normal_text));
  std::free(normal_text);
}

#endif
