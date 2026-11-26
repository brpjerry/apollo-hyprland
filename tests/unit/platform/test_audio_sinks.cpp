/**
 * @file tests/unit/platform/test_audio_sinks.cpp
 * @brief Tests for the Linux default-sink and stream policy (src/platform/linux/audio_sinks.*) with a fake server.
 */
#include "../../tests_common.h"

#ifdef __linux__

  #include <src/platform/linux/audio_sinks.h>

  #include <algorithm>

using namespace platf::pa;
using namespace std::literals;

namespace {
  /** An in-memory sound server: sinks, sink inputs and the default sink, with a log of every change. */
  struct fake_server_t: backend_t {
    std::vector<sink_desc_t> sink_list;
    std::vector<sink_input_desc_t> input_list;
    std::string default_name;
    std::vector<std::string> log;
    std::set<std::uint32_t> unmovable;
    /** Like pipewire-pulse + WirePlumber: a new default is reported only after this many more queries. */
    int default_lag = 0;
    std::string pending_default;
    int pending_reads = 0;
    /** Inputs without a target: they move along with the default, as with WirePlumber. A move onto the default
     *  sink makes an input follow the default, a move onto another sink pins it (pipewire-pulse). */
    std::set<std::uint32_t> followers;

    std::uint32_t add_sink(const std::string &name, const std::string &description = {}) {
      auto index = (std::uint32_t) sink_list.size() + 10;
      sink_list.push_back({index, name, description.empty() ? name : description});
      return index;
    }

    void remove_sink(const std::string &name) {
      std::erase_if(sink_list, [&](const auto &s) {
        return s.name == name;
      });
    }

    std::uint32_t sink_index(const std::string &name) const {
      for (const auto &s : sink_list) {
        if (s.name == name) {
          return s.index;
        }
      }
      return invalid_index;
    }

    void add_input(std::uint32_t index, const std::string &sink) {
      input_list.push_back({index, sink_index(sink), "input-" + std::to_string(index)});
    }

    sink_input_desc_t *input(std::uint32_t index) {
      for (auto &i : input_list) {
        if (i.index == index) {
          return &i;
        }
      }
      return nullptr;
    }

    std::string sink_of(std::uint32_t index) {
      for (const auto &s : sink_list) {
        if (s.index == input(index)->sink) {
          return s.name;
        }
      }
      return {};
    }

    /** What a session manager does on a default change: streams without a target follow it. */
    void follow_default(std::initializer_list<std::uint32_t> followers) {
      for (auto idx : followers) {
        input(idx)->sink = sink_index(default_name);
      }
    }

    std::optional<std::string> default_sink() override {
      if (pending_reads > 0 && --pending_reads == 0) {
        default_name = pending_default;
      }
      return default_name;
    }

    int set_default_sink(const std::string &name) override {
      if (sink_index(name) == invalid_index) {
        return -1;
      }
      if (default_lag > 0) {
        pending_default = name;
        pending_reads = default_lag;
      } else {
        default_name = name;
      }
      for (auto idx : followers) {
        if (auto i = input(idx)) {
          i->sink = sink_index(name);
        }
      }
      log.push_back("default " + name);
      return 0;
    }

    std::optional<std::vector<sink_desc_t>> sinks() override {
      return sink_list;
    }

    std::optional<std::vector<sink_input_desc_t>> sink_inputs() override {
      return input_list;
    }

    std::optional<sink_input_desc_t> sink_input(std::uint32_t index) override {
      if (auto i = input(index)) {
        return *i;
      }
      return std::nullopt;
    }

    int move_sink_input(std::uint32_t index, const std::string &sink) override {
      auto i = input(index);
      if (!i || unmovable.contains(index) || sink_index(sink) == invalid_index) {
        return -1;
      }
      i->sink = sink_index(sink);
      if (sink == default_name) {
        followers.insert(index);
      } else {
        followers.erase(index);
      }
      log.push_back("move " + std::to_string(index) + " " + sink);
      return 0;
    }
  };

  struct AudioSinkManagerTest: testing::Test {
    void SetUp() override {
      server.add_sink("speakers", "Speakers");
      server.add_sink("headset", "Headset");
      server.add_sink("sink-sunshine-stereo");
      server.add_sink("sink-sunshine-surround51");
      server.default_name = "speakers";
      manager.set_mute_sinks({"sink-sunshine-stereo", "sink-sunshine-surround51"});
      manager.prime();
    }

    std::uint64_t generation() const {
      return manager.capture_generation()->load();
    }

    fake_server_t server;
    sink_manager_t manager {server};
  };
}  // namespace

