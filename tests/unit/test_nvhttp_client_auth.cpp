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
#include <iterator>
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

// lib includes
#include <boost/process/v1.hpp>

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

// Bugs caught: 0, False positives: 0
TEST_F(ClientAuthorizationTest, MissingHostIdKeepsClientsAndTheRestOfTheFile) {
  // The state file also holds the Web UI credentials: repairing the host id must keep them and the paired clients.
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  {
    std::ofstream out(state_file);
    out << nlohmann::json {
      {"username", "admin"},
      {"root", {{"named_devices", nlohmann::json::array({{{"name", "phone"}, {"cert", credentials.x509}, {"uuid", "0123abcd-0000-4000-8000-000000000005"}}})}}},
    }.dump();
  }
  nvhttp::test_support::reload_client_state();
  EXPECT_EQ(nvhttp::get_all_clients().size(), 1);
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(credentials.x509));
  const auto repaired = http::unique_id;
  ASSERT_TRUE(uuid_util::uuid_t::valid_text(repaired));

  std::ifstream in(state_file);
  const auto saved = nlohmann::json::parse(in);
  EXPECT_EQ(saved.at("username"), "admin");
  EXPECT_EQ(saved.at("root").at("uniqueid"), repaired);
  EXPECT_EQ(saved.at("root").at("named_devices").size(), 1U);

  nvhttp::test_support::reload_client_state();
  EXPECT_EQ(http::unique_id, repaired);
}

// Bugs caught: 0, False positives: 0
TEST_F(ClientAuthorizationTest, StreamAdmissionSeesRevocation) {
  // RTSP checks the client again once its stream session is registered (cmd_announce()).
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto uuid = nvhttp::test_support::add_client("streamer", credentials.x509, true);
  ASSERT_FALSE(uuid.empty());
  EXPECT_TRUE(nvhttp::client_may_stream(credentials.x509));
  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, false));
  EXPECT_FALSE(nvhttp::client_may_stream(credentials.x509));
  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, true));
  ASSERT_TRUE(nvhttp::unpair_client(uuid));
  EXPECT_FALSE(nvhttp::client_may_stream(credentials.x509));
  EXPECT_FALSE(nvhttp::client_may_stream(""));
}

/**
 * @brief Paired-client tests whose state file can't be saved to, with an app running as if a client streamed it.
 */
class UnsavableClientStateTest: public ClientAuthorizationTest {
protected:
  void SetUp() override {
    ClientAuthorizationTest::SetUp();
    saved_proc = std::move(proc::proc);
    saved_input_only_mode = config::input.enable_input_only_mode;
    saved_ignore_probe = config::video.ignore_encoder_probe_failure;
    config::input.enable_input_only_mode = false;
    // No display here: the launch's encoder probe fails.
    config::video.ignore_encoder_probe_failure = true;

    credentials = test_utils::certificates::generate_ca_credentials();
    other_credentials = test_utils::certificates::generate_ca_credentials("Sunshine Other Client");
    // The first client paired gets every permission; the client under test should have the default ones.
    first_uuid = nvhttp::test_support::add_client("first", test_utils::certificates::generate_ca_credentials("Sunshine First Client").x509, true);
    ASSERT_FALSE(first_uuid.empty());
    uuid = nvhttp::test_support::add_client("kept", credentials.x509, true);
    ASSERT_FALSE(uuid.empty());

    // A state file that isn't a JSON object can't be saved to (it may hold other settings, so it is left alone).
    std::ofstream out(state_file);
    out << "[1]";
  }

  void TearDown() override {
    proc::proc.terminate();
    proc::proc = std::move(saved_proc);
    config::input.enable_input_only_mode = saved_input_only_mode;
    config::video.ignore_encoder_probe_failure = saved_ignore_probe;
    ClientAuthorizationTest::TearDown();
  }

  /**
   * @brief Start an app, as a client's launch does; revoking the last streaming client terminates it.
   */
  void run_app() {
    std::vector<proc::ctx_t> apps;
    proc::ctx_t app;
    app.name = "Desktop";
    app.id = "42";
    apps.push_back(std::move(app));
    proc::proc = proc::proc_t {boost::this_process::environment(), std::move(apps)};
    auto launch_session = std::make_shared<rtsp_stream::launch_session_t>();
    launch_session->width = 1920;
    launch_session->height = 1080;
    launch_session->fps = 60000;
    ASSERT_EQ(proc::proc.execute(42, launch_session), 0);
    ASSERT_EQ(proc::proc.running(), 42);
  }

