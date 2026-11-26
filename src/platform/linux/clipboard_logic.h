/**
 * @file src/platform/linux/clipboard_logic.h
 * @brief Compositor-independent parts of the Wayland clipboard: MIME selection, line endings and pipe transfers.
 */
#pragma once

// standard includes
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <ctime>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// platform includes
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

namespace platf::clipboard {
  /**
   * @brief MIME types offered when Apollo owns the selection, most specific first.
   * @details The X11 names are there for XWayland clients, which map them to their selection targets.
   */
  inline constexpr std::array<std::string_view, 5> offered_text_mimes {
    "text/plain;charset=utf-8",
    "text/plain",
    "UTF8_STRING",
    "TEXT",
    "STRING",
  };

  /**
   * @brief Lower-cases a MIME type and drops the spaces around parameters.
   * @details "Text/Plain; charset=UTF-8" becomes "text/plain;charset=utf-8".
   */
  inline std::string normalize_mime(std::string_view mime) {
    std::string result;
    result.reserve(mime.size());
    for (char c : mime) {
      if (c == ' ' || c == '\t') {
        continue;
      }
      result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return result;
  }

  /**
   * @brief Picks the MIME type to read text in from what the selection owner offers.
   * @details Preference: text/plain;charset=utf-8 (any case, also with a quoted charset), UTF8_STRING, text/plain.
   * @param offered MIME types in the order the owner announced them.
   * @return The offered spelling of the chosen type, or std::nullopt when the selection holds no text.
   */
  inline std::optional<std::string> choose_text_mime(const std::vector<std::string> &offered) {
    static constexpr std::array<std::string_view, 4> preference {
      "text/plain;charset=utf-8",
      "text/plain;charset=\"utf-8\"",
      "utf8_string",
      "text/plain",
    };
    // UTF8_STRING is case sensitive in X11, but no other type normalizes to it, so compare normalized forms.
    std::size_t best_rank = preference.size();
    const std::string *best = nullptr;
    for (const auto &mime : offered) {
      auto normalized = normalize_mime(mime);
      auto it = std::find(preference.begin(), preference.end(), normalized);
      if (it == preference.end()) {
        continue;
      }
      auto rank = static_cast<std::size_t>(it - preference.begin());
      // Both spellings of the UTF-8 charset rank the same.
      if (rank == 1) {
        rank = 0;
      }
      if (rank < best_rank) {
        best_rank = rank;
        best = &mime;
      }
    }
    if (!best) {
      return std::nullopt;
    }
    return *best;
  }

  /**
   * @brief Converts CRLF line endings to LF. Lone CRs are kept.
   * @details The reverse of ensureCrLf in the Windows clipboard code: clients on Windows send CRLF text.
   */
  inline std::string crlf_to_lf(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
        continue;
      }
      result += text[i];
    }
    return result;
  }

  /**
   * @brief Result of one step of a pipe transfer.
   */
  enum class io_status_e {
    again,  ///< The pipe would block; wait for it to become ready and call again.
    done,  ///< Reader: end of file. Writer: everything was written.
    error,  ///< The transfer failed (the other end went away, or an I/O error).
    too_large,  ///< Reader: the data exceeds the size limit.
  };

  /**
   * @brief Reads a whole pipe in chunks from a non-blocking descriptor.
   */
  struct pipe_reader_t {
    std::string data;  ///< Everything read so far.
    std::size_t limit;  ///< Largest accepted size in bytes.

    explicit pipe_reader_t(std::size_t limit):
        limit {limit} {}

    /**
     * @brief Reads what is available now.
     * @param fd A non-blocking descriptor.
     */
    io_status_e read_some(int fd) {
      std::array<char, 64 * 1024> buffer;
      while (true) {
        auto n = ::read(fd, buffer.data(), buffer.size());
        if (n > 0) {
          if (data.size() + static_cast<std::size_t>(n) > limit) {
            return io_status_e::too_large;
          }
          data.append(buffer.data(), static_cast<std::size_t>(n));
          continue;
        }
        if (n == 0) {
          return io_status_e::done;
        }
        if (errno == EINTR) {
          continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          return io_status_e::again;
        }
        return io_status_e::error;
      }
    }
  };

  /**
   * @brief write(2) that reports EPIPE instead of raising SIGPIPE in the calling thread.
   * @details SIGPIPE is blocked around the write and a SIGPIPE it raised is consumed, so a paste target that closes
   *          its end early cannot kill Apollo.
   */
  inline ssize_t write_no_sigpipe(int fd, const void *buffer, std::size_t size) {
    sigset_t pipe_set;
    sigset_t old_set;
    sigemptyset(&pipe_set);
    sigaddset(&pipe_set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipe_set, &old_set);

    // A SIGPIPE that was already pending is not ours to consume.
    sigset_t pending;
    sigpending(&pending);
    bool was_pending = sigismember(&pending, SIGPIPE);

    auto n = ::write(fd, buffer, size);
    auto saved_errno = errno;

    if (n < 0 && saved_errno == EPIPE && !was_pending) {
      timespec zero {};
      while (sigtimedwait(&pipe_set, nullptr, &zero) < 0 && errno == EINTR) {}
    }
    pthread_sigmask(SIG_SETMASK, &old_set, nullptr);
    errno = saved_errno;
    return n;
  }

  /**
   * @brief Writes a buffer to a non-blocking descriptor in chunks.
   */
  struct pipe_writer_t {
    std::shared_ptr<const std::string> data;  ///< The text being served; shared by every transfer of one source.
    std::size_t offset = 0;  ///< Bytes written so far.

    explicit pipe_writer_t(std::shared_ptr<const std::string> data):
        data {std::move(data)} {}

    /**
     * @brief Writes as much as the pipe takes now.
     * @param fd A non-blocking descriptor.
     */
    io_status_e write_some(int fd) {
      while (offset < data->size()) {
        auto n = write_no_sigpipe(fd, data->data() + offset, data->size() - offset);
        if (n > 0) {
          offset += static_cast<std::size_t>(n);
          continue;
        }
        if (n < 0 && errno == EINTR) {
          continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
          return io_status_e::again;
        }
        return io_status_e::error;
      }
      return io_status_e::done;
    }
  };
}  // namespace platf::clipboard
