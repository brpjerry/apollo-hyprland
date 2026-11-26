/**
 * @file tests/unit/test_pairing_session.cpp
 * @brief Test the stored pairing sessions in src/nvhttp.cpp (PIN entry and restarted pairing).
 */

#include "../tests_common.h"

#include <src/nvhttp.h>

#include <algorithm>
#include <chrono>
#include <thread>

using namespace nvhttp;

namespace {
  constexpr auto SALT = "ff5dc6eda99339a8a0793e216c4257c4";

  pair_session_t make_session(const std::string &unique_id, const std::string &salt = SALT) {
    pair_session_t sess {};
    sess.client.uniqueID = unique_id;
    sess.client.name = "client";
    sess.async_insert_pin.salt = salt;
    sess.async_insert_pin.device_name = "client";
    return sess;
  }

  bool is_pending(const std::string &pairing_id) {
    const auto pending = get_pending_pairings();
    return std::ranges::any_of(pending, [&pairing_id](const pending_pairing_t &pairing) {
      return pairing.id == pairing_id;
    });
  }

  /**
   * @brief Drops every stored pairing session after each test.
   */
  struct PairingSessionTest: testing::Test {
    void TearDown() override {
      expire_pair_sessions(std::chrono::steady_clock::time_point::max());
    }
  };
}  // namespace

// Bugs caught: 1, False positives: 0
// A client that restarted pairing while its first session already had a PIN made the next PIN entry an
// out-of-order getservercert. fail_pair() erased the session and pin() kept writing to it (heap
// corruption, apollo-dev crashed on 2026-11-25). A PIN for a session that is past the first phase must be
// rejected without touching that session.
TEST_F(PairingSessionTest, PinForAnUnfinishedSessionIsRejectedSafely) {
  auto stale = make_session("pin-stale-client");
  stale.last_phase = PAIR_PHASE::GETSERVERCERT;
  std::string pairing_id;
  ASSERT_EQ(insert_pair_session(std::move(stale), pairing_id), pair_session_insert_e::ADDED);

  // Only sessions waiting for a PIN can be approved.
  EXPECT_FALSE(is_pending(pairing_id));
  EXPECT_FALSE(pin(pairing_id, "1234", "laptop"));
  EXPECT_FALSE(pin(pairing_id, "1234", "laptop"));
}

// Bugs caught: 0, False positives: 0
// getservercert from a client that already has an unfinished session starts the pairing over: the abandoned
// session is evicted and the new one waits for a PIN at the first phase.
TEST_F(PairingSessionTest, RestartedPairingReplacesTheUnfinishedSession) {
  auto first = make_session("restart-client", std::string(32, 'a'));
  first.last_phase = PAIR_PHASE::GETSERVERCERT;
  std::string first_id;
  ASSERT_EQ(insert_pair_session(std::move(first), first_id), pair_session_insert_e::ADDED);

  std::string second_id;
  ASSERT_EQ(insert_pair_session(make_session("restart-client", SALT), second_id), pair_session_insert_e::ALREADY_EXISTS);
  ASSERT_TRUE(evict_abandoned_pair_session("restart-client"));
  ASSERT_EQ(insert_pair_session(make_session("restart-client", SALT), second_id), pair_session_insert_e::ADDED);

  EXPECT_NE(first_id, second_id);
  EXPECT_TRUE(is_pending(second_id));
}

// Bugs caught: 1, False positives: 0
// pin() runs on the web UI thread while pair() runs on the nvhttp server threads; both change the session map.
// Run under ThreadSanitizer to see races; without the lock this also crashes intermittently.
TEST_F(PairingSessionTest, ConcurrentPairingAndPinEntry) {
  constexpr int rounds = 2000;
  auto id = [](int i) {
    return "stress-client-" + std::to_string(i % 8);
  };
  std::thread starter([&] {
    for (int i = 0; i < rounds; ++i) {
      std::string pairing_id;
      insert_pair_session(make_session(id(i)), pairing_id);
    }
  });
  std::thread remover([&] {
    for (int i = 0; i < rounds; ++i) {
      pair_session_t sess {};
      sess.client.uniqueID = id(i);
      remove_session(sess);
    }
  });
  std::thread pinner([&] {
    for (int i = 0; i < rounds; ++i) {
      if (const auto pending = get_pending_pairings(); !pending.empty()) {
        pin(pending.front().id, "1234", "client");
      }
    }
  });
  starter.join();
  remover.join();
  pinner.join();
}

// Bugs caught: 0, False positives: 0
// With two clients waiting for a PIN, the PIN goes to the session the user selected, not to an arbitrary or the
// newest map entry; the other session keeps waiting.
TEST_F(PairingSessionTest, PinGoesToTheSelectedWaitingSession) {
  std::string older_id;
  ASSERT_EQ(insert_pair_session(make_session("pin-older-client"), older_id), pair_session_insert_e::ADDED);
  std::string newer_id;
  ASSERT_EQ(insert_pair_session(make_session("pin-newer-client"), newer_id), pair_session_insert_e::ADDED);

  // Test sessions have no parked client response, so pin() consumes the selected session and returns false.
  EXPECT_FALSE(pin(older_id, "1234", "client"));

  EXPECT_FALSE(is_pending(older_id));
  EXPECT_TRUE(is_pending(newer_id));
}
