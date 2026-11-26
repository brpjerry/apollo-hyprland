/**
 * @file tests/unit/test_nvhttp_client_auth.cpp
 * @brief Test exact paired-client certificate authorization and persistence.
 */

// test includes
#include "../certificate_test_utils.h"
#include "../tests_common.h"

// standard includes
#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>
#include <type_traits>
#include <vector>

// local includes
#include <src/config.h>
#include <src/httpcommon.h>
#include <src/nvhttp.h>
#include <src/process.h>
#include <src/uuid.h>

namespace fs = std::filesystem;

static_assert(std::is_same_v<decltype(&nvhttp::pin), bool (*)(std::string_view, const std::string &, std::string)>);

/**
 * @brief Isolate paired-client authorization tests from the user's Sunshine state.
 */
class ClientAuthorizationTest: public BaseTest {
protected:
  /**
   * @brief Redirect persisted client state to the test build directory.
   */
  void SetUp() override {
    BaseTest::SetUp();
    original_state_file = config::nvhttp.file_state;
    original_fresh_state = config::sunshine.flags[config::flag::FRESH_STATE];
    state_file = fs::path {SUNSHINE_TEST_BIN_DIR} / "client_authorization_state.json";

    config::nvhttp.file_state = state_file.string();
    config::sunshine.flags[config::flag::FRESH_STATE] = false;
    nvhttp::test_support::reset_client_state();

    std::error_code remove_error;
    fs::remove(state_file, remove_error);
  }

  /**
   * @brief Remove test state and restore the caller's configuration.
   */
  void TearDown() override {
    nvhttp::test_support::reset_client_state();
    std::error_code remove_error;
    fs::remove(state_file, remove_error);

    config::nvhttp.file_state = original_state_file;
    config::sunshine.flags[config::flag::FRESH_STATE] = original_fresh_state;
    BaseTest::TearDown();
  }

protected:
  fs::path state_file;  ///< Task-specific persisted state fixture.

private:
  std::string original_state_file;  ///< State-file setting restored after each test.
  bool original_fresh_state;  ///< Fresh-state flag restored after each test.
};

TEST_F(ClientAuthorizationTest, CanonicalIdentityFailsClosedAndTracksEnableState) {
  const auto paired_credentials = test_utils::certificates::generate_ca_credentials();
  const auto crlf_certificate = test_utils::certificates::to_crlf_pem(paired_credentials.x509);
  const auto unknown_credentials = crypto::gen_creds("Sunshine Unknown Client", 2048);
  const auto uuid = nvhttp::test_support::add_client("paired", crlf_certificate, true);

  EXPECT_TRUE(nvhttp::test_support::add_client("invalid", "not a certificate", true).empty());
  ASSERT_FALSE(uuid.empty());
  EXPECT_EQ(nvhttp::get_cert_by_uuid(uuid), paired_credentials.x509);
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(crlf_certificate));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(unknown_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate("not a certificate"));

  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, false));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, true));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
}

TEST_F(ClientAuthorizationTest, DerivedLeafDoesNotInheritPairedAuthorization) {
  const auto paired_credentials = test_utils::certificates::generate_ca_credentials();
  const auto derived_credentials = test_utils::certificates::generate_derived_leaf(paired_credentials);
  const auto uuid = nvhttp::test_support::add_client("paired issuer", paired_credentials.x509, true);

  ASSERT_FALSE(uuid.empty());
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(derived_credentials.x509));

  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, false));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(derived_credentials.x509));
}

