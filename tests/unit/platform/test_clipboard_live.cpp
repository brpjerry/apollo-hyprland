/**
 * @file tests/unit/platform/test_clipboard_live.cpp
 * @brief The Wayland clipboard (src/platform/linux/clipboard.cpp) against a private headless Hyprland.
 * @details These tests read and replace the clipboard, so they only run inside the instance that
 *          scripts/hyprland/test-live.sh (run-headless-hyprland.sh) starts, never against a desktop session:
 *          they skip unless APOLLO_HEADLESS_HYPRLAND_DIR names a directory with the instance marker that is also
 *          XDG_RUNTIME_DIR, and WAYLAND_DISPLAY is a socket name inside it. Interop tests use wl-copy/wl-paste
 *          (wl-clipboard) when installed.
 */
#include "../../tests_common.h"

#if defined(__linux__) && defined(SUNSHINE_BUILD_WAYLAND)

  #include <src/platform/linux/clipboard.h>
  #include <src/platform/linux/clipboard_logic.h>

  #include <ext-data-control-v1.h>
  #include <filesystem>
  #include <fstream>
  #include <sstream>
  #include <sys/wait.h>
  #include <thread>
  #include <wayland-client.h>

using namespace platf::clipboard;
using namespace std::literals;

namespace {
  /**
   * @brief Absolute path of the private instance's Wayland socket, or an explanation why there is none.
   */
  std::optional<std::string> private_wayland_socket(std::string &why) {
    const char *dir = std::getenv("APOLLO_HEADLESS_HYPRLAND_DIR");
    const char *runtime = std::getenv("XDG_RUNTIME_DIR");
    const char *display = std::getenv("WAYLAND_DISPLAY");
    if (!dir || !*dir) {
      why = "not inside a private headless Hyprland (run scripts/hyprland/test-live.sh)";
      return std::nullopt;
    }
    std::error_code ec;
    if (!std::filesystem::exists(std::filesystem::path {dir} / ".apollo-headless-hyprland", ec)) {
      why = "APOLLO_HEADLESS_HYPRLAND_DIR has no instance marker";
      return std::nullopt;
    }
    if (!runtime || std::filesystem::path {runtime} != std::filesystem::path {dir}) {
      why = "XDG_RUNTIME_DIR is not the private instance's directory";
      return std::nullopt;
    }
    if (!display || !*display || std::string_view {display}.find('/') != std::string_view::npos || std::getenv("WAYLAND_SOCKET")) {
      why = "WAYLAND_DISPLAY does not name a socket of the private instance";
      return std::nullopt;
    }
    auto socket = std::filesystem::path {dir} / display;
    if (!std::filesystem::exists(socket, ec)) {
      why = "the private instance's Wayland socket is gone";
      return std::nullopt;
    }
    return socket.string();
  }

  bool have(const char *program) {
    return std::system(("command -v "s + program + " > /dev/null 2>&1").c_str()) == 0;
  }

  std::string read_file(const std::filesystem::path &path) {
    std::ifstream in {path, std::ios::binary};
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
  }

  void write_file(const std::filesystem::path &path, std::string_view data) {
    std::ofstream out {path, std::ios::binary};
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
  }

  // About 1 MiB of mixed ASCII and multi-byte UTF-8 with the given line ending.
  std::string large_text(std::string_view eol, std::string_view tag) {
    std::string text;
    while (text.size() < 1024 * 1024) {
      text += tag;
      text += std::to_string(text.size());
      text += " grüße, 世界, Ελληνικά, 😀";
      text += eol;
    }
    return text;
  }

  /**
   * @brief A selection owner that offers text but never writes it, for the timeout test.
   * @details It never dispatches, so the pipe a paste target hands it stays queued, unanswered, on its socket.
   */
  struct silent_owner_t {
    wl_display *display = nullptr;
    wl_seat *seat = nullptr;
    ext_data_control_manager_v1 *manager = nullptr;
    ext_data_control_device_v1 *device = nullptr;
    ext_data_control_source_v1 *source = nullptr;

    explicit silent_owner_t(const std::string &socket) {
      display = wl_display_connect(socket.c_str());
      if (!display) {
        return;
      }
      auto registry = wl_display_get_registry(display);
      static constexpr wl_registry_listener listener {
        .global = [](void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t) {
          auto self = static_cast<silent_owner_t *>(data);
          if (std::string_view {interface} == wl_seat_interface.name && !self->seat) {
            self->seat = static_cast<wl_seat *>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
          } else if (std::string_view {interface} == ext_data_control_manager_v1_interface.name) {
            self->manager = static_cast<ext_data_control_manager_v1 *>(wl_registry_bind(registry, name, &ext_data_control_manager_v1_interface, 1));
          }
        },
        .global_remove = [](void *, wl_registry *, uint32_t) {},
      };
      wl_registry_add_listener(registry, &listener, this);
      wl_display_roundtrip(display);
      wl_registry_destroy(registry);
      if (!seat || !manager) {
        return;
      }
      device = ext_data_control_manager_v1_get_data_device(manager, seat);
      source = ext_data_control_manager_v1_create_data_source(manager);
      ext_data_control_source_v1_offer(source, "text/plain;charset=utf-8");
      ext_data_control_device_v1_set_selection(device, source);
      wl_display_roundtrip(display);
    }

