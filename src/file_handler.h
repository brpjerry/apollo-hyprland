/**
 * @file file_handler.h
 * @brief Declarations for file handling functions.
 */
#pragma once

// standard includes
#include <cstdint>
#include <string>
#include <string_view>

/**
 * @brief Responsible for file handling functions.
 */
namespace file_handler {
  /**
   * @brief Get the parent directory of a file or directory.
   * @param path The path of the file or directory.
   * @return The parent directory.
   * @examples
   * std::string parent_dir = get_parent_directory("path/to/file");
   * @examples_end
   */
  std::string get_parent_directory(const std::string &path);

  /**
   * @brief Make a directory.
   * @param path The path of the directory.
   * @return `true` on success, `false` on failure.
   * @examples
   * bool dir_created = make_directory("path/to/directory");
   * @examples_end
   */
  bool make_directory(const std::string &path);

  /**
   * @brief Read a file to string.
   * @param path The path of the file.
   * @return The contents of the file.
   * @examples
   * std::string contents = read_file("path/to/file");
   * @examples_end
   */
  std::string read_file(const char *path);

  /**
   * @brief Writes a file.
   *
   * On Linux the contents go to a temporary sibling that replaces the file once it is complete, so a failed write
   * leaves the previous file as it was. The file keeps its owner, group and mode; a symlink keeps pointing at the new
   * contents. A file that a replacement would change otherwise (other hard links, an access ACL, an owner or group we
   * can't set, a bind mount, a directory we can't create files in) is overwritten in place instead.
   * @param path The path of the file.
   * @param contents The contents to write.
   * @return ``0`` on success, ``-1`` on failure.
   * @examples
   * int write_status = write_file("path/to/file", "file contents");
   * @examples_end
   */
  int write_file(const char *path, const std::string_view &contents);

#ifndef _WIN32
  /**
   * @brief Building blocks of write_file(), exposed for tests.
   */
  namespace detail {
    /**
     * @brief Whether a probe for a file's access ACL (getxattr of `system.posix_acl_access`) proves it has none.
     *
     * @param probe_result Return value of the probe.
     * @param probe_errno errno after a failed probe.
     * @return `true` only for "no such attribute" or "no ACL support"; another error proves nothing.
     */
    bool access_acl_absent(long probe_result, int probe_errno);

    /**
     * @brief Overwrite a regular file in place if its device and inode numbers are still the ones inspected.
     *
     * @param path File to overwrite.
     * @param contents New contents.
     * @param device Device of the file inspected earlier.
     * @param inode Inode of the file inspected earlier.
     * @return ``0`` on success, ``-1`` on failure. A file of another type or identity is detected before truncating.
     */
    int write_in_place(const char *path, std::string_view contents, std::uint64_t device, std::uint64_t inode);
  }  // namespace detail
#endif
}  // namespace file_handler