TEST_F(ClientAuthorizationTest, MultipleClientsPersistAndUnpairIndependently) {
  const auto enabled_credentials = test_utils::certificates::generate_ca_credentials("Sunshine Enabled Client");
  const auto disabled_credentials = crypto::gen_creds("Sunshine Disabled Client", 2048);
  const auto expired_credentials = test_utils::certificates::expire_credentials(
    test_utils::certificates::generate_ca_credentials("Sunshine Expired Client")
  );

  const auto enabled_uuid = nvhttp::test_support::add_client("enabled", enabled_credentials.x509, true);
  const auto disabled_uuid = nvhttp::test_support::add_client("disabled", disabled_credentials.x509, false);
  const auto expired_uuid = nvhttp::test_support::add_client("expired", expired_credentials.x509, true);
  ASSERT_FALSE(enabled_uuid.empty());
  ASSERT_FALSE(disabled_uuid.empty());
  ASSERT_FALSE(expired_uuid.empty());
  EXPECT_EQ(nvhttp::get_all_clients().size(), 3);

  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(enabled_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(disabled_credentials.x509));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(expired_credentials.x509));

  nvhttp::test_support::reset_client_state();
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(enabled_credentials.x509));
  nvhttp::test_support::reload_client_state();

  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(enabled_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(disabled_credentials.x509));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(expired_credentials.x509));

  ASSERT_TRUE(nvhttp::set_client_enabled(disabled_uuid, true));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(disabled_credentials.x509));
  ASSERT_TRUE(nvhttp::unpair_client(enabled_uuid));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(enabled_credentials.x509));

  nvhttp::erase_all_clients();
  nvhttp::test_support::reset_client_state();
  nvhttp::test_support::reload_client_state();
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(disabled_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(expired_credentials.x509));
}

TEST_F(ClientAuthorizationTest, DuplicateCertificateIdentityFailsClosed) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto uuid = nvhttp::test_support::add_client("first", credentials.x509, true);
  ASSERT_FALSE(uuid.empty());
  EXPECT_FALSE(nvhttp::test_support::duplicate_client("missing"));
  ASSERT_TRUE(nvhttp::test_support::duplicate_client(uuid));

  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(credentials.x509));
}

TEST_F(ClientAuthorizationTest, RePairingReplacesDuplicateCertificateIdentity) {
  const auto paired_credentials = test_utils::certificates::generate_ca_credentials();
  const auto other_credentials = test_utils::certificates::generate_ca_credentials("Sunshine Other Client");
  const auto original_uuid = nvhttp::test_support::add_client("original", paired_credentials.x509, true);
  const auto other_uuid = nvhttp::test_support::add_client("other", other_credentials.x509, true);
  ASSERT_FALSE(original_uuid.empty());
  ASSERT_FALSE(other_uuid.empty());
  ASSERT_TRUE(nvhttp::test_support::duplicate_client(original_uuid));
  ASSERT_EQ(nvhttp::get_all_clients().size(), 3);
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));

  const auto repaired_uuid = nvhttp::test_support::add_client(
    "repaired",
    test_utils::certificates::to_crlf_pem(paired_credentials.x509),
    true
  );

  ASSERT_FALSE(repaired_uuid.empty());
  EXPECT_NE(repaired_uuid, original_uuid);
  EXPECT_EQ(nvhttp::get_all_clients().size(), 2);
  EXPECT_EQ(nvhttp::get_cert_by_uuid(repaired_uuid), paired_credentials.x509);
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(other_credentials.x509));

  nvhttp::test_support::reset_client_state();
  nvhttp::test_support::reload_client_state();
  EXPECT_EQ(nvhttp::get_all_clients().size(), 2);
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(other_credentials.x509));
}

TEST_F(ClientAuthorizationTest, ConcurrentStateChangesRemainConsistent) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto uuid = nvhttp::test_support::add_client("concurrent", credentials.x509, true);
  ASSERT_FALSE(uuid.empty());

  std::atomic_bool operations_succeeded {true};
  std::vector<std::jthread> workers;
  for (std::size_t worker = 0; worker < 4; ++worker) {
    workers.emplace_back([&operations_succeeded, uuid, certificate = credentials.x509, worker]() {
      for (std::size_t iteration = 0; iteration < 8; ++iteration) {
        if (!nvhttp::set_client_enabled(uuid, (worker + iteration) % 2 == 0)) {
          operations_succeeded = false;
        }
        static_cast<void>(nvhttp::test_support::authorize_client_certificate(certificate));
      }
    });
  }
  workers.clear();

  EXPECT_TRUE(operations_succeeded);
  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, false));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(credentials.x509));
}

