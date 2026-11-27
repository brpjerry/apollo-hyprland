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
  #include <sys/xattr.h>
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

#ifndef _WIN32
  namespace {
    constexpr auto access_acl_attribute = "system.posix_acl_access";  ///< POSIX access ACL of a file.

    /**
     * @brief Write a whole buffer to a file descriptor.
     *
     * @return 0, or the errno of the failure.
     */
    int write_all(int fd, const std::string_view contents) {
      for (std::size_t written = 0; written < contents.size();) {
        const auto n = ::write(fd, contents.data() + written, contents.size() - written);
        if (n > 0) {
          written += static_cast<std::size_t>(n);
        } else if (n == 0) {
          return EIO;
        } else if (errno != EINTR) {
          return errno;
        }
      }
      return 0;
    }

    /**
     * @brief Overwrite an existing file in place, keeping its inode (hard links, owner, group, ACLs).
     *
     * Not atomic: used only where replacing the file would change it in some other way. Before truncating, the opened
     * file must be a regular file with the device and inode numbers inspected earlier, so a file put in its place
     * meanwhile (by an editor or a configuration manager) is left alone; only a new file that reuses the inode in
     * between can't be told apart. Opening a FIFO doesn't block.
     *
     * @param target File to overwrite.
     * @param path Path to name in log messages.
     * @param contents New contents.
     * @param device Device of the file inspected earlier.
     * @param inode Inode of the file inspected earlier.
     * @return 0 on success, -1 on failure.
     */
    int write_in_place(const std::filesystem::path &target, const char *path, const std::string_view contents, const dev_t device, const ino_t inode) {
      const int fd = ::open(target.c_str(), O_WRONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
      if (fd < 0) {
        BOOST_LOG(error) << "Couldn't write "sv << path << ": "sv << std::strerror(errno);
        return -1;
      }
      struct stat opened {};
      int failure = 0;
      if (::fstat(fd, &opened) != 0) {
        failure = errno;
      } else if (!S_ISREG(opened.st_mode) || opened.st_dev != device || opened.st_ino != inode) {
        BOOST_LOG(error) << "Couldn't write "sv << path << ": it was replaced while being saved"sv;
        ::close(fd);
        return -1;
      } else if (::ftruncate(fd, 0) != 0) {
        failure = errno;
      } else {
        failure = write_all(fd, contents);
      }
      if (!failure && ::fsync(fd) != 0) {
        failure = errno;
      }
      if (::close(fd) != 0 && !failure) {
        failure = errno;
      }
      if (failure) {
        BOOST_LOG(error) << "Couldn't write "sv << path << ": "sv << std::strerror(failure);
        return -1;
      }
      return 0;
    }

    /**
     * @brief Whether a file may have a POSIX access ACL, which a replacement would not carry.
     *
     * @param target File to check.
     * @return `false` only when the file is known to have no access ACL.
     */
    bool may_have_access_acl(const std::filesystem::path &target) {
      const auto result = ::getxattr(target.c_str(), access_acl_attribute, nullptr, 0);
      return !detail::access_acl_absent(result, result < 0 ? errno : 0);
    }
  }  // namespace

  namespace detail {
    bool access_acl_absent(const long probe_result, const int probe_errno) {
      // Only "no such attribute" and "no ACL support" prove it; an access or I/O error proves nothing.
      return probe_result < 0 && (probe_errno == ENODATA || probe_errno == ENOTSUP || probe_errno == EOPNOTSUPP);
    }

    int write_in_place(const char *path, const std::string_view contents, const std::uint64_t device, const std::uint64_t inode) {
      return file_handler::write_in_place(path, path, contents, static_cast<dev_t>(device), static_cast<ino_t>(inode));
    }
  }  // namespace detail
#endif

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

    // Write a sibling file and rename it over the target, so a full disk or a crash midway leaves the old file intact
    // rather than truncated. Write through a symlink to the file it points at; a dangling link is an error, since
    // renaming over it would replace the link itself.
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
      // A replacement would leave other hard links with the old contents, and wouldn't carry an access ACL.
      if (existing.st_nlink > 1 || may_have_access_acl(target)) {
        return write_in_place(target, path, contents, existing.st_dev, existing.st_ino);
      }
    } else if (errno != ENOENT) {
      BOOST_LOG(error) << "Couldn't inspect "sv << path << ": "sv << std::strerror(errno);
      return -1;
    }

    static std::atomic<unsigned> counter {0};
    auto temporary = target;
    temporary += std::format(".tmp-{}-{}", ::getpid(), counter++);

    // A replacement starts private and gets the old file's owner, group and mode once it is ours, so nobody can open
    // it in between with broader access than the file it replaces. A new file gets the usual umask-based mode.
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, replacing ? 0600 : 0666);
    if (fd < 0) {
      // E.g. a writable file in a directory we can't create files in: write it in place, as before.
      if (replacing && (errno == EACCES || errno == EPERM)) {
        return write_in_place(target, path, contents, existing.st_dev, existing.st_ino);
      }
      BOOST_LOG(error) << "Couldn't write "sv << path << ": "sv << std::strerror(errno);
      return -1;
    }
    auto discard = [&]() {
      ::close(fd);
      ::unlink(temporary.c_str());
    };

    int failure = 0;
    if (replacing) {
      // Who may access the file must not change: drop an ACL the directory's default ACL gave the new file, and keep
      // the owner and group. When that isn't possible (another owner, a group we're not in), write in place.
      struct stat staged {};
      const bool metadata_kept =
        (::fremovexattr(fd, access_acl_attribute) == 0 || errno == ENODATA || errno == EOPNOTSUPP) &&
        ::fstat(fd, &staged) == 0 &&
        ((staged.st_uid == existing.st_uid && staged.st_gid == existing.st_gid) || ::fchown(fd, existing.st_uid, existing.st_gid) == 0) &&
        ::fchmod(fd, existing.st_mode & 07777) == 0;
      if (!metadata_kept) {
        discard();
        return write_in_place(target, path, contents, existing.st_dev, existing.st_ino);
      }
    }
    failure = write_all(fd, contents);
    if (!failure && ::fsync(fd) != 0) {
      failure = errno;
    }
    if (::close(fd) != 0 && !failure) {
      failure = errno;
    }
    if (failure) {
      BOOST_LOG(error) << "Couldn't write "sv << path << ": "sv << std::strerror(failure);
      ::unlink(temporary.c_str());
      return -1;
    }
    if (::rename(temporary.c_str(), target.c_str()) == 0) {
      return 0;
    }

    const int rename_errno = errno;
    ::unlink(temporary.c_str());
    // The file may be a bind mount (a container's config file), which can be written but not replaced.
    if (replacing && (rename_errno == EBUSY || rename_errno == EXDEV || rename_errno == EPERM || rename_errno == EACCES)) {
      return write_in_place(target, path, contents, existing.st_dev, existing.st_ino);
    }
    BOOST_LOG(error) << "Couldn't write "sv << path << ": "sv << std::strerror(rename_errno);
    return -1;
#endif
  }
}  // namespace file_handler
