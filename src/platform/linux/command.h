/**
 * @file src/platform/linux/command.h
 * @brief How platf::run_command() turns a configured command into what it executes on Linux.
 *
 * Kept free of process creation so it can be unit-tested without running anything:
 * - URLs (`scheme://...`) open with `xdg-open`, as Windows hands them to rundll32's URL handler.
 * - Elevated commands run as root through `pkexec`, `sudo -n`, or `sudo` in a terminal window. A small helper
 *   (`apollo-run-elevated DIR COMMAND...`, installed with Apollo's polkit action) changes to the working
 *   directory first, because pkexec starts programs in root's home; without it an equivalent `sh -c` is used.
 * - Everything else runs unchanged.
 */
#pragma once

// standard includes
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace platf::command {
  enum class elevation_e {
    pkexec,  ///< `pkexec`: authorization through the polkit agent.
    sudo,  ///< `sudo -n`: never prompts; needs a sudoers rule without a password.
    terminal,  ///< `sudo` in a terminal window, where the password can be typed from the stream.
  };

  /**
   * @brief Parse the `elevation_method` config value. Unknown values give pkexec.
   */
  elevation_e parse_elevation(std::string_view value);

  /**
   * @brief Exit status of pkexec when the user dismissed or failed the authorization.
   */
  constexpr int exit_auth_denied = 126;

  struct options_t {
    bool elevated = false;
    elevation_e elevation = elevation_e::pkexec;
    std::string working_dir;  ///< For elevated commands; "/" if empty.
    std::string helper;  ///< Path of apollo-run-elevated, or "" for the `sh -c` fallback.
    std::vector<std::string> terminal;  ///< Terminal prefix for elevation_e::terminal (see terminal_prefix()).
  };

  struct plan_t {
    std::vector<std::string> argv;  ///< What to execute; empty means: run the command string unchanged.
    bool url = false;  ///< The command was a URL handed to xdg-open.
  };

  /**
   * @brief Whether a command target is a URL: `scheme://...` with an RFC 3986 scheme.
   */
  bool is_url(std::string_view target);

  /**
   * @brief Decide how to execute a command.
   * @return nullopt if the command is empty or cannot be split into words, or if a terminal is needed and
   *         `options.terminal` is empty.
   */
  std::optional<plan_t> plan(const std::string &cmd, const options_t &options);

  /**
   * @brief The command line that opens a terminal running the arguments appended to it.
   *
   * In order: `xdg-terminal-exec`; `$TERMINAL` (with the flag known for it, `-e` otherwise); the first known
   * terminal found in PATH.
   * @param getenv Reads an environment variable (nullptr if unset).
   * @param in_path Whether an executable is in PATH.
   * @return Empty if no terminal was found.
   */
  std::vector<std::string> terminal_prefix(
    const std::function<const char *(const char *)> &getenv,
    const std::function<bool(const std::string &)> &in_path
  );

  /**
   * @brief Join argv for logs, quoting words that need it.
   */
  std::string to_string(const std::vector<std::string> &argv);
}  // namespace platf::command