// Bugs caught: 0, False positives: 0
TEST_F(ClientAuthorizationTest, KeepAliveRequestsSeeTheCurrentClientRecord) {
  // The TLS handshake runs once per connection; every later request on it must see unpairing, disabling and edits.
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto uuid = nvhttp::test_support::add_client("before", credentials.x509, true);
  ASSERT_FALSE(uuid.empty());

  const auto snapshot = nvhttp::test_support::current_client_record(credentials.x509);
  ASSERT_NE(snapshot, nullptr);
  EXPECT_EQ(snapshot->name, "before");

  // The handler's copy does not change under it; the next request sees the edit.
  ASSERT_TRUE(nvhttp::update_device_info(uuid, "after", "", {}, {}, crypto::PERM::_default, true, true, false));
  EXPECT_EQ(snapshot->name, "before");
  EXPECT_EQ(nvhttp::test_support::current_client_record(credentials.x509)->name, "after");

  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, false));
  EXPECT_EQ(nvhttp::test_support::current_client_record(credentials.x509), nullptr);
  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, true));
  EXPECT_NE(nvhttp::test_support::current_client_record(credentials.x509), nullptr);

  ASSERT_TRUE(nvhttp::unpair_client(uuid));
  EXPECT_EQ(nvhttp::test_support::current_client_record(credentials.x509), nullptr);
}

// Bugs caught: 0, False positives: 0
TEST_F(ClientAuthorizationTest, DuplicateIdentityStaysRejectedAfterAnUnrelatedSave) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto other_credentials = test_utils::certificates::generate_ca_credentials("Sunshine Other Client");
  const auto uuid = nvhttp::test_support::add_client("first", credentials.x509, true);
  const auto other_uuid = nvhttp::test_support::add_client("other", other_credentials.x509, true);
  ASSERT_TRUE(nvhttp::test_support::duplicate_client(uuid));
  ASSERT_FALSE(nvhttp::test_support::authorize_client_certificate(credentials.x509));

  // Saving another client's change must not drop the duplicate, or the identity is authorized after a restart.
  ASSERT_TRUE(nvhttp::set_client_enabled(other_uuid, false));
  nvhttp::test_support::reset_client_state();
  nvhttp::test_support::reload_client_state();
  EXPECT_EQ(nvhttp::get_all_clients().size(), 3);
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(credentials.x509));
}

// Bugs caught: 0, False positives: 0
TEST_F(ClientAuthorizationTest, LoadsPropertyTreeStateFiles) {
  // Sunshine and older Apollo wrote the state with Boost's property tree: booleans and numbers as strings, and an
  // empty client list as "".
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  {
    std::ofstream out(state_file);
    out << nlohmann::json {{"root", {{"uniqueid", "0123abcd-0000-4000-8000-000000000001"}, {"named_devices", ""}}}}.dump();
  }
  ASSERT_NO_THROW(nvhttp::test_support::reload_client_state());
  EXPECT_EQ(nvhttp::get_all_clients().size(), 0);

  {
    std::ofstream out(state_file);
    out << nlohmann::json {{"root", {{"uniqueid", "0123abcd-0000-4000-8000-000000000001"}, {"named_devices", nlohmann::json::array({{{"name", "legacy"}, {"cert", credentials.x509}, {"uuid", "0123abcd-0000-4000-8000-000000000002"}, {"enabled", "false"}, {"perm", "50331648"}, {"allow_client_commands", "false"}}})}}}}.dump();
  }
  ASSERT_NO_THROW(nvhttp::test_support::reload_client_state());
  const auto clients = nvhttp::get_all_clients();
  ASSERT_EQ(clients.size(), 1);
  // A disabled client stays disabled, and keeps its permissions rather than getting all of them.
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(credentials.x509));
  EXPECT_EQ(clients.at(0).value("perm", 0U), static_cast<std::uint32_t>(crypto::PERM::_default));  // "50331648"
}

// Bugs caught: 0, False positives: 0
TEST_F(ClientAuthorizationTest, RepairedHostIdIsSaved) {
  // An unusable host id gets replaced once; the replacement must survive the next restart.
  for (const auto &stored : {nlohmann::json("not-a-uuid"), nlohmann::json("0123abcd-0000-4000-8000-00000000000z"), nlohmann::json(42)}) {
    SCOPED_TRACE(stored.dump());
    {
      std::ofstream out(state_file);
      out << nlohmann::json {{"root", {{"uniqueid", stored}, {"named_devices", nlohmann::json::array()}}}}.dump();
    }
    ASSERT_NO_THROW(nvhttp::test_support::reload_client_state());
    const auto repaired = http::unique_id;
    EXPECT_TRUE(uuid_util::uuid_t::valid_text(repaired));
    nvhttp::test_support::reload_client_state();
    EXPECT_EQ(http::unique_id, repaired);
  }
}

