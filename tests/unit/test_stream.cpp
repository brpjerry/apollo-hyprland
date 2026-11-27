/**
 * @file tests/unit/test_stream.cpp
 * @brief Test src/stream.*
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <list>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// lib includes
#include <boost/process/v1/environment.hpp>
#include <rs.h>

// local includes
#include <src/crypto.h>
#include <src/network.h>
#include <src/process.h>
#include <src/utility.h>

namespace stream {
  std::vector<uint8_t> concat_and_insert(uint64_t insert_size, uint64_t slice_size, const std::string_view &data1, const std::string_view &data2);
  std::optional<std::pair<std::uint16_t, std::string_view>> parse_control_packet(const ENetPacket &packet);
  std::optional<std::uint8_t> server_cmd_index(std::string_view payload);
  void spawn_client_commands(const std::list<crypto::command_entry_t> &cmd_list, std::string_view kind);
  bool test_receive_ping_endpoints(bool session_id_pings);

  namespace fec {
    /**
     * @brief Return the production video FEC data-shard limit.
     *
     * @param percentage Recovery-shard percentage between 0 and 100.
     * @return Maximum data shards in one 8-bit Reed-Solomon block.
     */
    int max_data_shards(int percentage);
  }  // namespace fec
}  // namespace stream

/**
 * @brief Preserve the 8-bit shard limits for disabled, default, and maximum FEC.
 */
TEST(StreamFecTests, PreservesVideoShardLimits) {
  EXPECT_EQ(stream::fec::max_data_shards(0), 255);
  EXPECT_EQ(stream::fec::max_data_shards(20), 212);
  EXPECT_EQ(stream::fec::max_data_shards(100), 127);
}

/**
 * @brief Every supported FEC percentage fills the encoder without exceeding its shard limit.
 */
TEST(StreamFecTests, FitsDataAndRoundedParityWithinEncoderLimit) {
  for (int percentage = 1; percentage <= 100; ++percentage) {
    SCOPED_TRACE(percentage);
    const auto data_shards = stream::fec::max_data_shards(percentage);
    const auto parity_shards = (data_shards * percentage + 99) / 100;
    auto encoder = reed_solomon_new(data_shards, parity_shards);
    ASSERT_NE(encoder, nullptr);
    reed_solomon_release(encoder);

    const auto next_data_shards = data_shards + 1;
    const auto next_parity_shards = (next_data_shards * percentage + 99) / 100;
    EXPECT_GT(next_data_shards + next_parity_shards, RS8_DATA_SHARDS_MAX);
  }
}

TEST(StreamPingTests, KeepsLegacyAudioAndVideoSenderEndpointsSeparate) {
  for (int attempt = 0; attempt < 20; ++attempt) {
    ASSERT_TRUE(stream::test_receive_ping_endpoints(false)) << "Attempt " << attempt;
  }
}

TEST(StreamPingTests, KeepsSessionIdAudioAndVideoSenderEndpointsSeparate) {
  for (int attempt = 0; attempt < 20; ++attempt) {
    ASSERT_TRUE(stream::test_receive_ping_endpoints(true)) << "Attempt " << attempt;
  }
}

TEST(ConcatAndInsertTests, ConcatNoInsertionTest) {
  std::array<char, 2> b1 = {'a', 'b'};
  std::array<char, 3> b2 = {'c', 'd', 'e'};
  auto res = stream::concat_and_insert(0, 2, std::string_view {b1.data(), b1.size()}, std::string_view {b2.data(), b2.size()});
  auto expected = std::vector<uint8_t> {'a', 'b', 'c', 'd', 'e'};
  ASSERT_EQ(res, expected);
}

TEST(ConcatAndInsertTests, ConcatLargeStrideTest) {
  std::array<char, 2> b1 = {'a', 'b'};
  std::array<char, 3> b2 = {'c', 'd', 'e'};
  auto res = stream::concat_and_insert(1, b1.size() + b2.size() + 1, std::string_view {b1.data(), b1.size()}, std::string_view {b2.data(), b2.size()});
  auto expected = std::vector<uint8_t> {0, 'a', 'b', 'c', 'd', 'e'};
  ASSERT_EQ(res, expected);
}

