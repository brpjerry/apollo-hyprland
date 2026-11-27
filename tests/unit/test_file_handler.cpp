/**
 * @file tests/unit/test_file_handler.cpp
 * @brief Test src/file_handler.*.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <format>

// local includes
#include <src/file_handler.h>

struct FileHandlerParentDirectoryTest: BaseTest, testing::WithParamInterface<std::tuple<std::string, std::string>> {};

TEST_P(FileHandlerParentDirectoryTest, Run) {
  auto [input, expected] = GetParam();
  EXPECT_EQ(file_handler::get_parent_directory(input), expected);
}

INSTANTIATE_TEST_SUITE_P(
  FileHandlerTests,
  FileHandlerParentDirectoryTest,
  testing::Values(
    std::make_tuple("/path/to/file.txt", "/path/to"),
    std::make_tuple("/path/to/directory", "/path/to"),
    std::make_tuple("/path/to/directory/", "/path/to")
  )
);

struct FileHandlerMakeDirectoryTest: BaseTest, testing::WithParamInterface<std::tuple<std::string, bool, bool>> {};

TEST_P(FileHandlerMakeDirectoryTest, Run) {
  auto [input, expected, remove] = GetParam();
  const std::string test_dir = platf::appdata().string() + "/tests/path/";
  input = test_dir + input;

  EXPECT_EQ(file_handler::make_directory(input), expected);
  EXPECT_TRUE(std::filesystem::exists(input));

  // remove test directory
  if (remove) {
    std::filesystem::remove_all(test_dir);
    EXPECT_FALSE(std::filesystem::exists(test_dir));
  }
}

INSTANTIATE_TEST_SUITE_P(
  FileHandlerTests,
  FileHandlerMakeDirectoryTest,
  testing::Values(
    std::make_tuple("dir_123", true, false),
    std::make_tuple("dir_123", true, true),
    std::make_tuple("dir_123/abc", true, false),
    std::make_tuple("dir_123/abc", true, true)
  )
);

struct FileHandlerTests: BaseTest, testing::WithParamInterface<std::tuple<int, std::string>> {};

INSTANTIATE_TEST_SUITE_P(
  TestFiles,
  FileHandlerTests,
  testing::Values(
    std::make_tuple(0, ""),  // empty file
    std::make_tuple(1, "a"),  // single character
    std::make_tuple(2, "Mr. Blue Sky - Electric Light Orchestra"),  // single line
    std::make_tuple(3, R"(
Morning! Today's forecast calls for blue skies
The sun is shining in the sky
There ain't a cloud in sight
It's stopped raining
Everybody's in the play
And don't you know, it's a beautiful new day
Hey, hey, hey!
Running down the avenue
See how the sun shines brightly in the city
All the streets where once was pity
Mr. Blue Sky is living here today!
Hey, hey, hey!
    )")  // multi-line
  )
);

TEST_P(FileHandlerTests, WriteFileTest) {
  auto [fileNum, content] = GetParam();
  const std::string fileName = std::format("write_file_test_{}.txt", fileNum);
  EXPECT_EQ(file_handler::write_file(fileName.c_str(), content), 0);
}

TEST_P(FileHandlerTests, ReadFileTest) {
  auto [fileNum, content] = GetParam();
  const std::string fileName = std::format("write_file_test_{}.txt", fileNum);
  EXPECT_EQ(file_handler::read_file(fileName.c_str()), content);
}

TEST(FileHandlerTests, ReadMissingFileTest) {
  // read missing file
  EXPECT_EQ(file_handler::read_file("non-existing-file.txt"), "");
}

#ifndef _WIN32
  // standard includes
  #include <cerrno>
  #include <chrono>
  #include <cstdlib>
  #include <filesystem>
  #include <fstream>
  #include <future>
  #include <string>
  #include <string_view>
  #include <sys/stat.h>
  #include <sys/xattr.h>
  #include <unistd.h>
  #include <vector>

  // local includes
  #include <src/compat/boost_process.h>

namespace {
  /**
   * @brief Read a file's access ACL attribute.
   *
   * @param file File to read.
   * @return The raw attribute, or an empty string when the file has none.
   */
  std::string access_acl_of(const std::filesystem::path &file) {
    // Ask for the size first: an inherited default ACL can make the attribute longer than any fixed buffer.
    const auto needed = ::getxattr(file.c_str(), "system.posix_acl_access", nullptr, 0);
    if (needed <= 0) {
      return {};
    }
    std::string value(static_cast<std::size_t>(needed), '\0');
    const auto size = ::getxattr(file.c_str(), "system.posix_acl_access", value.data(), value.size());
    value.resize(size < 0 ? 0 : static_cast<std::size_t>(size));
    return value;
  }
}  // namespace

