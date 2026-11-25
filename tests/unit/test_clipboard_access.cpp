/**
 * @file tests/unit/test_clipboard_access.cpp
 * @brief Test the permission gate of the clipboard endpoint (nvhttp::clipboard_access).
 * @details Only the decision is tested; the handlers call platf::get_clipboard / set_clipboard after it, which
 *          must not run here (they would use the desktop session's clipboard).
 */
#include "../tests_common.h"

#include <src/nvhttp.h>

using crypto::PERM;
using SimpleWeb::StatusCode;

namespace {
  constexpr PERM operator|(PERM a, PERM b) {
    return static_cast<PERM>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
  }

  crypto::named_cert_t client(PERM perm) {
    crypto::named_cert_t cert {};
    cert.name = "test client";
    cert.uuid = "4D7BB2DD-5704-A405-B41C-891A022932E1";
    cert.perm = perm;
    return cert;
  }

  const std::list<std::string> streaming {"0000-OTHER", "4D7BB2DD-5704-A405-B41C-891A022932E1"};
}  // namespace

// Bugs caught: 0, False positives: 0
TEST(ClipboardAccess, ReadNeedsClipboardReadPermission) {
  EXPECT_EQ(nvhttp::clipboard_access(client(PERM::_default), false, "text", streaming), StatusCode::client_error_unauthorized);
  // clipboard_set alone does not allow reading.
  EXPECT_EQ(nvhttp::clipboard_access(client(PERM::_default | PERM::clipboard_set), false, "text", streaming), StatusCode::client_error_unauthorized);
  EXPECT_EQ(nvhttp::clipboard_access(client(PERM::_default | PERM::clipboard_read), false, "text", streaming), StatusCode::success_ok);
}

// Bugs caught: 0, False positives: 0
TEST(ClipboardAccess, SetNeedsClipboardSetPermission) {
  EXPECT_EQ(nvhttp::clipboard_access(client(PERM::_default), true, "text", streaming), StatusCode::client_error_unauthorized);
  EXPECT_EQ(nvhttp::clipboard_access(client(PERM::_default | PERM::clipboard_read), true, "text", streaming), StatusCode::client_error_unauthorized);
  EXPECT_EQ(nvhttp::clipboard_access(client(PERM::_default | PERM::clipboard_set), true, "text", streaming), StatusCode::success_ok);
}

// Bugs caught: 0, False positives: 0
TEST(ClipboardAccess, ClipboardPermissionWithoutViewIsRefused) {
  auto ops = PERM::clipboard_read | PERM::clipboard_set | PERM::list;
  EXPECT_EQ(nvhttp::clipboard_access(client(ops), false, "text", streaming), StatusCode::client_error_unauthorized);
  EXPECT_EQ(nvhttp::clipboard_access(client(ops), true, "text", streaming), StatusCode::client_error_unauthorized);
  // launch counts as view.
  EXPECT_EQ(nvhttp::clipboard_access(client(ops | PERM::launch), false, "text", streaming), StatusCode::success_ok);
  EXPECT_EQ(nvhttp::clipboard_access(client(PERM::_all), true, "text", streaming), StatusCode::success_ok);
}

// Bugs caught: 0, False positives: 0
TEST(ClipboardAccess, OnlyTextIsSupported) {
  auto perm = PERM::_all;
  EXPECT_EQ(nvhttp::clipboard_access(client(perm), false, "image", streaming), StatusCode::client_error_bad_request);
  EXPECT_EQ(nvhttp::clipboard_access(client(perm), true, "", streaming), StatusCode::client_error_bad_request);
}

// Bugs caught: 0, False positives: 0
TEST(ClipboardAccess, ClientMustBeStreaming) {
  auto perm = PERM::_all;
  EXPECT_EQ(nvhttp::clipboard_access(client(perm), false, "text", {}), StatusCode::client_error_forbidden);
  EXPECT_EQ(nvhttp::clipboard_access(client(perm), true, "text", {"0000-OTHER"}), StatusCode::client_error_forbidden);
}

// Bugs caught: 0, False positives: 0
TEST(ClipboardAccess, PermissionIsCheckedBeforeEverythingElse) {
  // A client without permission learns nothing about types or streaming state.
  EXPECT_EQ(nvhttp::clipboard_access(client(PERM::_no), false, "image", {}), StatusCode::client_error_unauthorized);
  EXPECT_EQ(nvhttp::clipboard_access(client(PERM::_default), true, "image", {}), StatusCode::client_error_unauthorized);
}