  /**
   * @brief The state file still holds what the test wrote.
   */
  void expect_state_file_untouched() {
    std::ifstream in(state_file);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(in), {}), "[1]");
  }

  crypto::creds_t credentials;  ///< The paired client.
  crypto::creds_t other_credentials;  ///< A client pairing during the test.
  std::string uuid;  ///< UUID of the paired client.
  std::string first_uuid;  ///< UUID of the first paired client, which has every permission.

private:
  proc::proc_t saved_proc;
  bool saved_input_only_mode = false;
  bool saved_ignore_probe = false;
};

// Bugs caught: 0, False positives: 0
TEST_F(UnsavableClientStateTest, UnpairCutsTheClientOffAndReportsTheFailedSave) {
  // An admin who can't write the disk must still be able to cut a client off.
  run_app();
  const auto change = nvhttp::unpair_client(uuid);
  EXPECT_FALSE(change);
  EXPECT_TRUE(change.applied);
  EXPECT_FALSE(change.saved);
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(credentials.x509));
  EXPECT_EQ(nvhttp::get_all_clients().size(), 1);
  EXPECT_EQ(proc::proc.running(), 42);

  // Unpairing the last client stops the streams and, with nobody paired, the app.
  EXPECT_TRUE(nvhttp::unpair_client(first_uuid).applied);
  EXPECT_EQ(nvhttp::get_all_clients().size(), 0);
  EXPECT_EQ(proc::proc.running(), 0);
  expect_state_file_untouched();
}

// Bugs caught: 0, False positives: 0
TEST_F(UnsavableClientStateTest, DisableCutsTheClientOffAndReportsTheFailedSave) {
  run_app();
  const auto change = nvhttp::set_client_enabled(uuid, false);
  EXPECT_FALSE(change);
  EXPECT_TRUE(change.applied);
  EXPECT_FALSE(change.saved);
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(credentials.x509));
  EXPECT_FALSE(nvhttp::client_may_stream(credentials.x509));
  EXPECT_EQ(proc::proc.running(), 0);

  // Taking permissions away is a revocation too; unpairing everyone is one.
  EXPECT_FALSE(nvhttp::set_client_enabled(uuid, true).applied);
  const auto removed = nvhttp::update_device_info(uuid, "kept", "", {}, {}, crypto::PERM::list, true, true, false);
  EXPECT_TRUE(removed.applied);
  EXPECT_FALSE(removed.saved);
  EXPECT_EQ(nvhttp::get_all_clients().at(1).at("perm"), static_cast<std::uint32_t>(crypto::PERM::list));
  const auto erased = nvhttp::erase_all_clients();
  EXPECT_TRUE(erased.applied);
  EXPECT_FALSE(erased.saved);
  EXPECT_EQ(nvhttp::get_all_clients().size(), 0);
  expect_state_file_untouched();
}

// Bugs caught: 0, False positives: 0
TEST_F(UnsavableClientStateTest, GrantsThatCantBeSavedAreUndone) {
  // A grant that only lived in memory would vanish on restart while the Web UI reported success.
  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, false).applied);  // in effect, not saved

  const auto enabled = nvhttp::set_client_enabled(uuid, true);
  EXPECT_FALSE(enabled.applied);
  EXPECT_FALSE(enabled.saved);
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(credentials.x509));

  // Adding a permission, or a mixed edit (rename while removing one), is undone.
  const auto granted = nvhttp::update_device_info(uuid, "kept", "", {}, {}, crypto::PERM::_all, true, true, false);
  EXPECT_FALSE(granted.applied);
  const auto mixed = nvhttp::update_device_info(uuid, "renamed", "", {}, {}, crypto::PERM::list, true, true, false);
  EXPECT_FALSE(mixed.applied);
  EXPECT_EQ(nvhttp::get_all_clients().at(1).at("name"), "kept");
  EXPECT_EQ(nvhttp::get_all_clients().at(1).at("perm"), static_cast<std::uint32_t>(crypto::PERM::_default));

  // A new pairing is undone.
  EXPECT_TRUE(nvhttp::test_support::add_client("new", other_credentials.x509, true).empty());
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(other_credentials.x509));
  expect_state_file_untouched();
}