// Bugs caught: 0, False positives: 0
TEST(FileHandlerTests, WriteFileReplacesAtomicallyAndKeepsPermissionsAndSymlinks) {
  namespace fs = std::filesystem;
  const auto dir = fs::path {SUNSHINE_TEST_BIN_DIR} / "write_file_atomic";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const auto target = dir / "apps.json";

  ASSERT_EQ(file_handler::write_file(target.c_str(), "old"), 0);
  fs::permissions(target, fs::perms::owner_read | fs::perms::owner_write);
  ASSERT_EQ(file_handler::write_file(target.c_str(), "new"), 0);
  EXPECT_EQ(file_handler::read_file(target.c_str()), "new");
  EXPECT_EQ(fs::status(target).permissions() & fs::perms::all, fs::perms::owner_read | fs::perms::owner_write);

  // A symlinked file (e.g. kept in a dotfiles repository) stays a symlink.
  const auto link = dir / "link.json";
  fs::create_symlink(target.filename(), link);
  ASSERT_EQ(file_handler::write_file(link.c_str(), "through link"), 0);
  EXPECT_TRUE(fs::is_symlink(link));
  EXPECT_EQ(file_handler::read_file(target.c_str()), "through link");

  // A writable file in a directory we can't create files in is written in place, as the in-place writer did.
  if (::geteuid() != 0) {
    fs::permissions(dir, fs::perms::owner_read | fs::perms::owner_exec);
    EXPECT_EQ(file_handler::write_file(target.c_str(), "in place"), 0);
    EXPECT_EQ(file_handler::read_file(target.c_str()), "in place");
    fs::permissions(dir, fs::perms::owner_all);
  }
  std::error_code ec;
  fs::remove_all(dir, ec);
}

// Bugs caught: 0, False positives: 0
TEST(FileHandlerTests, WriteFileOnlyReplacesWhatItCouldWriteInPlace) {
  namespace fs = std::filesystem;
  const auto dir = fs::path {SUNSHINE_TEST_BIN_DIR} / "write_file_refusals";
  fs::remove_all(dir);
  fs::create_directories(dir);

  // A read-only file in a writable directory stays as it is (root may write it anyway).
  const auto read_only = dir / "managed.conf";
  ASSERT_EQ(file_handler::write_file(read_only.c_str(), "managed"), 0);
  fs::permissions(read_only, fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read);
  if (::geteuid() != 0) {
    EXPECT_NE(file_handler::write_file(read_only.c_str(), "overwritten"), 0);
    EXPECT_EQ(file_handler::read_file(read_only.c_str()), "managed");
  }

  // A dangling symlink is not replaced by a regular file.
  const auto dangling = dir / "dangling.json";
  fs::create_symlink("missing-target.json", dangling);
  EXPECT_NE(file_handler::write_file(dangling.c_str(), "data"), 0);
  EXPECT_TRUE(fs::is_symlink(dangling));
  EXPECT_FALSE(fs::exists(dir / "missing-target.json"));

  // Nor is a FIFO.
  const auto fifo = dir / "fifo";
  ASSERT_EQ(::mkfifo(fifo.c_str(), 0600), 0);
  EXPECT_NE(file_handler::write_file(fifo.c_str(), "data"), 0);
  EXPECT_TRUE(fs::is_fifo(fifo));

  // A private file stays private, even under umask 0.
  const auto secret = dir / "secret.json";
  ASSERT_EQ(file_handler::write_file(secret.c_str(), "old"), 0);
  fs::permissions(secret, fs::perms::owner_read | fs::perms::owner_write);
  const auto old_umask = ::umask(0);
  EXPECT_EQ(file_handler::write_file(secret.c_str(), "new"), 0);
  ::umask(old_umask);
  EXPECT_EQ(fs::status(secret).permissions() & fs::perms::all, fs::perms::owner_read | fs::perms::owner_write);

  fs::permissions(read_only, fs::perms::owner_all);
  std::error_code ec;
  fs::remove_all(dir, ec);
}

