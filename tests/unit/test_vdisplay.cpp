/**
 * @file tests/unit/test_vdisplay.cpp
 * @brief Test the platform-neutral virtual display rules in src/process.* and src/platform/vdisplay.h.
 */
#include "../tests_common.h"

#include <src/platform/vdisplay.h>
#include <src/process.h>
#include <src/rtsp.h>
#include <src/uuid.h>

namespace {
  const std::string client_uuid_str = "0123ABCD-4567-89EF-0011-223344556677";
  const std::string app_uuid_str = "F0E1D2C3-B4A5-9687-7869-5A4B3C2D1E0F";
  const std::string other_client_uuid_str = "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE";

  proc::ctx_t make_app(bool use_app_identity, bool per_client_app_identity) {
    proc::ctx_t app {};
    app.name = "Some Game";
    app.uuid = app_uuid_str;
    app.use_app_identity = use_app_identity;
    app.per_client_app_identity = per_client_app_identity;
    return app;
  }

  rtsp_stream::launch_session_t make_session(const std::string &unique_id = client_uuid_str) {
    rtsp_stream::launch_session_t session {};
    session.device_name = "Living Room";
    session.unique_id = unique_id;
    return session;
  }
}  // namespace

struct VdisplayRefreshTest: testing::TestWithParam<std::tuple<int, bool, int>> {};

// Bugs caught: 0, False positives: 0
TEST_P(VdisplayRefreshTest, Run) {
  auto [client_fps, double_refreshrate, expected] = GetParam();
  EXPECT_EQ(proc::vdisplay_refresh_millihz(client_fps, double_refreshrate), expected);
}

INSTANTIATE_TEST_SUITE_P(
  VdisplayTests,
  VdisplayRefreshTest,
  testing::Values(
    // No fps from the client means 60 Hz.
    std::make_tuple(0, false, 60000),
    std::make_tuple(0, true, 120000),
    // Values below 1000 are Hz.
    std::make_tuple(60, false, 60000),
    std::make_tuple(999, false, 999000),
    std::make_tuple(120, true, 240000),
    // Values from 1000 on are already mHz.
    std::make_tuple(1000, false, 1000),
    std::make_tuple(59940, false, 59940),
    std::make_tuple(60000, true, 120000),
    std::make_tuple(143856, false, 143856)
  )
);

// Bugs caught: 0, False positives: 0
TEST(VdisplayIdentityTest, ClientIdentityByDefault) {
  const auto app = make_app(false, true);  // per_client_app_identity alone has no effect
  const auto session = make_session();

  const auto identity = proc::vdisplay_identity(app, session);

  EXPECT_EQ(identity.name, "Living Room");
  EXPECT_EQ(identity.uuid_str, client_uuid_str);
  EXPECT_EQ(identity.uuid, uuid_util::uuid_t::parse(client_uuid_str));
}

// Bugs caught: 0, False positives: 0
TEST(VdisplayIdentityTest, AppIdentity) {
  const auto app = make_app(true, false);
  const auto session = make_session();

  const auto identity = proc::vdisplay_identity(app, session);

  EXPECT_EQ(identity.name, "Some Game");
  EXPECT_EQ(identity.uuid_str, app_uuid_str);
  EXPECT_EQ(identity.uuid, uuid_util::uuid_t::parse(app_uuid_str));

  // Every client gets the same display for the app.
  EXPECT_EQ(proc::vdisplay_identity(app, make_session(other_client_uuid_str)).uuid, identity.uuid);
}

// Bugs caught: 0, False positives: 0
TEST(VdisplayIdentityTest, PerClientAppIdentityXorsClientAndApp) {
  const auto app = make_app(true, true);
  const auto session = make_session();

  const auto identity = proc::vdisplay_identity(app, session);

  const auto client_uuid = uuid_util::uuid_t::parse(client_uuid_str);
  const auto app_uuid = uuid_util::uuid_t::parse(app_uuid_str);
  uuid_util::uuid_t expected;
  for (int i = 0; i < 16; ++i) {
    expected.b8[i] = client_uuid.b8[i] ^ app_uuid.b8[i];
  }

  EXPECT_EQ(identity.name, "Some Game");
  EXPECT_EQ(identity.uuid, expected);
  EXPECT_EQ(identity.uuid_str, expected.string());
  EXPECT_NE(identity.uuid, client_uuid);
  EXPECT_NE(identity.uuid, app_uuid);

  // A different client gets a different display for the same app.
  EXPECT_NE(proc::vdisplay_identity(app, make_session(other_client_uuid_str)).uuid, identity.uuid);
}

// Bugs caught: 0, False positives: 0
TEST(VdisplayIdentityTest, RejectsMalformedClientUuid) {
  const auto app = make_app(false, false);
  EXPECT_THROW(proc::vdisplay_identity(app, make_session("not-a-uuid")), std::invalid_argument);
}

// Bugs caught: 0, False positives: 0
TEST(VdisplayStatusTest, KeepsWebUiStatusCodes) {
  // config.html / AudioVideo.vue index their messages by these SudoVDA driver codes.
  using platf::vdisplay::status_e;
  EXPECT_EQ(static_cast<int>(status_e::ok), 0);
  EXPECT_EQ(static_cast<int>(status_e::unknown), 1);
  EXPECT_EQ(static_cast<int>(status_e::failed), -1);
  EXPECT_EQ(static_cast<int>(status_e::version_incompatible), -2);
  EXPECT_EQ(static_cast<int>(status_e::watchdog_failed), -3);
}

// The stub is only built where no real backend exists (macOS, Linux without Wayland). Linux with Wayland uses the
// Hyprland backend, tested in tests/unit/platform/test_hyprland_vdisplay.cpp.
#if !defined(_WIN32) && !defined(SUNSHINE_BUILD_WAYLAND)
// Bugs caught: 0, False positives: 0
TEST(VdisplayStubTest, ReportsUnavailable) {
  EXPECT_EQ(platf::vdisplay::init(), platf::vdisplay::status_e::unavailable);
  EXPECT_EQ(platf::vdisplay::status(), platf::vdisplay::status_e::unavailable);
  EXPECT_EQ(platf::vdisplay::create(uuid_util::uuid_t::parse(client_uuid_str), "Living Room", {1920, 1080, 60000}), "");
  EXPECT_FALSE(platf::vdisplay::get_hdr("HEADLESS-1").has_value());
}

// Bugs caught: 0, False positives: 0
TEST(VdisplayStubTest, SessionDoesNotCreateDisplay) {
  const auto app = make_app(false, false);
  auto session = make_session();
  session.virtual_display = true;  // the client asked for one
  session.fps = 60000;

  EXPECT_EQ(proc::create_virtual_display(app, session, 1920, 1080), "");

  // Nothing to remove on terminate.
  EXPECT_FALSE(session.virtual_display);
  EXPECT_EQ(session.display_guid, uuid_util::uuid_t {});
}
#endif
