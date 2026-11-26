/**
 * @file src/platform/linux/input/virtual_keyboard.cpp
 * @brief Definitions for Unicode text input through zwp_virtual_keyboard_v1.
 */
// local includes
#include "virtual_keyboard.h"

#include "src/logging.h"
#include "unicode_keymap.h"

// standard includes
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>

// platform includes
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

// lib includes
#include <virtual-keyboard-unstable-v1.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

using namespace std::literals;

namespace platf::keyboard {
  namespace {
    std::string default_keymap() {
      auto ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
      if (!ctx) {
        return {};
      }
      std::string text;
      if (auto keymap = xkb_keymap_new_from_names(ctx, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS)) {
        if (char *s = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1)) {
          text = s;
          std::free(s);
        }
        xkb_keymap_unref(keymap);
      }
      xkb_context_unref(ctx);
      return text;
    }

    std::uint32_t now_ms() {
      using namespace std::chrono;
      return static_cast<std::uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
    }
  }  // namespace

  struct virtual_keyboard_t::impl_t {
    wl_display *display = nullptr;
    wl_seat *seat = nullptr;
    wl_keyboard *keyboard = nullptr;
    zwp_virtual_keyboard_manager_v1 *manager = nullptr;
    zwp_virtual_keyboard_v1 *vk = nullptr;

    mutable std::mutex lock;
    std::string restore;  ///< The seat's normal keymap
    std::size_t uploads = 0;
    bool broken = false;

    ~impl_t() {
      if (vk) {
        zwp_virtual_keyboard_v1_destroy(vk);
      }
      if (keyboard) {
        if (wl_keyboard_get_version(keyboard) >= WL_KEYBOARD_RELEASE_SINCE_VERSION) {
          wl_keyboard_release(keyboard);
        } else {
          wl_keyboard_destroy(keyboard);
        }
      }
      if (manager) {
        zwp_virtual_keyboard_manager_v1_destroy(manager);
      }
      if (seat) {
        if (wl_seat_get_version(seat) >= WL_SEAT_RELEASE_SINCE_VERSION) {
          wl_seat_release(seat);
        } else {
          wl_seat_destroy(seat);
        }
      }
      if (display) {
        wl_display_flush(display);
        wl_display_disconnect(display);
      }
    }

    // The compositor sends the seat's keymap to every wl_keyboard; the latest one that is not ours is what we restore.
    static void on_keymap(void *data, wl_keyboard *, uint32_t format, int32_t fd, uint32_t size) {
      auto self = static_cast<impl_t *>(data);
      if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && size > 1) {
        void *map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (map != MAP_FAILED) {
          std::string text(static_cast<const char *>(map), strnlen(static_cast<const char *>(map), size));
          munmap(map, size);
          if (!text.empty() && !unicode_keymap::is_ours(text)) {
            self->restore = std::move(text);
          }
        }
      }
      close(fd);
    }

    static constexpr wl_keyboard_listener keyboard_listener {
      .keymap = on_keymap,
      .enter = [](void *, wl_keyboard *, uint32_t, wl_surface *, wl_array *) {},
      .leave = [](void *, wl_keyboard *, uint32_t, wl_surface *) {},
      .key = [](void *, wl_keyboard *, uint32_t, uint32_t, uint32_t, uint32_t) {},
      .modifiers = [](void *, wl_keyboard *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {},
      .repeat_info = [](void *, wl_keyboard *, int32_t, int32_t) {},
    };

    static constexpr wl_seat_listener seat_listener {
      .capabilities = [](void *data, wl_seat *seat, uint32_t caps) {
        auto self = static_cast<impl_t *>(data);
        if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !self->keyboard) {
          self->keyboard = wl_seat_get_keyboard(seat);
          wl_keyboard_add_listener(self->keyboard, &keyboard_listener, self);
        }
      },
      .name = [](void *, wl_seat *, const char *) {},
    };

    static constexpr wl_registry_listener registry_listener {
      .global = [](void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
        auto self = static_cast<impl_t *>(data);
        if (std::string_view {interface} == wl_seat_interface.name && !self->seat) {
          self->seat = static_cast<wl_seat *>(wl_registry_bind(registry, name, &wl_seat_interface, std::min<uint32_t>(version, 5)));
          wl_seat_add_listener(self->seat, &seat_listener, self);
        } else if (std::string_view {interface} == zwp_virtual_keyboard_manager_v1_interface.name && !self->manager) {
          self->manager = static_cast<zwp_virtual_keyboard_manager_v1 *>(wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1));
        }
      },
      .global_remove = [](void *, wl_registry *, uint32_t) {},
    };

    bool check() {
      if (!broken && wl_display_get_error(display) != 0) {
        BOOST_LOG(warning) << "Virtual keyboard: lost the Wayland connection: "sv << std::strerror(wl_display_get_error(display));
        broken = true;
      }
      return !broken;
    }

    bool upload(const std::string &text) {
      int fd = memfd_create("apollo-keymap", MFD_CLOEXEC | MFD_ALLOW_SEALING);
      if (fd < 0) {
        BOOST_LOG(warning) << "Virtual keyboard: memfd_create failed: "sv << std::strerror(errno);
        return false;
      }
      // The size includes the terminating NUL, as for wl_keyboard.keymap.
      const char *p = text.c_str();
      std::size_t left = text.size() + 1;
      while (left > 0) {
        auto n = write(fd, p, left);
        if (n < 0 && errno == EINTR) {
          continue;
        }
        if (n <= 0) {
          BOOST_LOG(warning) << "Virtual keyboard: writing the keymap failed: "sv << std::strerror(errno);
          close(fd);
          return false;
        }
        p += n;
        left -= static_cast<std::size_t>(n);
      }
      fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);
      zwp_virtual_keyboard_v1_keymap(vk, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, static_cast<uint32_t>(text.size() + 1));
      close(fd);  // libwayland has duplicated it
      ++uploads;
      return true;
    }
  };

  std::unique_ptr<virtual_keyboard_t> virtual_keyboard_t::connect(const char *display_name) {
    auto impl = std::make_unique<impl_t>();
    impl->display = wl_display_connect(display_name);
    if (!impl->display) {
      BOOST_LOG(debug) << "Virtual keyboard: no Wayland display"sv;
      return nullptr;
    }
    auto registry = wl_display_get_registry(impl->display);
    wl_registry_add_listener(registry, &impl_t::registry_listener, impl.get());
    wl_display_roundtrip(impl->display);
    wl_registry_destroy(registry);
    if (!impl->seat || !impl->manager) {
      BOOST_LOG(info) << "Virtual keyboard: the compositor has no "sv << (impl->seat ? "zwp_virtual_keyboard_manager_v1"sv : "seat"sv);
      return nullptr;
    }
    // Seat capabilities, then the seat's current keymap.
    wl_display_roundtrip(impl->display);
    wl_display_roundtrip(impl->display);
    if (impl->restore.empty()) {
      impl->restore = default_keymap();
    }

    impl->vk = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(impl->manager, impl->seat);
    // A keyboard needs a keymap before its first key; start with the normal one.
    if (impl->restore.empty() || !impl->upload(impl->restore)) {
      BOOST_LOG(warning) << "Virtual keyboard: no keymap to start with"sv;
      return nullptr;
    }
    if (wl_display_roundtrip(impl->display) < 0 || !impl->check()) {
      BOOST_LOG(warning) << "Virtual keyboard: the compositor refused the virtual keyboard"sv;
      return nullptr;
    }
    BOOST_LOG(info) << "Virtual keyboard: typing Unicode text through zwp_virtual_keyboard_v1"sv;
    return std::unique_ptr<virtual_keyboard_t>(new virtual_keyboard_t(std::move(impl)));
  }

  virtual_keyboard_t::virtual_keyboard_t(std::unique_ptr<impl_t> impl):
      impl(std::move(impl)) {
  }

  virtual_keyboard_t::~virtual_keyboard_t() = default;

  bool virtual_keyboard_t::type(std::u32string_view text) {
    std::lock_guard lg {impl->lock};
    if (!impl->check()) {
      return false;
    }
    // Pick up keymap changes the compositor sent since the last call.
    if (wl_display_roundtrip(impl->display) < 0 || !impl->check()) {
      return false;
    }

    auto batches = unicode_keymap::plan(text);
    if (batches.empty()) {
      return true;
    }
    bool complete = true;
    for (const auto &batch : batches) {
      if (!impl->upload(unicode_keymap::keymap_text(batch.keysyms))) {
        complete = false;
        break;
      }
      for (auto key : batch.keys) {
        auto t = now_ms();
        zwp_virtual_keyboard_v1_key(impl->vk, t, key, WL_KEYBOARD_KEY_STATE_PRESSED);
        zwp_virtual_keyboard_v1_key(impl->vk, t, key, WL_KEYBOARD_KEY_STATE_RELEASED);
      }
    }
    // Every key is up again, so the normal keymap can come back.
    const bool restored = impl->upload(impl->restore);
    if (wl_display_roundtrip(impl->display) < 0) {
      impl->check();
      return false;
    }
    // A failed upload means only part of the text was typed, or the temporary keymap is still in place: report it,
    // so the caller drops this keyboard (and its keymap) instead of typing on with it.
    return impl->check() && complete && restored;
  }

  bool virtual_keyboard_t::alive() const {
    std::lock_guard lg {impl->lock};
    return !impl->broken && wl_display_get_error(impl->display) == 0;
  }

  std::size_t virtual_keyboard_t::keymap_uploads() const {
    std::lock_guard lg {impl->lock};
    return impl->uploads;
  }

  std::string virtual_keyboard_t::restore_keymap() const {
    std::lock_guard lg {impl->lock};
    return impl->restore;
  }
}  // namespace platf::keyboard