// Bugs caught: 0, False positives: 0
TEST(FileHandlerTests, WriteFileKeepsHardLinksAndAccessAcls) {
  namespace fs = std::filesystem;
  const auto dir = fs::path {SUNSHINE_TEST_BIN_DIR} / "write_file_links";
  fs::remove_all(dir);
  fs::create_directories(dir);

  // Every hard link sees the new contents: the file is not split into two inodes.
  const auto file = dir / "sunshine.conf";
  const auto alias = dir / "alias.conf";
  ASSERT_EQ(file_handler::write_file(file.c_str(), "old"), 0);
  fs::create_hard_link(file, alias);
  ASSERT_EQ(file_handler::write_file(file.c_str(), "new"), 0);
  EXPECT_EQ(file_handler::read_file(alias.c_str()), "new");
  EXPECT_EQ(fs::hard_link_count(file), 2U);

  // An access ACL survives the save unchanged (skipped where setfacl or ACL support is missing). The file name is an
  // argument, not shell text, so no build directory name can turn the check into a skip.
  const auto acl_file = dir / "acl.json";
  ASSERT_EQ(file_handler::write_file(acl_file.c_str(), "old"), 0);
  namespace bp = boost::process::v1;
  // Explicit PATH entries: the default search reads PATH through the process locale, which can be unavailable.
  std::vector<boost::filesystem::path> path_dirs;
  if (const char *path = std::getenv("PATH")) {
    std::string_view rest {path};
    while (!rest.empty()) {
      const auto colon = rest.find(':');
      if (const auto dir_entry = rest.substr(0, colon); !dir_entry.empty()) {
        path_dirs.emplace_back(std::string {dir_entry});
      }
      rest = colon == std::string_view::npos ? std::string_view {} : rest.substr(colon + 1);
    }
  }
  const auto setfacl = bp::search_path("setfacl", path_dirs);
  if (!setfacl.empty() && bp::system(setfacl, "-m", "u:nobody:r", "--", acl_file.string(), bp::std_out > bp::null, bp::std_err > bp::null) == 0) {
    const auto acl = access_acl_of(acl_file);
    ASSERT_FALSE(acl.empty());
    ASSERT_EQ(file_handler::write_file(acl_file.c_str(), "new"), 0);
    EXPECT_EQ(file_handler::read_file(acl_file.c_str()), "new");
    EXPECT_EQ(access_acl_of(acl_file), acl);
  }

  std::error_code ec;
  fs::remove_all(dir, ec);
}

// Bugs caught: 0, False positives: 0
TEST(FileHandlerTests, OnlyAMissingAttributeOrNoAclSupportProvesThereIsNoAcl) {
  EXPECT_TRUE(file_handler::detail::access_acl_absent(-1, ENODATA));
  EXPECT_TRUE(file_handler::detail::access_acl_absent(-1, EOPNOTSUPP));
  EXPECT_TRUE(file_handler::detail::access_acl_absent(-1, ENOTSUP));
  // A failed probe proves nothing; replacing the file could drop its ACL.
  EXPECT_FALSE(file_handler::detail::access_acl_absent(-1, EACCES));
  EXPECT_FALSE(file_handler::detail::access_acl_absent(-1, EIO));
  EXPECT_FALSE(file_handler::detail::access_acl_absent(28, 0));
}

// Bugs caught: 0, False positives: 0
TEST(FileHandlerTests, InPlaceWriteLeavesAFileReplacedMeanwhileAlone) {
  namespace fs = std::filesystem;
  const auto dir = fs::path {SUNSHINE_TEST_BIN_DIR} / "write_file_in_place";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  const auto file = dir / "sunshine_state.json";

  ASSERT_EQ(file_handler::write_file(file.c_str(), "old"), 0);
  struct stat inspected {};
  ASSERT_EQ(::stat(file.c_str(), &inspected), 0);
  ASSERT_EQ(file_handler::detail::write_in_place(file.c_str(), "new", inspected.st_dev, inspected.st_ino), 0);
  EXPECT_EQ(file_handler::read_file(file.c_str()), "new");

  // An editor saved its own copy after the inspection: its contents stay.
  const auto replacement = dir / "replacement.json";
  {
    std::ofstream out(replacement);
    out << "edited elsewhere";
  }
  fs::rename(replacement, file);
  EXPECT_NE(file_handler::detail::write_in_place(file.c_str(), "ours", inspected.st_dev, inspected.st_ino), 0);
  EXPECT_EQ(file_handler::read_file(file.c_str()), "edited elsewhere");

  // A FIFO put in its place doesn't block the write.
  fs::remove(file);
  ASSERT_EQ(::mkfifo(file.c_str(), 0600), 0);
  struct stat fifo {};
  ASSERT_EQ(::stat(file.c_str(), &fifo), 0);
  auto result = std::async(std::launch::async, [&] {
    return file_handler::detail::write_in_place(file.c_str(), "ours", fifo.st_dev, fifo.st_ino);
  });
  ASSERT_EQ(result.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  EXPECT_NE(result.get(), 0);

  fs::remove_all(dir, ec);
}
#endif
