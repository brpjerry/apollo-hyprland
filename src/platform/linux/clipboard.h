/**
 * @file src/platform/linux/clipboard.h
 * @brief Wayland clipboard through ext-data-control-v1, or zwlr_data_control_manager_v1 as a fallback.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace platf::clipboard {
  /**
   * @brief Which data-control protocol to use.
   */
  enum class protocol_e {
    any,  ///< ext-data-control-v1 if the compositor has it, otherwise wlr-data-control.
    ext,  ///< Only ext-data-control-v1.
    wlr,  ///< Only zwlr_data_control_manager_v1.
  };

  struct options_t {
    protocol_e protocol = protocol_e::any;
    std::chrono::milliseconds read_timeout {3000};  ///< How long get() waits for the selection owner's data.
    std::chrono::milliseconds write_timeout {30000};  ///< How long a paste target may take to read our text.
    std::size_t max_size = 64 * 1024 * 1024;  ///< Larger selections are not read.
  };

  /**
   * @brief A persistent connection to the compositor's clipboard.
   * @details The connection runs its own event thread, which receives selection changes, serves paste requests
   *          for the text set with set() until another client takes the selection, and moves data through pipes
   *          without blocking. get() and set() may be called from any thread.
   */
  class wayland_clipboard_t {
  public:
    /**
     * @brief Connects to a compositor.
     * @param display_name Wayland display to connect to; nullptr uses WAYLAND_DISPLAY.
     * @return nullptr when there is no compositor or it has neither data-control protocol.
     */
    static std::unique_ptr<wayland_clipboard_t> connect(const char *display_name, const options_t &options = {});

    ~wayland_clipboard_t();
    wayland_clipboard_t(const wayland_clipboard_t &) = delete;
    wayland_clipboard_t &operator=(const wayland_clipboard_t &) = delete;

    /**
     * @brief Reads the selection as UTF-8 text.
     * @return The text (empty when the selection is empty or holds no text), or std::nullopt when reading failed
     *         or timed out.
     */
    std::optional<std::string> get();

    /**
     * @brief Takes the selection and serves @p text (CRLF converted to LF) until another client replaces it.
     * @return false if the connection is broken.
     */
    bool set(std::string_view text);

    /**
     * @brief False once the connection to the compositor failed; reconnect with connect().
     */
    bool alive() const;

    /**
     * @brief The protocol in use: "ext-data-control-v1" or "wlr-data-control-unstable-v1".
     */
    std::string_view protocol() const;

    /**
     * @brief Number of selection sources this connection still serves (for tests).
     */
    std::size_t active_sources();

    struct impl_t;

  private:
    explicit wayland_clipboard_t(std::unique_ptr<impl_t> impl);
    std::unique_ptr<impl_t> impl;
  };
}  // namespace platf::clipboard
