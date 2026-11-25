/**
 * @file tests/unit/platform/test_hyprland_display_settings.cpp
 * @brief Test the pure logic of per-client virtual display settings and window placement
 *        (src/platform/linux/hyprland/display_settings.*).
 */
#include "../../tests_common.h"

#ifdef SUNSHINE_BUILD_WAYLAND

  // standard includes
  #include <fstream>
  #include <map>

  // local includes
  #include "hyprland_fakes.h"
  #include <src/platform/linux/hyprland/display_settings.h>

namespace {
  using namespace hyprland;

  monitor_info_t monitor(double scale = 1.0, int x = 2560, int y = 0) {
    monitor_info_t m;
    m.name = "APOLLO-1a2b3c4d";
    m.width = 2560;
    m.height = 1600;
    m.refresh_hz = 120.0;
    m.x = x;
    m.y = y;
    m.scale = scale;
    m.current_format = "XRGB8888";
    m.active_workspace = "apollo-1a2b3c4d";
    return m;
  }
}  // namespace

// Bugs caught: 0, False positives: 0
TEST(HyprlandDisplaySettingsTest, JsonRoundTripAndValidation) {
  display_settings_t s;
  s.client_name = "Living Room";
  s.scale = 1.6000000000000001;
  s.scale_percent = 150;
  s.transform = 1;
  s.bitdepth = 10;
  s.position = "0x-1600";
  s.workspace = "apollo-1a2b3c4d";
  auto json = settings_to_json(s);
  EXPECT_EQ(json["scale"].get<double>(), 1.6);
  EXPECT_EQ(json["scale_factor"], 150);
  auto back = settings_from_json(json);
  back.scale = s.scale;  // rounded in the file
  EXPECT_EQ(back, s);

  // Wrong types and out-of-range values are dropped, not trusted.
  auto bad = settings_from_json(nlohmann::json::parse(R"({"scale": "2", "transform": 9, "bitdepth": 12, "position": "", "workspace": 3, "scale_factor": -5})"));
  EXPECT_TRUE(bad.empty());
  EXPECT_TRUE(settings_from_json(nlohmann::json::array()).empty());

  display_settings_t named_only;
  named_only.client_name = "x";
  EXPECT_TRUE(named_only.empty());
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandDisplaySettingsTest, StoreKeepsOtherEntriesAndRemovesEmptyOnes) {
  hyprland_fakes::temp_dir_t tmp;
  const auto file = tmp.path() / "sub" / "vdisplay_settings.json";
  display_settings_store_t store {file};

  EXPECT_TRUE(store.load("APOLLO-1a2b3c4d").empty());

  display_settings_t a;
  a.client_name = "A";
  a.scale = 2.0;
  display_settings_t b;
  b.transform = 3;
  ASSERT_TRUE(store.save("APOLLO-1a2b3c4d", a));
  ASSERT_TRUE(store.save("APOLLO-0000beef", b));
  EXPECT_EQ(store.load("APOLLO-1a2b3c4d"), a);
  EXPECT_EQ(store.load("APOLLO-0000beef"), b);

  {
    std::ifstream in {file};
    auto json = nlohmann::json::parse(in);
    EXPECT_EQ(json["version"], 1);
    EXPECT_EQ(json["displays"]["APOLLO-1a2b3c4d"]["scale"], 2.0);
  }

  // Saving empty settings removes the entry, and leaves the others alone.
  ASSERT_TRUE(store.save("APOLLO-1a2b3c4d", display_settings_t {}));
  EXPECT_TRUE(store.load("APOLLO-1a2b3c4d").empty());
  EXPECT_EQ(store.load("APOLLO-0000beef"), b);
  // No temporary files left behind.
  EXPECT_EQ(std::distance(std::filesystem::directory_iterator {file.parent_path()}, std::filesystem::directory_iterator {}), 1);

  // A corrupt file reads as empty and is replaced on the next save.
  std::ofstream {file} << "{not json";
  EXPECT_TRUE(store.load("APOLLO-0000beef").empty());
  ASSERT_TRUE(store.save("APOLLO-0000beef", b));
  EXPECT_EQ(store.load("APOLLO-0000beef"), b);

  // No path: nothing is remembered.
  display_settings_store_t none {""};
  EXPECT_FALSE(none.save("APOLLO-0000beef", b));
  EXPECT_TRUE(none.load("APOLLO-0000beef").empty());
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandDisplaySettingsTest, WorkspaceNamesAndSelectors) {
  EXPECT_EQ(default_workspace_name("APOLLO-1a2b3c4d"), "apollo-1a2b3c4d");
  EXPECT_EQ(workspace_selector("apollo-1a2b3c4d"), "name:apollo-1a2b3c4d");
  EXPECT_EQ(workspace_selector("3"), "3");
  EXPECT_EQ(workspace_selector("3a"), "name:3a");

  EXPECT_EQ(bitdepth_of_format("XRGB2101010"), 10);
  EXPECT_EQ(bitdepth_of_format("XBGR2101010"), 10);
  EXPECT_EQ(bitdepth_of_format("XRGB8888"), 8);
  EXPECT_EQ(bitdepth_of_format(""), std::nullopt);
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandDisplaySettingsTest, ScaleFactorAndRememberedScale) {
  display_settings_t none;
  EXPECT_EQ(choose_scale(none, 100), std::nullopt);
  EXPECT_EQ(choose_scale(none, 0), std::nullopt);
  EXPECT_EQ(choose_scale(none, 150), 1.5);
  EXPECT_EQ(choose_scale(none, 50), 0.5);

  display_settings_t learned;
  learned.scale = 2.0;  // learned without a scale factor (old entries have no scale_factor either)
  EXPECT_EQ(choose_scale(learned, 100), 2.0);
  EXPECT_EQ(choose_scale(learned, 150), 1.5) << "a different scale factor wins over the remembered scale";

  learned.scale_percent = 150;
  learned.scale = 1.25;
  EXPECT_EQ(choose_scale(learned, 150), 1.25);
  EXPECT_EQ(choose_scale(learned, 100), std::nullopt);
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandDisplaySettingsTest, BuildsTheMonitorRule) {
  rule_inputs_t in {.output = "APOLLO-1a2b3c4d", .mode = {2560, 1600, 120000}};
  auto rule = build_monitor_rule(in);
  EXPECT_EQ(lua_monitor_call(rule), R"(hl.monitor({ output = "APOLLO-1a2b3c4d", mode = "2560x1600@120.000", position = "auto-right", scale = 1 }))")
    << "without settings: scale 1 (not Hyprland's resolution-based pick), transform and bit depth left to static rules";

  in.settings.scale = 2.0;
  in.settings.transform = 1;
  in.settings.bitdepth = 10;
  in.settings.position = "0x-1600";
  rule = build_monitor_rule(in);
  EXPECT_EQ(lua_monitor_call(rule), R"(hl.monitor({ output = "APOLLO-1a2b3c4d", mode = "2560x1600@120.000", position = "0x-1600", scale = 2, transform = 1, bitdepth = 10 }))");

  // Isolation overrides the remembered position.
  in.isolated_position = "5000x3000";
  EXPECT_EQ(build_monitor_rule(in).position, "5000x3000");

  // Fields set in an earlier session are reset rather than left to stick.
  rule_inputs_t later {.output = "APOLLO-1a2b3c4d", .mode = {1920, 1080, 60000}, .emitted = {.transform = true, .bitdepth = true}};
  rule = build_monitor_rule(later);
  EXPECT_EQ(rule.scale, 1.0);
  EXPECT_EQ(rule.transform, 0);
  EXPECT_EQ(rule.bitdepth, 8);

  // The scale factor comes in with the mode.
  rule_inputs_t factor {.output = "APOLLO-1a2b3c4d", .mode = {3840, 2160, 60000, 200}};
  EXPECT_EQ(build_monitor_rule(factor).scale, 2.0);
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandDisplaySettingsTest, LearnsOnlyWhatChangedDuringTheSession) {
  const display_settings_t stored;
  const auto baseline = monitor();

  // Nothing changed: nothing learned.
  EXPECT_EQ(learn_settings(stored, baseline, baseline, false, 100, "apollo-1a2b3c4d"), stored);

  // A static rule's value is in the baseline, so it is never learned; a change on top of it is, with the snapped
  // value Hyprland reports.
  auto now = monitor(1.6);
  auto learned = learn_settings(stored, monitor(1.25), now, false, 150, "apollo-1a2b3c4d");
  EXPECT_EQ(learned.scale, 1.6);
  EXPECT_EQ(learned.scale_percent, 150);
  EXPECT_FALSE(learned.position);

  now = monitor();
  now.transform = 3;
  now.current_format = "XRGB2101010";
  learned = learn_settings(stored, baseline, now, false, 100, "");
  EXPECT_EQ(learned.transform, 3);
  EXPECT_EQ(learned.bitdepth, 10);
  EXPECT_FALSE(learned.workspace) << "not activated: the workspace is not Apollo's";

  // Position only when not isolated.
  now = monitor(1.0, 0, -1600);
  EXPECT_EQ(learn_settings(stored, baseline, now, false, 100, "").position, "0x-1600");
  EXPECT_FALSE(learn_settings(stored, baseline, now, true, 100, "").position);

  // Workspace: another one left showing is remembered; the default clears it again.
  now = monitor();
  now.active_workspace = "7";
  learned = learn_settings(stored, baseline, now, false, 100, "apollo-1a2b3c4d");
  EXPECT_EQ(learned.workspace, "7");
  now.active_workspace = "apollo-1a2b3c4d";
  EXPECT_FALSE(learn_settings(learned, baseline, now, false, 100, "7").workspace);

  // Earlier settings are kept when other fields change.
  display_settings_t earlier;
  earlier.scale = 2.0;
  now = monitor();
  now.transform = 1;
  learned = learn_settings(earlier, baseline, now, false, 100, "");
  EXPECT_EQ(learned.scale, 2.0);
  EXPECT_EQ(learned.transform, 1);
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandDisplaySettingsTest, ParsesProcStat) {
  auto st = parse_proc_stat("1234 (my app) S 1000 1200 1000 0 -1 4194560 ...");
  ASSERT_TRUE(st);
  EXPECT_EQ(st->ppid, 1000);
  EXPECT_EQ(st->pgid, 1200);

  // A command name with ") " in it must not shift the fields.
  st = parse_proc_stat("77 (evil) S 9 9) R 55 66 1 0");
  ASSERT_TRUE(st);
  EXPECT_EQ(st->ppid, 55);
  EXPECT_EQ(st->pgid, 66);

  EXPECT_FALSE(parse_proc_stat(""));
  EXPECT_FALSE(parse_proc_stat("12 (x) S"));

  // This process, for real.
  auto self = read_proc_stat(::getpid());
  ASSERT_TRUE(self);
  EXPECT_EQ(self->ppid, ::getppid());
  EXPECT_EQ(self->pgid, ::getpgrp());
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandDisplaySettingsTest, MatchesWindowsToTheLaunchedProcessGroup) {
  // 100: the launched app, leader of group 100. 101: its child in the group. 102: a child that called setsid
  // (group 102) under 101. 200: some other app whose parent is the session (pid 1).
  const std::map<int, proc_stat_t> procs {
    {100, {50, 100}},
    {101, {100, 100}},
    {102, {101, 102}},
    {103, {102, 102}},
    {200, {1, 200}},
    {300, {301, 300}},
    {301, {300, 301}},  // a loop, as a racing /proc could show
  };
  auto read = [&](int pid) -> std::optional<proc_stat_t> {
    auto it = procs.find(pid);
    return it == procs.end() ? std::nullopt : std::optional {it->second};
  };
  EXPECT_TRUE(process_belongs_to_group(100, 100, read));
  EXPECT_TRUE(process_belongs_to_group(101, 100, read));
  EXPECT_TRUE(process_belongs_to_group(102, 100, read)) << "left the group but its parent is in it";
  EXPECT_TRUE(process_belongs_to_group(103, 100, read));
  EXPECT_FALSE(process_belongs_to_group(200, 100, read));
  EXPECT_FALSE(process_belongs_to_group(999, 100, read)) << "gone";
  EXPECT_FALSE(process_belongs_to_group(300, 100, read));
  EXPECT_FALSE(process_belongs_to_group(100, 0, read));
  EXPECT_FALSE(process_belongs_to_group(-1, 100, read));
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandDisplaySettingsTest, ParsesClientsAndOpenWindowEvents) {
  // Trimmed from Hyprland 0.57.2's j/clients.
  auto clients = parse_clients(nlohmann::json::parse(R"([
    {"address": "0x55c1c0dd7250", "mapped": true, "workspace": {"name": "apollo-1234abcd"}, "monitor": 2, "class": "wev", "pid": 1168079},
    {"address": "0x55c1c0dccf80", "workspace": {"id": 2, "name": "2"}, "monitor": 1, "pid": 1169334},
    "junk"
  ])"));
  ASSERT_EQ(clients.size(), 2u);
  EXPECT_EQ(clients[0].address, "0x55c1c0dd7250");
  EXPECT_EQ(clients[0].pid, 1168079);
  EXPECT_EQ(clients[0].workspace, "apollo-1234abcd");
  EXPECT_EQ(clients[0].monitor, 2);
  EXPECT_EQ(clients[1].workspace, "2");

  EXPECT_EQ(parse_openwindow_address("55c1c0dedc60,2,wev,wev"), "0x55c1c0dedc60");
  EXPECT_EQ(parse_openwindow_address("0x55c1c0dedc60,2,wev,a, title"), "0x55c1c0dedc60");
  EXPECT_EQ(parse_openwindow_address(",2,wev,wev"), std::nullopt);
  EXPECT_EQ(parse_openwindow_address("zz,2"), std::nullopt);
}

// Bugs caught: 0, False positives: 0
TEST(HyprlandDisplaySettingsTest, LuaCommands) {
  EXPECT_EQ(lua_show_workspace("APOLLO-1a2b3c4d", "apollo-1a2b3c4d"),
            R"(local m = hl.get_monitor("APOLLO-1a2b3c4d"); if not m then return "no monitor" end; m:set_workspace("name:apollo-1a2b3c4d"); return "ok")");
  EXPECT_EQ(lua_focus_monitor("APOLLO-1a2b3c4d"), R"(hl.dsp.focus({ monitor = "APOLLO-1a2b3c4d" }))");
  EXPECT_EQ(lua_focus_workspace("2"), R"(hl.dsp.focus({ workspace = "2" }))");
  EXPECT_EQ(lua_move_window("0xabc", "apollo-1a2b3c4d"), R"(hl.dsp.window.move({ workspace = "name:apollo-1a2b3c4d", window = "address:0xabc", follow = false }))");
  // Names are quoted, not pasted into the code.
  EXPECT_EQ(lua_focus_workspace("a\"b"), R"(hl.dsp.focus({ workspace = "name:a\"b" }))");
}

#endif