// Bugs caught: 0, False positives: 0
TEST(LaunchRequestTest, ViewPermissionNeverCoversTerminate) {
  const auto saved_terminate_id = proc::terminate_app_id;
  proc::terminate_app_id = 777;
  const auto restore = util::fail_guard([saved_terminate_id]() {
    proc::terminate_app_id = saved_terminate_id;
  });
  constexpr std::string_view running_uuid = "RUNNING-APP";

  // Joining the running app needs only view permission.
  EXPECT_EQ(nvhttp::launch_permission(5, "", false, 5, running_uuid), crypto::PERM::_allow_view);
  EXPECT_EQ(nvhttp::launch_permission(0, running_uuid, false, 5, running_uuid), crypto::PERM::_allow_view);
  EXPECT_EQ(nvhttp::launch_permission(0, "", true, 5, running_uuid), crypto::PERM::_allow_view);

  // Either identifier naming Terminate stops the app, so it needs launch permission.
  EXPECT_EQ(nvhttp::launch_permission(5, TERMINATE_APP_UUID, false, 5, running_uuid), crypto::PERM::launch);
  EXPECT_EQ(nvhttp::launch_permission(777, running_uuid, false, 5, running_uuid), crypto::PERM::launch);

  // Another app, or anything on an idle host, needs launch permission.
  EXPECT_EQ(nvhttp::launch_permission(6, "", false, 5, running_uuid), crypto::PERM::launch);
  EXPECT_EQ(nvhttp::launch_permission(0, "OTHER", false, 0, ""), crypto::PERM::launch);
}

// Bugs caught: 0, False positives: 0
TEST(LaunchRequestTest, UuidOnlyLaunchOnAnIdleHostStartsTheApp) {
  // An omitted appid reads as 0, like an idle host's running app: that is not a resume.
  EXPECT_FALSE(nvhttp::launch_targets_running_app(0, "SOME-APP", 0, ""));
  EXPECT_TRUE(nvhttp::launch_targets_running_app(0, "SOME-APP", 3, "SOME-APP"));
  EXPECT_TRUE(nvhttp::launch_targets_running_app(3, "", 3, "SOME-APP"));
  EXPECT_FALSE(nvhttp::launch_targets_running_app(0, "", 0, ""));
}

/**
 * @brief Launch arguments of a client launch.
 *
 * @param mode The mode argument.
 */
static nvhttp::args_t launch_args(const std::string &mode) {
  nvhttp::args_t args;
  args.emplace("rikey", std::string(32, '0'));
  args.emplace("rikeyid", "1");
  args.emplace("mode", mode);
  return args;
}

// Bugs caught: 0, False positives: 0
TEST(LaunchRequestTest, ClientCommandsNeedTheClientSwitch) {
  crypto::named_cert_t client {};
  client.name = "client";
  client.uuid = "0123abcd-0000-4000-8000-000000000003";
  client.perm = crypto::PERM::_all;
  client.do_cmds = {crypto::command_entry_t {"echo do", false}};
  client.undo_cmds = {crypto::command_entry_t {"echo undo", false}};

  client.allow_client_commands = false;
  auto session = nvhttp::make_launch_session(false, false, launch_args("1920x1080x60"), &client);
  EXPECT_TRUE(session->client_do_cmds.empty());
  EXPECT_TRUE(session->client_undo_cmds.empty());

  client.allow_client_commands = true;
  session = nvhttp::make_launch_session(false, false, launch_args("1920x1080x60"), &client);
  EXPECT_EQ(session->client_do_cmds.size(), 1U);
  EXPECT_EQ(session->client_undo_cmds.size(), 1U);
}

// Bugs caught: 0, False positives: 0
TEST(LaunchRequestTest, ModeWithoutARateGetsTheDefaultRate) {
  crypto::named_cert_t client {};
  client.uuid = "0123abcd-0000-4000-8000-000000000004";
  for (const auto *mode : {"1920x1080x", "1920x1080x0", "1920x1080x-5"}) {
    SCOPED_TRACE(mode);
    EXPECT_EQ(nvhttp::make_launch_session(false, false, launch_args(mode), &client)->fps, 60000);
  }
  EXPECT_EQ(nvhttp::make_launch_session(false, false, launch_args("1920x1080x120"), &client)->fps, 120000);
}
