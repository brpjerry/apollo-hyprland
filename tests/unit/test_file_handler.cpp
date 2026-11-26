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
  #include <filesystem>
  #include <unistd.h>

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

  // A failed write reports the failure and leaves the old contents (root can write anywhere, so skip that part).
  if (::geteuid() != 0) {
    fs::permissions(dir, fs::perms::owner_read | fs::perms::owner_exec);
    EXPECT_NE(file_handler::write_file(target.c_str(), "lost"), 0);
    EXPECT_EQ(file_handler::read_file(target.c_str()), "through link");
    fs::permissions(dir, fs::perms::owner_all);
  }
  std::error_code ec;
  fs::remove_all(dir, ec);
}
#endif