// Bugs caught: 0, False positives: 0
TEST_F(UnsavableClientStateTest, RemovingClientCommandsStaysInEffect) {
  const nvhttp::cmd_list_t commands {{"first", true}, {"second", false}, {"first", true}};
  {
    std::ofstream out(state_file);
    out << "{}";
  }
  ASSERT_TRUE(nvhttp::update_device_info(uuid, "kept", "", commands, commands, crypto::PERM::_default, true, true, false));
  {
    std::ofstream out(state_file);
    out << "[1]";
  }
  const auto commands_of = [this](const char *key) {
    for (const auto &client : nvhttp::get_all_clients()) {
      if (client.at("uuid") == uuid) {
        return client.value(key, nlohmann::json::array());
      }
    }
    return nlohmann::json {};
  };

  // Adding, reordering or elevating a command grants something and is undone.
  const nvhttp::cmd_list_t added {{"first", true}, {"second", false}, {"first", true}, {"third", false}};
  const nvhttp::cmd_list_t reordered {{"second", false}, {"first", true}, {"first", true}};
  const nvhttp::cmd_list_t elevated {{"first", true}, {"second", true}, {"first", true}};
  for (const auto &edit : {added, reordered, elevated}) {
    EXPECT_FALSE(nvhttp::update_device_info(uuid, "kept", "", edit, commands, crypto::PERM::_default, true, true, false).applied);
    EXPECT_FALSE(nvhttp::update_device_info(uuid, "kept", "", commands, edit, crypto::PERM::_default, true, true, false).applied);
  }
  EXPECT_EQ(commands_of("do").size(), 3);

  // Removing one of two duplicates, or dropping elevation, only takes something away and stays in effect.
  const nvhttp::cmd_list_t removed {{"second", false}, {"first", false}};
  const auto change = nvhttp::update_device_info(uuid, "kept", "", removed, {}, crypto::PERM::_default, true, true, false);
  EXPECT_TRUE(change.applied);
  EXPECT_FALSE(change.saved);
  ASSERT_EQ(commands_of("do").size(), 2);
  EXPECT_EQ(commands_of("do").at(1).at("elevated"), false);
  EXPECT_TRUE(commands_of("undo").empty());
  expect_state_file_untouched();
}

// Bugs caught: 0, False positives: 0
TEST_F(UnsavableClientStateTest, RetryingAnUnpairRetriesItsSave) {
  ASSERT_TRUE(nvhttp::unpair_client(uuid).applied);  // in effect, not saved

  // Still unsavable: the retry reports it.
  const auto still_unsaved = nvhttp::unpair_client(uuid);
  EXPECT_TRUE(still_unsaved.applied);
  EXPECT_FALSE(still_unsaved.saved);
  expect_state_file_untouched();

  {
    std::ofstream out(state_file);
    out << "{}";
  }
  EXPECT_TRUE(nvhttp::unpair_client(uuid));
  nvhttp::test_support::reset_client_state();
  nvhttp::test_support::reload_client_state();
  ASSERT_EQ(nvhttp::get_all_clients().size(), 1);
  EXPECT_EQ(nvhttp::get_all_clients().at(0).at("uuid"), first_uuid);

  // With everything saved, an unknown client is just not found.
  EXPECT_FALSE(nvhttp::unpair_client(uuid).applied);
}

// Bugs caught: 0, False positives: 0
TEST_F(ClientAuthorizationTest, InaccessibleStateDirectoryIsAFailedSave) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto uuid = nvhttp::test_support::add_client("paired", credentials.x509, true);
  ASSERT_FALSE(uuid.empty());

  // A state file whose directory can't be searched: checking for it fails rather than finding nothing.
  const auto locked_dir = fs::path {SUNSHINE_TEST_BIN_DIR} / "client_authorization_locked";
  std::error_code ignored;
  fs::remove_all(locked_dir, ignored);
  fs::create_directories(locked_dir);
  config::nvhttp.file_state = (locked_dir / "state.json").string();
  fs::permissions(locked_dir, fs::perms::none);
  std::error_code probe_error;
  fs::exists(config::nvhttp.file_state, probe_error);
  if (!probe_error) {
    fs::permissions(locked_dir, fs::perms::owner_all);
    fs::remove_all(locked_dir, ignored);
    GTEST_SKIP() << "Directory permissions are not enforced here (running as root?)";
  }

  nvhttp::client_change_t change;
  EXPECT_NO_THROW(change = nvhttp::unpair_client(uuid));
  EXPECT_TRUE(change.applied);
  EXPECT_FALSE(change.saved);
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(credentials.x509));
  EXPECT_NO_THROW(change = nvhttp::erase_all_clients());
  EXPECT_FALSE(change.saved);

  fs::permissions(locked_dir, fs::perms::owner_all);
  fs::remove_all(locked_dir, ignored);
}

