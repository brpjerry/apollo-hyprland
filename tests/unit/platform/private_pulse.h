/**
 * @file tests/unit/platform/private_pulse.h
 * @brief Points libpulse at a private PipeWire instance (scripts/hyprland/run-private-pipewire.sh) for audio tests.
 * @details Audio tests load sinks, change the default sink and move streams. They must never do that to the desktop's
 *          sound server, so start() always leaves PULSE_SERVER pointing somewhere private: at the private instance
 *          when it came up, else at a socket that does not exist (audio then fails to initialise, which the tests
 *          handle). stop() stops the instance and restores the environment.
 */
#pragma once

#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>

namespace test_pulse {
  class private_pulse_t {
  public:
    /** @return true if the private instance runs. */
    bool start() {
      save("PULSE_SERVER");
      save("PULSE_RUNTIME_PATH");
      save("PIPEWIRE_RUNTIME_DIR");
      save("PIPEWIRE_REMOTE");
      // Nothing below may reach the desktop's server, whatever happens next.
      setenv("PULSE_SERVER", "unix:/nonexistent/apollo-test-no-pulse", 1);

#if defined(__linux__) && defined(SUNSHINE_SOURCE_DIR)
      std::string cmd = "'" SUNSHINE_SOURCE_DIR "/scripts/hyprland/run-private-pipewire.sh' start";
      FILE *pipe = popen(cmd.c_str(), "r");
      if (!pipe) {
        return false;
      }
      std::map<std::string, std::string> vars;
      char line[1024];
      while (fgets(line, sizeof(line), pipe)) {
        // export NAME='value'
        std::string s {line};
        if (s.rfind("export ", 0) != 0) {
          continue;
        }
        auto eq = s.find('=');
        auto open = s.find('\'', eq);
        auto close = s.rfind('\'');
        if (eq == std::string::npos || open == std::string::npos || close <= open) {
          continue;
        }
        vars[s.substr(7, eq - 7)] = s.substr(open + 1, close - open - 1);
      }
      if (pclose(pipe) != 0 || !vars.contains("APOLLO_PRIVATE_PIPEWIRE_DIR") || !vars.contains("PULSE_SERVER")) {
        return false;
      }
      _dir = vars["APOLLO_PRIVATE_PIPEWIRE_DIR"];
      for (const auto &[name, value] : vars) {
        if (name != "APOLLO_PRIVATE_PIPEWIRE_DIR") {
          setenv(name.c_str(), value.c_str(), 1);
        }
      }
      return true;
#else
      return false;
#endif
    }

    /** Runs a shell command against the private instance; returns its stdout, or nullopt if it failed. */
    std::optional<std::string> run(const std::string &cmd) const {
      FILE *pipe = popen(cmd.c_str(), "r");
      if (!pipe) {
        return std::nullopt;
      }
      std::string out;
      char buf[512];
      while (auto n = fread(buf, 1, sizeof(buf), pipe)) {
        out.append(buf, n);
      }
      if (pclose(pipe) != 0) {
        return std::nullopt;
      }
      return out;
    }

    void stop() {
#if defined(__linux__) && defined(SUNSHINE_SOURCE_DIR)
      if (!_dir.empty()) {
        std::string cmd = "'" SUNSHINE_SOURCE_DIR "/scripts/hyprland/run-private-pipewire.sh' stop '" + _dir + "'";
        std::system(cmd.c_str());
        _dir.clear();
      }
#endif
      for (const auto &[name, value] : _saved) {
        if (value) {
          setenv(name.c_str(), value->c_str(), 1);
        } else {
          unsetenv(name.c_str());
        }
      }
      _saved.clear();
    }

    bool running() const {
      return !_dir.empty();
    }

  private:
    void save(const char *name) {
      auto value = std::getenv(name);
      _saved[name] = value ? std::optional<std::string> {value} : std::nullopt;
    }

    std::string _dir;
    std::map<std::string, std::optional<std::string>> _saved;
  };
}  // namespace test_pulse
