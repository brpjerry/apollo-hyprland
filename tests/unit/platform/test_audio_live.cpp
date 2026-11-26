/**
 * @file tests/unit/platform/test_audio_live.cpp
 * @brief Linux audio control (src/platform/linux/audio.cpp) against a private PipeWire + pipewire-pulse + WirePlumber.
 * @details The suite starts its own instance with scripts/hyprland/run-private-pipewire.sh and skips when it cannot
 *          (PipeWire, WirePlumber or dbus-run-session missing). It never talks to the desktop's sound server.
 *          Needs pactl and pacat.
 */
#include "../../tests_common.h"

#ifdef __linux__

  #include "private_pulse.h"

  #include <csignal>
  #include <fcntl.h>
  #include <nlohmann/json.hpp>
  #include <spawn.h>
  #include <src/config.h>
  #include <sys/wait.h>
  #include <thread>

extern char **environ;

using namespace std::literals;

namespace {
  struct PulseAudioLiveTest: PlatformTestSuite {
    static void SetUpTestSuite() {
      PlatformTestSuite::SetUpTestSuite();
      available = pulse.start();
    }

    static void TearDownTestSuite() {
      pulse.stop();
      PlatformTestSuite::TearDownTestSuite();
    }

    void SetUp() override {
      if (!available) {
        GTEST_SKIP() << "No private PipeWire instance (see scripts/hyprland/run-private-pipewire.sh)";
      }
      saved_audio = config::audio;
      config::audio.sink.clear();
      config::audio.virtual_sink.clear();
      config::audio.keep_default = true;
      config::audio.auto_capture = true;

      ASSERT_TRUE(load_sink("apollo-test-speakers", "Apollo_Test_Speakers"));
      ASSERT_TRUE(load_sink("apollo-test-headset", "Apollo_Test_Headset"));
      ASSERT_TRUE(pactl("set-default-sink apollo-test-speakers"));
    }

    void TearDown() override {
      for (auto pid : players) {
        kill(pid, SIGTERM);
        waitpid(pid, nullptr, 0);
      }
      players.clear();
      for (auto module : modules) {
        pactl("unload-module " + module);
      }
      modules.clear();
      if (available) {
        config::audio = saved_audio;
      }
    }

    static std::optional<std::string> pactl(const std::string &args) {
      return pulse.run("pactl " + args + " 2>/dev/null");
    }

    bool load_sink(const std::string &name, const std::string &description) {
      auto out = pactl("load-module module-null-sink sink_name=" + name + " sink_properties=device.description=" + description);
      if (!out) {
        return false;
      }
      modules.push_back(out->substr(0, out->find('\n')));
      return true;
    }

    /** Stops a player started with play(). */
    void stop(pid_t pid) {
      kill(pid, SIGTERM);
      waitpid(pid, nullptr, 0);
      std::erase(players, pid);
    }

    /** Starts pacat playing silence as application @p app. */
    void play(const std::string &app) {
      posix_spawn_file_actions_t actions;
      posix_spawn_file_actions_init(&actions);
      posix_spawn_file_actions_addopen(&actions, 0, "/dev/zero", O_RDONLY, 0);
      posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
      auto client = "--client-name=" + app;
      char *argv[] = {(char *) "pacat", client.data(), nullptr};
      pid_t pid;
      ASSERT_EQ(posix_spawnp(&pid, "pacat", &actions, nullptr, argv, environ), 0);
      posix_spawn_file_actions_destroy(&actions);
      players.push_back(pid);
    }

    static std::string default_sink() {
      auto out = pactl("get-default-sink");
      return out ? out->substr(0, out->find('\n')) : std::string {};
    }

    static std::map<std::uint32_t, std::string> sink_names() {
      std::map<std::uint32_t, std::string> result;
      if (auto out = pactl("-f json list sinks")) {
        for (const auto &sink : nlohmann::json::parse(*out)) {
          result[sink["index"].get<std::uint32_t>()] = sink["name"].get<std::string>();
        }
      }
      return result;
    }