// Bugs caught: 0, False positives: 0
TEST_F(AudioSinkManagerTest, MuteMovesPinnedStreamsAndRestoresThem) {
  server.add_input(1, "speakers");  // follows the default
  server.add_input(2, "headset");  // pinned to the headset
  server.add_input(3, "speakers");  // pinned to the speakers

  ASSERT_EQ(manager.set_sink("sink-sunshine-stereo"), 0);
  EXPECT_TRUE(manager.muted());
  // The default switches before anything moves, so moved streams are not pinned to the virtual sink.
  ASSERT_FALSE(server.log.empty());
  EXPECT_EQ(server.log.front(), "default sink-sunshine-stereo");
  EXPECT_EQ(server.sink_of(2), "sink-sunshine-stereo");
  EXPECT_EQ(server.sink_of(3), "sink-sunshine-stereo");

  server.log.clear();
  ASSERT_EQ(manager.set_sink("speakers"), 0);
  EXPECT_FALSE(manager.muted());
  EXPECT_EQ(server.log.front(), "default speakers");
  EXPECT_EQ(server.sink_of(2), "headset");
  EXPECT_EQ(server.sink_of(3), "speakers");
}

// Bugs caught: 0, False positives: 0
TEST_F(AudioSinkManagerTest, MuteOnlyMovesStreamsNotAlreadyOnTheMuteSink) {
  server.add_input(1, "speakers");
  ASSERT_EQ(manager.set_sink("sink-sunshine-stereo"), 0);
  // The session manager moved the follower before Apollo listed the streams.
  server.log.clear();
  server.add_input(4, "sink-sunshine-stereo");
  manager.on_sink_input_changed(4);
  EXPECT_TRUE(server.log.empty());

  ASSERT_EQ(manager.set_sink("speakers"), 0);
  // Input 4 was never moved by Apollo; the session manager takes it back to the default.
  EXPECT_EQ(std::ranges::count(server.log, "move 4 speakers"s), 0);
}

// Bugs caught: 0, False positives: 0
TEST_F(AudioSinkManagerTest, RestoreLeavesStreamsTheUserMovedAndStreamsWhoseSinkIsGone) {
  server.add_sink("hdmi");
  server.add_input(2, "headset");
  server.add_input(3, "headset");
  server.add_input(5, "speakers");
  ASSERT_EQ(manager.set_sink("sink-sunshine-stereo"), 0);

  // The user moves input 2 to the HDMI sink during the stream, and input 3's sink disappears.
  server.input(2)->sink = server.sink_index("hdmi");
  server.followers.erase(2);
  manager.on_sink_input_changed(2);
  EXPECT_EQ(server.sink_of(2), "hdmi") << "a stream the user moved must not be moved again";

  server.remove_sink("headset");
  server.log.clear();
  ASSERT_EQ(manager.set_sink("speakers"), 0);
  EXPECT_EQ(server.sink_of(2), "hdmi");
  // Input 3 followed the default back to the speakers; it stays there.
  EXPECT_EQ(server.sink_of(3), "speakers");
  EXPECT_EQ(server.sink_of(5), "speakers");
  EXPECT_EQ(server.log, (std::vector<std::string> {"default speakers", "move 5 speakers"}));
}

// Bugs caught: 0, False positives: 0
TEST_F(AudioSinkManagerTest, StreamsStartedWhileMutedAreMoved) {
  ASSERT_EQ(manager.set_sink("sink-sunshine-stereo"), 0);

  // A stream that is not linked yet is looked at again when it changes.
  server.input_list.push_back({7, invalid_index, "firefox"});
  manager.on_sink_input_changed(7);
  EXPECT_TRUE(server.input(7)->sink == invalid_index);

  // The session manager links it to its stored target.
  server.input(7)->sink = server.sink_index("headset");
  manager.on_sink_input_changed(7);
  EXPECT_EQ(server.sink_of(7), "sink-sunshine-stereo");

  ASSERT_EQ(manager.set_sink("speakers"), 0);
  EXPECT_EQ(server.sink_of(7), "headset");

  // Not muted any more: new streams stay where they are.
  server.add_input(8, "headset");
  manager.on_sink_input_changed(8);
  EXPECT_EQ(server.sink_of(8), "headset");
}

// Bugs caught: 0, False positives: 0
TEST_F(AudioSinkManagerTest, UnmovableStreamsAreSkipped) {
  server.add_input(2, "headset");
  server.add_input(3, "headset");
  server.unmovable.insert(2);
  ASSERT_EQ(manager.set_sink("sink-sunshine-stereo"), 0);
  EXPECT_EQ(server.sink_of(2), "headset");
  EXPECT_EQ(server.sink_of(3), "sink-sunshine-stereo");

  manager.unmute();
  EXPECT_FALSE(manager.muted());
  EXPECT_EQ(server.sink_of(3), "headset");
}

