/**
 * @file src/platform/linux/command.cpp
 * @brief Command construction for platf::run_command() on Linux.
 */
// local includes
#include "command.h"

// standard includes
#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <utility>

namespace platf::command {
  namespace {
    /// The `sh -c` script used when the helper is not installed; same arguments as apollo-run-elevated.
    constexpr auto chdir_script = R"(cd -- "$1" || exit 1; shift; exec "$@")";

    struct terminal_t {
      std::string_view name;
      std::vector<std::string> exec_args;  ///< What goes between the terminal and the command.
    };

    const std::vector<terminal_t> &known_terminals() {
      static const std::vector<terminal_t> terminals {
        {"foot", {}},
        {"kitty", {}},
        {"alacritty", {"-e"}},
        {"wezterm", {"start", "--"}},
        {"ghostty", {"-e"}},
        {"konsole", {"-e"}},
        {"gnome-terminal", {"--"}},
        {"xterm", {"-e"}},
      };
      return terminals;
    }

  }  // namespace

  std::optional<std::vector<std::string>> split(std::string_view cmd) {
    std::vector<std::string> words;
    std::string word;
    bool in_word = false;  // Set by any character or quote, so '' and "" give an empty word.

    for (std::size_t i = 0; i < cmd.size(); ++i) {
      char c = cmd[i];
      if (c == '\\' && i + 1 < cmd.size() && cmd[i + 1] == '\n') {
        // A backslash and newline join lines.
        ++i;
        continue;
      }
      if (c == ' ' || c == '\t' || c == '\n') {
        if (in_word) {
          words.push_back(std::move(word));
          word.clear();
          in_word = false;
        }
        continue;
      }

      in_word = true;
      if (c == '\\') {
        if (++i == cmd.size()) {
          // A trailing backslash stays, as with `sh -c 'echo a\'`.
          word += c;
        } else {
          word += cmd[i];
        }
      } else if (c == '\'') {
        auto end = cmd.find('\'', i + 1);
        if (end == std::string_view::npos) {
          return std::nullopt;
        }
        word.append(cmd.substr(i + 1, end - i - 1));
        i = end;
      } else if (c == '"') {
        for (++i;; ++i) {
          if (i == cmd.size()) {
            return std::nullopt;
          }
          if (cmd[i] == '"') {
            break;
          }
          if (cmd[i] == '\\' && i + 1 < cmd.size() && std::string_view {"$`\"\\\n"}.find(cmd[i + 1]) != std::string_view::npos) {
            if (cmd[++i] != '\n') {
              word += cmd[i];
            }
            continue;
          }
          word += cmd[i];
        }
      } else {
        word += c;
      }
    }
    if (in_word) {
      words.push_back(std::move(word));
    }
    if (words.empty()) {
      return std::nullopt;
    }
    return words;
  }

  elevation_e parse_elevation(std::string_view value) {
    if (value == "sudo") {
      return elevation_e::sudo;
    }
    if (value == "terminal") {
      return elevation_e::terminal;
    }
    return elevation_e::pkexec;
  }

  bool is_url(std::string_view target) {
    auto sep = target.find("://");
    if (sep == std::string_view::npos || sep == 0 || sep + 3 >= target.size()) {
      return false;
    }
    auto scheme = target.substr(0, sep);
    if (!std::isalpha(static_cast<unsigned char>(scheme.front()))) {
      return false;
    }
    return std::ranges::all_of(scheme, [](char c) {
      return std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '-' || c == '.';
    });
  }

  std::optional<plan_t> plan(const std::string &cmd, const options_t &options) {
    auto words = split(cmd);
    if (!words) {
      return std::nullopt;
    }

    if (is_url(words->front())) {
      // Like Windows, the URL alone is opened; anything after it is ignored. Never elevated: the browser or the
      // URL handler runs as the user.
      return plan_t {{"xdg-open", words->front()}, true};
    }

    if (!options.elevated) {
      return plan_t {std::move(*words), false};
    }

    std::vector<std::string> argv;
    switch (options.elevation) {
      case elevation_e::pkexec:
        argv = {"pkexec"};
        break;
      case elevation_e::sudo:
        argv = {"sudo", "-n", "--"};
        break;
      case elevation_e::terminal:
        if (options.terminal.empty()) {
          return std::nullopt;
        }
        argv = options.terminal;
        argv.insert(argv.end(), {"sudo", "--"});
        break;
    }

    if (options.helper.empty()) {
      argv.insert(argv.end(), {"/bin/sh", "-c", chdir_script, "apollo-run-elevated"});
    } else {
      argv.push_back(options.helper);
    }
    argv.push_back(options.working_dir.empty() ? "/" : options.working_dir);
    argv.insert(argv.end(), words->begin(), words->end());
    return plan_t {std::move(argv), false};
  }

  std::vector<std::string> terminal_prefix(
    const std::function<const char *(const char *)> &getenv,
    const std::function<bool(const std::string &)> &in_path
  ) {
    if (in_path("xdg-terminal-exec")) {
      return {"xdg-terminal-exec"};
    }

    if (const char *terminal = getenv("TERMINAL"); terminal && *terminal) {
      if (auto words = split(terminal)) {
        auto name = std::filesystem::path {words->front()}.filename().string();
        auto known = std::ranges::find(known_terminals(), name, &terminal_t::name);
        if (known != known_terminals().end()) {
          words->insert(words->end(), known->exec_args.begin(), known->exec_args.end());
        } else {
          words->push_back("-e");
        }
        return *words;
      }
    }

    for (const auto &terminal : known_terminals()) {
      std::string name {terminal.name};
      if (in_path(name)) {
        std::vector<std::string> result {name};
        result.insert(result.end(), terminal.exec_args.begin(), terminal.exec_args.end());
        return result;
      }
    }
    return {};
  }

  std::string to_string(const std::vector<std::string> &argv) {
    std::string result;
    for (const auto &word : argv) {
      if (!result.empty()) {
        result += ' ';
      }
      bool plain = !word.empty() && std::ranges::all_of(word, [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || std::string_view {"-_./:=@%+,"}.find(c) != std::string_view::npos;
      });
      if (plain) {
        result += word;
        continue;
      }
      result += '\'';
      for (char c : word) {
        if (c == '\'') {
          result += R"('\'')";
        } else {
          result += c;
        }
      }
      result += '\'';
    }
    return result;
  }
}  // namespace platf::command