    /** Sink name of the (first) stream of application @p app, empty if there is none or it is not linked. */
    static std::string sink_of(const std::string &app) {
      auto out = pactl("-f json list sink-inputs");
      if (!out) {
        return {};
      }
      auto names = sink_names();
      for (const auto &input : nlohmann::json::parse(*out)) {
        auto props = input["properties"];
        if (props.value("application.name", "") == app) {
          auto it = names.find(input["sink"].get<std::uint32_t>());
          return it == names.end() ? std::string {} : it->second;
        }
      }
      return {};
    }

    static std::uint32_t input_index(const std::string &app) {
      auto out = pactl("-f json list sink-inputs");
      for (const auto &input : nlohmann::json::parse(out.value_or("[]"))) {
        if (input["properties"].value("application.name", "") == app) {
          return input["index"].get<std::uint32_t>();
        }
      }
      return UINT32_MAX;
    }

    /** Polls @p f for up to @p timeout. */
    template<class F>
    static bool eventually(F &&f, std::chrono::milliseconds timeout = 5s) {
      auto deadline = std::chrono::steady_clock::now() + timeout;
      while (!f()) {
        if (std::chrono::steady_clock::now() > deadline) {
          return false;
        }
        std::this_thread::sleep_for(50ms);
      }
      return true;
    }

    inline static test_pulse::private_pulse_t pulse;
    inline static bool available = false;

    config::audio_t saved_audio;
    std::vector<std::string> modules;
    std::vector<pid_t> players;
  };
}  // namespace

// Bugs caught: 2, False positives: 0
TEST_F(PulseAudioLiveTest, MuteMovesPinnedStreamsAndKeepsTheDefault) {
  // A follows the default; B is pinned to the headset the way a user pins Firefox (a move, stored by WirePlumber).
  // D is pinned too but not playing yet.
  play("apollo-test-a");
  play("apollo-test-b");
  play("apollo-test-d");
  ASSERT_TRUE(eventually([] { return sink_of("apollo-test-a") == "apollo-test-speakers" && sink_of("apollo-test-b") == "apollo-test-speakers" && sink_of("apollo-test-d") == "apollo-test-speakers"; }));
  ASSERT_TRUE(pactl("move-sink-input " + std::to_string(input_index("apollo-test-b")) + " apollo-test-headset"));
  ASSERT_TRUE(pactl("move-sink-input " + std::to_string(input_index("apollo-test-d")) + " apollo-test-headset"));
  ASSERT_TRUE(eventually([] { return sink_of("apollo-test-b") == "apollo-test-headset" && sink_of("apollo-test-d") == "apollo-test-headset"; }));
  stop(players.back());
  ASSERT_TRUE(eventually([] { return input_index("apollo-test-d") == UINT32_MAX; }));

  auto control = platf::audio_control();
  ASSERT_TRUE(control);
  auto sink = control->sink_info();
  ASSERT_TRUE(sink);
  EXPECT_EQ(sink->host, "apollo-test-speakers");
  ASSERT_TRUE(sink->null);
  EXPECT_TRUE(control->is_sink_available("apollo-test-headset"));
  EXPECT_TRUE(control->is_sink_available("Apollo_Test_Headset"));
  EXPECT_FALSE(control->is_sink_available("apollo-test-missing"));

  // Host audio off.
  const auto null_sink = sink->null->stereo;
  ASSERT_EQ(control->set_sink(null_sink), 0);
  EXPECT_EQ(default_sink(), null_sink) << "set_sink waits until the default is applied";
  EXPECT_TRUE(eventually([&] { return sink_of("apollo-test-a") == null_sink; }));
  EXPECT_TRUE(eventually([&] { return sink_of("apollo-test-b") == null_sink; }));

  // A pinned application that starts playing during the stream is linked to the headset first, then moved.
  play("apollo-test-d");
  EXPECT_TRUE(eventually([&] { return sink_of("apollo-test-d") == null_sink; }));

  // keep_sink_default: something else takes the default; it comes back.
  ASSERT_TRUE(pactl("set-default-sink apollo-test-speakers"));
  EXPECT_TRUE(eventually([&] { return default_sink() == null_sink; }));

  // Capture reads the virtual sink's monitor.
  auto mic = control->microphone(platf::speaker::map_stereo.data(), 2, 48000, 240, false, true);
  ASSERT_TRUE(mic);
  std::vector<float> samples(480);
  EXPECT_EQ(mic->sample(samples), platf::capture_e::ok);
  mic.reset();

  // Host audio back on (what stop_audio_control does): streams return to where they played.
  ASSERT_EQ(control->set_sink("apollo-test-speakers"), 0);
  EXPECT_EQ(default_sink(), "apollo-test-speakers");
  EXPECT_TRUE(eventually([] { return sink_of("apollo-test-b") == "apollo-test-headset"; }));
  EXPECT_TRUE(eventually([] { return sink_of("apollo-test-d") == "apollo-test-headset"; }));
  EXPECT_TRUE(eventually([] { return sink_of("apollo-test-a") == "apollo-test-speakers"; }));

  control.reset();
  // A still follows the default: the moves did not pin it.
  ASSERT_TRUE(pactl("set-default-sink apollo-test-headset"));
  EXPECT_TRUE(eventually([] { return sink_of("apollo-test-a") == "apollo-test-headset"; }));
  // Apollo's null sinks are gone.
  EXPECT_EQ(sink_names().size(), 2u);
}