    ~silent_owner_t() {
      if (display) {
        // Disconnecting closes the queued pipes and destroys the source.
        wl_display_disconnect(display);
      }
    }

    bool ok() const {
      return source != nullptr;
    }
  };

  struct ClipboardLiveTest: testing::TestWithParam<protocol_e> {
    void SetUp() override {
      std::string why;
      auto path = private_wayland_socket(why);
      if (!path) {
        GTEST_SKIP() << why;
      }
      socket = *path;
      options.protocol = GetParam();
      clipboard = wayland_clipboard_t::connect(socket.c_str(), options);
      if (!clipboard) {
        // Either protocol missing is a skip; the Any case must always work on Hyprland.
        if (GetParam() == protocol_e::any) {
          FAIL() << "no data-control protocol on the private Hyprland";
        }
        GTEST_SKIP() << "the compositor lacks this data-control protocol";
      }
      std::cout << "Clipboard protocol: " << clipboard->protocol() << std::endl;

      tmp = std::filesystem::temp_directory_path() / ("apollo-clipboard-test-" + std::to_string(getpid()));
      std::filesystem::create_directories(tmp);
    }

    void TearDown() override {
      if (!tmp.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(tmp, ec);
      }
    }

    /// Runs a shell command with the private instance as its Wayland display.
    int run(const std::string &command) const {
      auto full = "WAYLAND_DISPLAY='" + socket + "' " + command;
      return WEXITSTATUS(std::system(full.c_str()));
    }

    /// Polls get() until it returns @p expected or 5 s pass.
    std::optional<std::string> get_until(const std::string &expected) const {
      std::optional<std::string> last;
      auto deadline = std::chrono::steady_clock::now() + 5s;
      do {
        last = clipboard->get();
        if (last == expected) {
          break;
        }
        std::this_thread::sleep_for(50ms);
      } while (std::chrono::steady_clock::now() < deadline);
      return last;
    }

    std::string socket;
    options_t options;
    std::unique_ptr<wayland_clipboard_t> clipboard;
    std::filesystem::path tmp;
  };
}  // namespace

INSTANTIATE_TEST_SUITE_P(
  Protocols,
  ClipboardLiveTest,
  testing::Values(protocol_e::any, protocol_e::ext, protocol_e::wlr),
  [](const auto &info) {
    switch (info.param) {
      case protocol_e::ext:
        return "Ext"s;
      case protocol_e::wlr:
        return "Wlr"s;
      default:
        return "Any"s;
    }
  }
);

// Bugs caught: 0, False positives: 0
TEST_P(ClipboardLiveTest, ReadsBackOwnSelectionWithLfLineEndings) {
  ASSERT_TRUE(clipboard->set("héllo 世界\r\nline 2\r\n"));
  EXPECT_EQ(clipboard->get(), "héllo 世界\nline 2\n");
  EXPECT_EQ(clipboard->active_sources(), 1u);

  // A second set replaces the first source, which the compositor cancels.
  ASSERT_TRUE(clipboard->set("second"));
  EXPECT_EQ(get_until("second"), "second");
  EXPECT_EQ(clipboard->active_sources(), 1u);
  EXPECT_TRUE(clipboard->alive());
}

// Bugs caught: 0, False positives: 0
TEST_P(ClipboardLiveTest, LargeNonAsciiTextToWlPaste) {
  if (!have("wl-paste")) {
    GTEST_SKIP() << "wl-paste (wl-clipboard) is not installed";
  }
  auto text = large_text("\r\n", "client→host ");
  ASSERT_TRUE(clipboard->set(text));

  // The source keeps serving every paste until it is replaced.
  for (int paste = 0; paste < 2; ++paste) {
    auto out = tmp / ("paste" + std::to_string(paste));
    ASSERT_EQ(run("timeout 10 wl-paste --no-newline --type 'text/plain;charset=utf-8' > '" + out.string() + "'"), 0);
    auto pasted = read_file(out);
    EXPECT_EQ(pasted.size(), crlf_to_lf(text).size()) << "paste " << paste;
    EXPECT_TRUE(pasted == crlf_to_lf(text)) << "paste " << paste << " differs";
  }
  // X11-style types are served too.
  auto out = tmp / "utf8_string";
  ASSERT_EQ(run("timeout 10 wl-paste --no-newline --type UTF8_STRING > '" + out.string() + "'"), 0);
  EXPECT_TRUE(read_file(out) == crlf_to_lf(text));
}