TEST(ConcatAndInsertTests, ConcatSmallStrideTest) {
  std::array<char, 2> b1 = {'a', 'b'};
  std::array<char, 3> b2 = {'c', 'd', 'e'};
  auto res = stream::concat_and_insert(1, 1, std::string_view {b1.data(), b1.size()}, std::string_view {b2.data(), b2.size()});
  auto expected = std::vector<uint8_t> {0, 'a', 0, 'b', 0, 'c', 0, 'd', 0, 'e'};
  ASSERT_EQ(res, expected);
}

TEST(ControlPacketTests, RejectsZeroLengthPacket) {
  net::packet_t packet {enet_packet_create(nullptr, 0, 0)};

  ASSERT_NE(packet, nullptr);
  EXPECT_EQ(packet->data, nullptr);
  EXPECT_EQ(stream::parse_control_packet(*packet), std::nullopt);
}

TEST(ControlPacketTests, RejectsOneBytePacket) {
  const std::uint8_t data {0x06};
  net::packet_t packet {enet_packet_create(&data, sizeof(data), 0)};

  ASSERT_NE(packet, nullptr);
  EXPECT_EQ(stream::parse_control_packet(*packet), std::nullopt);
}

TEST(ControlPacketTests, AcceptsTypeWithoutPayload) {
  const std::array<std::uint8_t, 2> data {0x06, 0x02};
  net::packet_t packet {enet_packet_create(data.data(), data.size(), 0)};

  ASSERT_NE(packet, nullptr);
  auto message = stream::parse_control_packet(*packet);
  ASSERT_TRUE(message);
  EXPECT_EQ(message->first, 0x0206);
  EXPECT_TRUE(message->second.empty());
}

TEST(ControlPacketTests, AcceptsTypeAndPayload) {
  const std::array<std::uint8_t, 5> data {0x06, 0x02, 'a', 'b', 'c'};
  net::packet_t packet {enet_packet_create(data.data(), data.size(), 0)};

  ASSERT_NE(packet, nullptr);
  auto message = stream::parse_control_packet(*packet);
  ASSERT_TRUE(message);
  EXPECT_EQ(message->first, 0x0206);
  EXPECT_EQ(message->second, "abc");
}

// Bugs caught: 0, False positives: 0
TEST(ControlPacketTests, ServerCommandIndexMatchesApollosClient) {
  // LiSendExecServerCmd() in patches/moonlight-common-c-0001 sends {cmdId, 0, 0, 0}.
  const std::array<char, 4> padded {2, 0, 0, 0};
  EXPECT_EQ(stream::server_cmd_index({padded.data(), padded.size()}), std::optional<std::uint8_t> {2});
  const char bare = 3;
  EXPECT_EQ(stream::server_cmd_index({&bare, 1}), std::optional<std::uint8_t> {3});
  EXPECT_EQ(stream::server_cmd_index({}), std::nullopt);
  EXPECT_EQ(stream::server_cmd_index({padded.data(), 2}), std::nullopt);
}

#ifdef __linux__
// Bugs caught: 0, False positives: 0
TEST(ClientCommandTests, DoCommandsFollowShellQuoting) {
  // HYP-40: Boost.Process split this on double quotes only and ran sh -c "'date", so nothing was written.
  auto tmpl = (std::filesystem::temp_directory_path() / "aclient-XXXXXX").string();
  ASSERT_NE(::mkdtemp(tmpl.data()), nullptr);
  const std::filesystem::path dir {tmpl};
  auto saved_proc = std::move(proc::proc);
  const auto restore = util::fail_guard([&]() {
    proc::proc = std::move(saved_proc);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  });
  // The commands get the app's environment; a temporary directory stands in for the runtime directory.
  boost::process::v1::environment env = boost::this_process::environment();  // a copy, not this process's
  env["XDG_RUNTIME_DIR"] = dir.string();
  proc::proc = proc::proc_t {std::move(env), {}};

  stream::spawn_client_commands({{R"(sh -c 'date > "$XDG_RUNTIME_DIR/x"')", false}}, "do");

  // The command is detached; wait for what it writes.
  const auto file = dir / "x";
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds {10};
  std::error_code ec;
  while ((!std::filesystem::exists(file, ec) || std::filesystem::file_size(file, ec) == 0) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds {20});
  }
  ASSERT_TRUE(std::filesystem::exists(file));
  std::string line;
  std::getline(std::ifstream {file}, line);
  EXPECT_FALSE(line.empty());
}
#endif