// Bugs caught: 0, False positives: 0
TEST_F(PulseAudioLiveTest, AutoCaptureReinitialisesWhenTheDefaultChanges) {
  auto control = platf::audio_control();
  ASSERT_TRUE(control);
  ASSERT_TRUE(control->sink_info());

  auto mic = control->microphone(platf::speaker::map_stereo.data(), 2, 48000, 240, false, true);
  ASSERT_TRUE(mic);
  std::vector<float> samples(480);
  ASSERT_EQ(mic->sample(samples), platf::capture_e::ok);

  ASSERT_TRUE(pactl("set-default-sink apollo-test-headset"));
  EXPECT_TRUE(eventually([&] { return mic->sample(samples) == platf::capture_e::reinit; }));
  mic.reset();

  // Nothing was assigned with host audio on, so the new default is kept and captured.
  EXPECT_EQ(default_sink(), "apollo-test-headset");
  mic = control->microphone(platf::speaker::map_stereo.data(), 2, 48000, 240, false, true);
  ASSERT_TRUE(mic);
  EXPECT_EQ(mic->sample(samples), platf::capture_e::ok);
}

// Bugs caught: 0, False positives: 0
TEST_F(PulseAudioLiveTest, ConfiguredVirtualSinkReplacesTheNullSinks) {
  ASSERT_TRUE(load_sink("apollo-test-virtual", "Apollo_Test_Virtual"));
  play("apollo-test-c");
  ASSERT_TRUE(eventually([] { return sink_of("apollo-test-c") == "apollo-test-speakers"; }));
  ASSERT_TRUE(pactl("move-sink-input " + std::to_string(input_index("apollo-test-c")) + " apollo-test-headset"));
  ASSERT_TRUE(eventually([] { return sink_of("apollo-test-c") == "apollo-test-headset"; }));

  config::audio.virtual_sink = "Apollo_Test_Virtual";
  auto control = platf::audio_control();
  ASSERT_TRUE(control);
  auto sink = control->sink_info();
  ASSERT_TRUE(sink && sink->null);
  EXPECT_EQ(sink->null->stereo, "apollo-test-virtual");
  EXPECT_EQ(sink->null->surround71, "apollo-test-virtual");
  EXPECT_EQ(sink_names().size(), 3u) << "no null sinks are loaded";

  ASSERT_EQ(control->set_sink(sink->null->stereo), 0);
  EXPECT_TRUE(eventually([] { return sink_of("apollo-test-c") == "apollo-test-virtual"; }));
  control.reset();
  // Destroying the control without set_sink(host) still puts the stream back.
  EXPECT_TRUE(eventually([] { return sink_of("apollo-test-c") == "apollo-test-headset"; }));
}

#endif
