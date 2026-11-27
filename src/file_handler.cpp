/**
 * @file file_handler.cpp
 * @brief Definitions for file handling functions.
 */

// standard includes
#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>

#ifndef _WIN32
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

// local includes
#include "file_handler.h"
#include "logging.h"

namespace file_handler {
  using namespace std::literals;

  std::string get_parent_directory(const std::string &path) {
    // remove any trailing path separators
    std::string trimmed_path = path;
    while (!trimmed_path.empty() && trimmed_path.back() == '/') {
      trimmed_path.pop_back();
    }

    std::filesystem::path p(trimmed_path);
    return p.parent_path().string();
  }

  bool make_directory(const std::string &path) {
    // first, check if the directory already exists
    if (std::filesystem::exists(path)) {
      return true;
    }

    return std::filesystem::create_directories(path);
  }

  std::string read_file(const char *path) {
    if (!std::filesystem::exists(path)) {
      BOOST_LOG(debug) << "Missing file: " << path;
      return {};
    }

    std::ifstream in(path);
    return std::string {(std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()};
  }

  int write_file(const char *path, const std::string_view &contents) {
#ifdef _WIN32
    std::ofstream out(path);

    if (!out.is_open()) {
      return -1;
    }

    out << contents;
    out.close();
    return out ? 0 : -1;
#else
    namespace fs = std::filesystem;

    // Write a sibling file and rename it over the target, so a full disk, a read-only directory or a crash midway
    // leaves the old file intact rather than truncated. Write through a symlink to the file it points at; a dangling
    // link is an error, since renaming over it would replace the link itself.
    std::error_code ec;
    fs::path target {path};
    if (fs::is_symlink(target, ec)) {
      target = fs::canonical(target, ec);
      if (ec) {
        BOOST_LOG(error) << "Couldn't resolve "sv << path << ": "sv << ec.message();
        return -1;
      }
    }

    // Only replace what could be written in place: a read-only file stays read-only to us even though its directory
    // would allow the rename, and a FIFO or device must not turn into a regular file.
    struct stat existing {};
    const bool replacing = ::stat(target.c_str(), &existing) == 0;
    if (replacing) {
      if (!S_ISREG(existing.st_mode) || ::faccessat(AT_FDCWD, target.c_str(), W_OK, AT_EACCESS) != 0) {
        BOOST_LOG(error) << "Couldn't write "sv << path << ": not a writable regular file"sv;
        return -1;
      }
    } else if (errno != ENOENT) {
      BOOST_LOG(error) << "Couldn't inspect "sv << path << ": "sv << std::strerror(errno);
      return -1;
    }

    static std::atomic<unsigned> counter {0};
    auto temporary = target;
    temporary += std::format(".tmp-{}-{}", ::getpid(), counter++);

    // A replacement starts private and gets the old file's permissions once it is ours (fchmod below), so nobody can
    // open it in between with broader access than the file it replaces. A new file gets the usual umask-based mode.
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, replacing ? 0600 : 0666);
    if (fd < 0) {
      BOOST_LOG(error) << "Couldn't write "sv << path << ": "sv << std::strerror(errno);
      return -1;
    }

    int failure = 0;
    // Keep the permissions of the file being replaced.
    if (replacing && ::fchmod(fd, existing.st_mode & 07777) != 0) {
      failure = errno;
    }
    for (std::size_t written = 0; !failure && written < contents.size();) {
      const auto n = ::write(fd, contents.data() + written, contents.size() - written);
      if (n > 0) {
        written += static_cast<std::size_t>(n);
      } else if (n < 0 && errno != EINTR) {
        failure = errno;
      } else if (n == 0) {
        failure = EIO;
      }
    }
    if (!failure && ::fsync(fd) != 0) {
      failure = errno;
    }
    if (::close(fd) != 0 && !failure) {
      failure = errno;
    }
    if (!failure && ::rename(temporary.c_str(), target.c_str()) == 0) {
      return 0;
    }

    BOOST_LOG(error) << "Couldn't write "sv << path << ": "sv << std::strerror(failure ? failure : errno);
    ::unlink(temporary.c_str());
    return -1;
#endif
  }
}  // namespace file_handler