// Bugs caught: 0, False positives: 0
TEST_F(AudioSinkManagerTest, StreamsThatFollowedTheRestoredDefaultAreStillMovedBack) {
  server.add_input(2, "headset");
  server.add_input(1, "speakers");
  server.followers.insert(1);
  ASSERT_EQ(manager.set_sink("sink-sunshine-stereo"), 0);
  EXPECT_EQ(server.sink_of(2), "sink-sunshine-stereo");
  EXPECT_TRUE(server.followers.contains(2)) << "moved onto the default sink, so it follows the default now";

  // Switching the default back takes input 2 to the speakers before Apollo restores it.
  ASSERT_EQ(manager.set_sink("speakers"), 0);
  EXPECT_EQ(server.sink_of(2), "headset");
  EXPECT_FALSE(server.followers.contains(2)) << "pinned again, as it was";
  EXPECT_EQ(server.sink_of(1), "speakers");
  EXPECT_TRUE(server.followers.contains(1));
}

// Bugs caught: 0, False positives: 0
TEST_F(AudioSinkManagerTest, ADefaultThatIsAppliedLateIsNotMistakenForAChange) {
  server.default_lag = 3;
  ASSERT_EQ(manager.set_sink("sink-sunshine-stereo"), 0);
  EXPECT_EQ(server.default_name, "sink-sunshine-stereo") << "set_sink waits until the server reports the new default";

  auto gen = generation();
  manager.on_server_changed({.keep_default = false, .auto_capture = true});
  EXPECT_EQ(generation(), gen);
  EXPECT_TRUE(manager.muted());
  EXPECT_EQ(manager.capture_sink(""), "sink-sunshine-stereo");
}

// Bugs caught: 0, False positives: 0
TEST_F(AudioSinkManagerTest, KeepDefaultPutsTheVirtualSinkBack) {
  server.add_input(2, "headset");
  ASSERT_EQ(manager.set_sink("sink-sunshine-stereo"), 0);
  auto gen = generation();

  server.log.clear();
  server.default_name = "speakers";
  manager.on_server_changed({.keep_default = true, .auto_capture = true});
  EXPECT_EQ(server.default_name, "sink-sunshine-stereo");
  EXPECT_EQ(server.log, (std::vector<std::string> {"default sink-sunshine-stereo"}));
  EXPECT_TRUE(manager.muted());
  EXPECT_EQ(generation(), gen) << "capture does not need to follow a default that was put back";

  // The event for Apollo's own change is ignored.
  server.log.clear();
  manager.on_server_changed({.keep_default = true, .auto_capture = true});
  EXPECT_TRUE(server.log.empty());
}

// Bugs caught: 0, False positives: 0
TEST_F(AudioSinkManagerTest, KeepDefaultGivesUpWhenSomethingKeepsChangingIt) {
  ASSERT_EQ(manager.set_sink("sink-sunshine-stereo"), 0);
  auto now = sink_manager_t::clock::now();
  for (int x = 0; x < sink_manager_t::keep_default_limit; ++x) {
    server.default_name = "speakers";
    manager.on_server_changed({.keep_default = true, .auto_capture = true}, now + std::chrono::seconds(x));
    ASSERT_EQ(server.default_name, "sink-sunshine-stereo") << "restore " << x;
  }
  auto gen = generation();
  server.default_name = "speakers";
  manager.on_server_changed({.keep_default = true, .auto_capture = true}, now + 6s);
  EXPECT_EQ(server.default_name, "speakers");
  EXPECT_FALSE(manager.muted());
  EXPECT_EQ(generation(), gen + 1);
  EXPECT_EQ(manager.capture_sink(""), "speakers");
}

// Bugs caught: 0, False positives: 0
TEST_F(AudioSinkManagerTest, KeepDefaultRestoresAgainAfterTheWindow) {
  ASSERT_EQ(manager.set_sink("sink-sunshine-stereo"), 0);
  auto now = sink_manager_t::clock::now();
  for (int x = 0; x < sink_manager_t::keep_default_limit + 2; ++x) {
    server.default_name = "speakers";
    manager.on_server_changed({.keep_default = true, .auto_capture = true}, now + sink_manager_t::keep_default_window * x);
    ASSERT_EQ(server.default_name, "sink-sunshine-stereo") << "restore " << x;
  }
}

