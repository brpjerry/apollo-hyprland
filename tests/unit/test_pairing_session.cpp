/**
 * @file tests/unit/test_pairing_session.cpp
 * @brief Test the stored pairing sessions in src/nvhttp.cpp (PIN entry and restarted pairing).
 */

#include "../tests_common.h"

#include <src/nvhttp.h>

using namespace nvhttp;

namespace {
  constexpr auto SALT = "ff5dc6eda99339a8a0793e216c4257c4";

  pair_session_t make_session(const std::string &unique_id, const std::string &salt = SALT) {
    pair_session_t sess {};
    sess.client.uniqueID = unique_id;
    sess.client.name = "client";
    sess.async_insert_pin.salt = salt;
    return sess;
  }
}  // namespace

// Bugs caught: 1, False positives: 0
// A client that restarted pairing while its first session already had a PIN made the next PIN entry an
// out-of-order getservercert. fail_pair() erased the session and pin() kept writing to it (heap
// corruption, apollo-dev crashed on 2026-11-25). The PIN must be rejected without touching the erased session.
TEST(PairingSessionTest, PinForAnUnfinishedSessionIsRejectedSafely) {
  auto &stale = start_pair_session(make_session("pin-stale-client"));
  stale.last_phase = PAIR_PHASE::GETSERVERCERT;

  EXPECT_FALSE(pin("1234", "laptop"));

  // fail_pair() removed the session; nothing is left for a later PIN to pick up.
  EXPECT_FALSE(pin("1234", "laptop"));
}

// Bugs caught: 0, False positives: 0
// getservercert from a client that already has a session starts the pairing over: the new salt is used
// and the session is back at the first phase, so the PIN for the new attempt is accepted.
TEST(PairingSessionTest, RestartedPairingReplacesTheUnfinishedSession) {
  auto &first = start_pair_session(make_session("restart-client", std::string(32, 'a')));
  first.last_phase = PAIR_PHASE::GETSERVERCERT;

  auto &second = start_pair_session(make_session("restart-client", SALT));
  EXPECT_EQ(second.last_phase, PAIR_PHASE::NONE);
  EXPECT_EQ(second.async_insert_pin.salt, SALT);

  remove_session(second);
}