// Bugs caught: 0, False positives: 0
TEST_P(ClipboardLiveTest, LargeNonAsciiTextFromWlCopy) {
  if (!have("wl-copy")) {
    GTEST_SKIP() << "wl-copy (wl-clipboard) is not installed";
  }
  ASSERT_TRUE(clipboard->set("ours"));
  ASSERT_EQ(clipboard->active_sources(), 1u);

  auto text = large_text("\n", "host→client ");
  auto in = tmp / "copy";
  write_file(in, text);
  // wl-copy forks a server that lives until its selection is replaced.
  ASSERT_EQ(run("timeout 10 wl-copy --type 'text/plain;charset=utf-8' < '" + in.string() + "' > /dev/null 2>&1"), 0);

  auto got = get_until(text);
  ASSERT_TRUE(got);
  EXPECT_EQ(got->size(), text.size());
  EXPECT_TRUE(*got == text) << "selection read from wl-copy differs";
  // Our source was cancelled when wl-copy took the selection.
  EXPECT_EQ(clipboard->active_sources(), 0u);

  // Take the selection back so the wl-copy server exits.
  ASSERT_TRUE(clipboard->set("done"));
}

// Bugs caught: 0, False positives: 0
TEST_P(ClipboardLiveTest, PicksTextTypeFromOwnerOfferingSeveral) {
  if (!have("wl-copy")) {
    GTEST_SKIP() << "wl-copy (wl-clipboard) is not installed";
  }
  // wl-copy offers text/plain;charset=utf-8, UTF8_STRING, text/plain, TEXT and STRING for text input.
  ASSERT_EQ(run("timeout 10 wl-copy 'ünïcødé' > /dev/null 2>&1"), 0);
  EXPECT_EQ(get_until("ünïcødé"), "ünïcødé");
  ASSERT_TRUE(clipboard->set("done"));
}

// Bugs caught: 0, False positives: 0
TEST_P(ClipboardLiveTest, EmptyAndNonTextSelectionsReadAsEmpty) {
  if (!have("wl-copy")) {
    GTEST_SKIP() << "wl-copy (wl-clipboard) is not installed";
  }
  ASSERT_EQ(run("timeout 10 wl-copy --clear > /dev/null 2>&1"), 0);
  EXPECT_EQ(get_until(""), "");

  auto png = tmp / "image.png";
  write_file(png, "\x89PNG\r\n\x1a\n not really a png");
  // Own the selection first, so that wl-copy having taken it shows as our source being cancelled.
  ASSERT_TRUE(clipboard->set("marker"));
  ASSERT_EQ(run("timeout 10 wl-copy --type image/png < '" + png.string() + "' > /dev/null 2>&1"), 0);
  auto deadline = std::chrono::steady_clock::now() + 5s;
  while (clipboard->active_sources() != 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(50ms);
  }
  ASSERT_EQ(clipboard->active_sources(), 0u);
  EXPECT_EQ(clipboard->get(), "");
  ASSERT_TRUE(clipboard->set("done"));
}

// Bugs caught: 0, False positives: 0
TEST_P(ClipboardLiveTest, OwnerThatNeverAnswersTimesOut) {
  options.read_timeout = 300ms;
  clipboard = wayland_clipboard_t::connect(socket.c_str(), options);
  ASSERT_TRUE(clipboard);
  {
    silent_owner_t owner {socket};
    if (!owner.ok()) {
      GTEST_SKIP() << "the compositor lacks ext-data-control-v1 for the silent owner";
    }
    // The silent owner's selection reaches our event thread asynchronously; until it has, get() still reads the
    // previous (empty) selection and returns at once.
    std::optional<std::string> got;
    std::chrono::steady_clock::duration took {};
    for (int attempt = 0; attempt < 20; ++attempt) {
      auto start = std::chrono::steady_clock::now();
      got = clipboard->get();
      took = std::chrono::steady_clock::now() - start;
      if (!got) {
        break;
      }
      std::this_thread::sleep_for(50ms);
    }
    EXPECT_FALSE(got) << "got " << got->size() << " bytes";
    EXPECT_GE(took, 250ms);
    EXPECT_LT(took, 2s);
  }
  // The connection still works after a timeout.
  EXPECT_TRUE(clipboard->alive());
  ASSERT_TRUE(clipboard->set("after timeout"));
  EXPECT_EQ(get_until("after timeout"), "after timeout");
}

// Bugs caught: 0, False positives: 0
TEST_P(ClipboardLiveTest, PlatformFunctionsUseTheSessionClipboard) {
  if (GetParam() != protocol_e::any) {
    GTEST_SKIP() << "platf:: always picks the protocol itself";
  }
  // platf:: connects to WAYLAND_DISPLAY, which SetUp checked is the private instance.
  ASSERT_TRUE(platf::set_clipboard("from platf\r\nline"));
  EXPECT_EQ(get_until("from platf\nline"), "from platf\nline");
  ASSERT_TRUE(clipboard->set("from test"));
  auto deadline = std::chrono::steady_clock::now() + 5s;
  std::string seen;
  do {
    seen = platf::get_clipboard();
    if (seen != "from test") {
      std::this_thread::sleep_for(50ms);
    }
  } while (seen != "from test" && std::chrono::steady_clock::now() < deadline);
  EXPECT_EQ(seen, "from test");
}

#endif