// Bugs caught: 0, False positives: 0
TEST_F(AudioSinkManagerTest, KeepDefaultOnlyAppliesToVirtualSinks) {
  ASSERT_EQ(manager.set_sink("headset"), 0);
  auto gen = generation();
  server.default_name = "speakers";
  manager.on_server_changed({.keep_default = true, .auto_capture = true});
  EXPECT_EQ(server.default_name, "speakers");
  EXPECT_EQ(generation(), gen + 1);
  EXPECT_EQ(manager.capture_sink("headset"), "speakers");
}

// Bugs caught: 0, False positives: 0
TEST_F(AudioSinkManagerTest, AutoCaptureFollowsANewDefaultAndEndsTheMute) {
  server.add_input(2, "headset");
  ASSERT_EQ(manager.set_sink("sink-sunshine-stereo"), 0);
  auto gen = generation();

  server.default_name = "headset";
  manager.on_server_changed({.keep_default = false, .auto_capture = true});
  EXPECT_EQ(server.default_name, "headset");
  EXPECT_EQ(generation(), gen + 1);
  EXPECT_EQ(manager.capture_sink(""), "headset");
  EXPECT_FALSE(manager.muted());
  EXPECT_EQ(server.sink_of(2), "headset");

  // Other server changes (default source, ...) do not reinitialise capture again.
  manager.on_server_changed({.keep_default = false, .auto_capture = true});
  EXPECT_EQ(generation(), gen + 1);
}

// Bugs caught: 0, False positives: 0
TEST_F(AudioSinkManagerTest, WithoutAutoCaptureCaptureStaysOnItsSink) {
  ASSERT_EQ(manager.set_sink("sink-sunshine-stereo"), 0);
  auto gen = generation();
  server.default_name = "headset";
  manager.on_server_changed({.keep_default = false, .auto_capture = false});
  EXPECT_EQ(generation(), gen);
  EXPECT_EQ(manager.capture_sink(""), "sink-sunshine-stereo");
}

// Bugs caught: 0, False positives: 0
TEST_F(AudioSinkManagerTest, AutoCaptureFollowsTheDefaultWhenApolloNeverSetOne) {
  auto gen = generation();
  EXPECT_EQ(manager.capture_sink(""), "speakers");
  server.default_name = "headset";
  manager.on_server_changed({.keep_default = true, .auto_capture = true});
  EXPECT_EQ(generation(), gen + 1);
  EXPECT_EQ(manager.capture_sink(""), "headset");
  EXPECT_EQ(server.default_name, "headset") << "nothing was assigned, so nothing is kept";
}

// Bugs caught: 0, False positives: 0
TEST_F(AudioSinkManagerTest, AutoCaptureFollowsTheDefaultAwayFromAConfiguredSinkThatWasAlreadyDefault) {
  // audio_sink names the current default, so the stream start never calls set_sink().
  EXPECT_EQ(manager.capture_sink("speakers"), "speakers");
  auto gen = generation();
  server.default_name = "headset";
  manager.on_server_changed({.keep_default = true, .auto_capture = true});
  EXPECT_EQ(generation(), gen + 1);
  EXPECT_EQ(manager.capture_sink("speakers"), "headset") << "the restarted capture must record the new default";
  EXPECT_EQ(server.default_name, "headset") << "a hardware sink is not put back";
}

// Bugs caught: 0, False positives: 0
TEST_F(AudioSinkManagerTest, CaptureSinkPriority) {
  EXPECT_EQ(manager.capture_sink(""), "speakers");
  EXPECT_EQ(manager.capture_sink("headset"), "headset");
  EXPECT_EQ(manager.capture_sink("Headset"), "headset") << "a description resolves to the sink name";
  ASSERT_EQ(manager.set_sink("sink-sunshine-stereo"), 0);
  EXPECT_EQ(manager.capture_sink("headset"), "sink-sunshine-stereo") << "with host audio off, capture the virtual sink";
}

// Bugs caught: 0, False positives: 0
TEST_F(AudioSinkManagerTest, FindsSinksByNameOrDescription) {
  EXPECT_TRUE(manager.is_sink_available("speakers"));
  EXPECT_TRUE(manager.is_sink_available("Speakers"));
  EXPECT_FALSE(manager.is_sink_available("hdmi"));
  EXPECT_FALSE(manager.is_sink_available(""));
  EXPECT_EQ(manager.find_sink("Headset"), "headset");

  server.add_sink("usb", "Headset");
  EXPECT_EQ(manager.find_sink("Headset"), std::nullopt) << "an ambiguous description names no sink";
  EXPECT_EQ(manager.find_sink("headset"), "headset");

  // set_sink accepts a description too.
  ASSERT_EQ(manager.set_sink("Speakers"), 0);
  EXPECT_EQ(server.default_name, "speakers");
  EXPECT_NE(manager.set_sink("missing"), 0);
}

#endif