// Bugs caught: 0, False positives: 0
TEST_F(ClientAuthorizationTest, FreshStateNeverWritesTheStateFile) {
  const std::string persistent = R"({"root":{"uniqueid":"kept","named_devices":[]},"credentials":"kept"})";
  {
    std::ofstream out(state_file);
    out << persistent;
  }
  config::sunshine.flags[config::flag::FRESH_STATE] = true;

  // A temporary client pairs, is edited, disabled and unpaired without touching the persistent pairings.
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto uuid = nvhttp::test_support::add_client("temporary", credentials.x509, true);
  ASSERT_FALSE(uuid.empty());
  EXPECT_TRUE(nvhttp::update_device_info(uuid, "renamed", "", {}, {}, crypto::PERM::_all, true, true, false));
  EXPECT_TRUE(nvhttp::set_client_enabled(uuid, false));
  EXPECT_TRUE(nvhttp::set_client_enabled(uuid, true));
  EXPECT_TRUE(nvhttp::unpair_client(uuid));
  EXPECT_TRUE(nvhttp::erase_all_clients());

  std::ifstream in(state_file);
  EXPECT_EQ(std::string(std::istreambuf_iterator<char>(in), {}), persistent);
}

// Bugs caught: 0, False positives: 0
TEST(ClientChangeTest, ASuccessfulSecondSaveHoldsARevocationThatStayedInEffect) {
  const nvhttp::client_change_t saved {.applied = true, .saved = true};
  const nvhttp::client_change_t kept_unsaved {.applied = true, .saved = false};
  const nvhttp::client_change_t undone {.applied = false, .saved = false};

  // The revocation's first save failed, the disable's save wrote the whole state: both are saved.
  EXPECT_TRUE(nvhttp::combine_client_changes(kept_unsaved, false, saved));
  EXPECT_TRUE(nvhttp::combine_client_changes(kept_unsaved, true, saved));
  // A grant that was undone stays reported as not made, even beside a disable that took effect and was saved.
  const auto disable_beside_undone = nvhttp::combine_client_changes(undone, false, saved);
  EXPECT_TRUE(disable_beside_undone.applied);
  EXPECT_FALSE(disable_beside_undone.saved);
  // Enabling after an undone grant isn't attempted by the API; if it were, nothing is in effect.
  EXPECT_FALSE(nvhttp::combine_client_changes(undone, true, saved).applied);
  // Both saves failing is a failure to save.
  const auto both_unsaved = nvhttp::combine_client_changes(kept_unsaved, false, kept_unsaved);
  EXPECT_TRUE(both_unsaved.applied);
  EXPECT_FALSE(both_unsaved.saved);
  EXPECT_TRUE(nvhttp::combine_client_changes(saved, true, saved));
}

// Bugs caught: 0, False positives: 0
TEST_F(ClientAuthorizationTest, NonObjectStateFileDoesNotStopStartup) {
  for (const auto *contents : {"[1]", "null", "42"}) {
    SCOPED_TRACE(contents);
    {
      std::ofstream out(state_file);
      out << contents;
    }
    ASSERT_NO_THROW(nvhttp::test_support::reload_client_state());
    EXPECT_TRUE(uuid_util::uuid_t::valid_text(http::unique_id));
    std::ifstream in(state_file);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(in), {}), contents);
  }
}

// Bugs caught: 0, False positives: 0
TEST_F(ClientAuthorizationTest, InvalidUtf8DeviceNameStillPairsAndSaves) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto other_credentials = test_utils::certificates::generate_ca_credentials("Sunshine Other Client");
  const auto uuid = nvhttp::test_support::add_client("Phone \xff\xfe", credentials.x509, true);
  ASSERT_FALSE(uuid.empty());
  EXPECT_EQ(nvhttp::get_all_clients().at(0).at("name"), "Phone \xEF\xBF\xBD\xEF\xBF\xBD");
  // Later saves still work.
  EXPECT_FALSE(nvhttp::test_support::add_client("other", other_credentials.x509, true).empty());
  nvhttp::test_support::reset_client_state();
  nvhttp::test_support::reload_client_state();
  EXPECT_EQ(nvhttp::get_all_clients().size(), 2);
}
