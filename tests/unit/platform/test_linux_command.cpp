/**
 * @file tests/unit/platform/test_linux_command.cpp
 * @brief Test how run_command() runs URLs and elevated commands on Linux (src/platform/linux/command.*).
 *
 * Nothing here opens a browser or a terminal or asks for root: the end-to-end tests put fake `xdg-open`, `pkexec`
 * and `sudo` scripts first in PATH (and nothing else), which record their arguments.
 */
#include "../../tests_common.h"

#ifdef __linux__

  // standard includes
  #include <cstdlib>
  #include <filesystem>
  #include <fstream>
  #include <map>
  #include <sstream>
  #include <string>
  #include <vector>

  // lib includes
  #include <boost/process/v1/child.hpp>
  #include <boost/process/v1/environment.hpp>

  // local includes
  #include <src/config.h>
  #include <src/platform/linux/command.h>

namespace {
  namespace fs = std::filesystem;
  using platf::command::elevation_e;
  using platf::command::options_t;
  using platf::command::plan;
  using strings = std::vector<std::string>;

  const std::string chdir_script = R"(cd -- "$1" || exit 1; shift; exec "$@")";

  /**
   * @brief A temporary directory, removed on destruction.
   */
  struct temp_dir_t {
    fs::path path;

    temp_dir_t() {
      auto tmpl = (fs::temp_directory_path() / "acmd-XXXXXX").string();
      if (::mkdtemp(tmpl.data())) {
        path = tmpl;
      }
    }

    ~temp_dir_t() {
      std::error_code ec;
      fs::remove_all(path, ec);
    }
  };

  void write_script(const fs::path &path, const std::string &body) {
    std::ofstream {path} << "#!/bin/sh\n" << body;
    fs::permissions(path, fs::perms::owner_all);
  }

  std::string read_file(const fs::path &path) {
    std::ifstream in {path};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
  }

  /**
   * @brief PATH holding only a directory of fake tools for the lifetime of the object, and the elevation method.
   */
  class fake_tools_t {
  public:
    explicit fake_tools_t(const std::string &elevation_method) {
      old_path_ = std::getenv("PATH") ? std::getenv("PATH") : "";
      old_method_ = config::sunshine.elevation_method;
      config::sunshine.elevation_method = elevation_method;
      bin = tmp.path / "bin";
      fs::create_directories(bin);
      log = tmp.path / "log";
      // Each fake records its name and arguments, one per line, then behaves like the real tool would here.
      const std::string record = "{ echo \"$0\"; for a in \"$@\"; do echo \"[$a]\"; done; } >> '" + log.string() + "'\n";
      write_script(bin / "xdg-open", record);
      write_script(bin / "pkexec", record + "[ -n \"$FAKE_DENY\" ] && exit 126\nexec \"$@\"\n");
      write_script(bin / "sudo", record + "[ \"$1\" = -n ] && shift\n[ \"$1\" = -- ] && shift\nexec \"$@\"\n");
      write_script(bin / "xdg-terminal-exec", record + "exec \"$@\"\n");
      ::setenv("PATH", bin.c_str(), 1);
    }

    ~fake_tools_t() {
      ::setenv("PATH", old_path_.c_str(), 1);
      config::sunshine.elevation_method = old_method_;
    }

    /**
     * @brief Run a command through platf::run_command() and wait for it.
     * @return Its exit code, or -1 if it did not start.
     */
    int run(bool elevated, const std::string &cmd, boost::filesystem::path working_dir, const std::map<std::string, std::string> &extra_env = {}) {
      // A copy: assigning to this_process::environment() itself would change this process's environment.
      boost::process::v1::environment env = boost::this_process::environment();
      for (const auto &[name, value] : extra_env) {
        env[name] = value;
      }
      std::error_code ec;
      auto child = platf::run_command(elevated, true, cmd, working_dir, env, nullptr, ec, nullptr);
      if (ec) {
        return -1;
      }
      child.wait();
      return child.exit_code();
    }

    temp_dir_t tmp;
    fs::path bin;
    fs::path log;

  private:
    std::string old_path_;
    std::string old_method_;
  };
}  // namespace

// Bugs caught: 0, False positives: 0
TEST(LinuxCommandTest, Urls) {
  EXPECT_TRUE(platf::command::is_url("steam://rungameid/570"));
  EXPECT_TRUE(platf::command::is_url("https://moonlight-stream.org"));
  EXPECT_TRUE(platf::command::is_url("com.example+x-1.0://a"));
  EXPECT_FALSE(platf::command::is_url("/usr/bin/steam"));
  EXPECT_FALSE(platf::command::is_url("://x"));
  EXPECT_FALSE(platf::command::is_url("steam://"));
  EXPECT_FALSE(platf::command::is_url("1steam://x"));
  EXPECT_FALSE(platf::command::is_url("sh -c 'xdg-open https://x'"));

  // Only the URL is opened, as on Windows; elevation does not apply to it.
  auto p = plan("steam://rungameid/570 -extra", {.elevated = true});
  ASSERT_TRUE(p);
  EXPECT_TRUE(p->url);
  EXPECT_EQ(p->argv, (strings {"xdg-open", "steam://rungameid/570"}));
}

// Bugs caught: 0, False positives: 1
TEST(LinuxCommandTest, PlainCommandsRunAsTheirWords) {
  auto p = plan("steam -gamepadui", {});
  ASSERT_TRUE(p);
  EXPECT_FALSE(p->url);
  EXPECT_EQ(p->argv, (strings {"steam", "-gamepadui"}));
  EXPECT_EQ(plan(R"(sh -c 'date > "$XDG_RUNTIME_DIR/x"')", {})->argv, (strings {"sh", "-c", R"(date > "$XDG_RUNTIME_DIR/x")"}));

  EXPECT_FALSE(plan("", {}));
  EXPECT_FALSE(plan("   ", {}));
  // As in sh, an unbalanced quote is an error, elevated or not.
  EXPECT_FALSE(plan("echo 'two words", {}));
  EXPECT_FALSE(plan("echo 'two words", {.elevated = true}));
}

// Bugs caught: 0, False positives: 0
TEST(LinuxCommandTest, SplitFollowsShellQuoting) {
  using platf::command::split;
  EXPECT_EQ(split(" a\tb\n c  "), (strings {"a", "b", "c"}));
  // Single quotes keep everything, including double quotes, backslashes and $.
  EXPECT_EQ(split(R"(sh -c 'date > "$HOME/x" \n')"), (strings {"sh", "-c", R"(date > "$HOME/x" \n)"}));
  EXPECT_EQ(split(R"(echo "it's" 'say "hi"')"), (strings {"echo", "it's", R"(say "hi")"}));
  // In double quotes a backslash only escapes $, `, ", \ and a newline.
  EXPECT_EQ(split(R"("a \$b \` \" \\ \n")"), (strings {R"(a $b ` " \ \n)"}));
  EXPECT_EQ(split("\"a\\\nb\""), (strings {"ab"}));
  // Outside quotes a backslash keeps the next character, and joins lines before a newline.
  EXPECT_EQ(split(R"(a\ b \'c \"d \\e)"), (strings {"a b", "'c", "\"d", "\\e"}));
  EXPECT_EQ(split("a\\\nb c"), (strings {"ab", "c"}));
  EXPECT_EQ(split("a \\\n b"), (strings {"a", "b"}));
  EXPECT_EQ(split("a\\"), (strings {"a\\"}));
  // Adjacent parts form one word; empty quotes give an empty word.
  EXPECT_EQ(split(R"(--name='A B'"C"D '' "")"), (strings {"--name=A BCD", "", ""}));
  // Nothing is expanded.
  EXPECT_EQ(split("echo $HOME $(id) `id` ~ * a|b >c"), (strings {"echo", "$HOME", "$(id)", "`id`", "~", "*", "a|b", ">c"}));

  EXPECT_FALSE(split(""));
  EXPECT_FALSE(split(" \t\n"));
  EXPECT_FALSE(split("\\\n"));
  EXPECT_FALSE(split("echo 'a"));
  EXPECT_FALSE(split(R"(echo "a)"));
  EXPECT_FALSE(split(R"(echo "a\")"));
}

// Bugs caught: 0, False positives: 0
TEST(LinuxCommandTest, ElevatedCommandLines) {
  options_t options {.elevated = true, .working_dir = "/srv/games dir", .helper = "/usr/lib/apollo/apollo-run-elevated"};

  options.elevation = elevation_e::pkexec;
  EXPECT_EQ(plan("nvidia-smi -pl '300'", options)->argv, (strings {"pkexec", "/usr/lib/apollo/apollo-run-elevated", "/srv/games dir", "nvidia-smi", "-pl", "300"}));

  options.elevation = elevation_e::sudo;
  EXPECT_EQ(plan("nvidia-smi -pl 300", options)->argv, (strings {"sudo", "-n", "--", "/usr/lib/apollo/apollo-run-elevated", "/srv/games dir", "nvidia-smi", "-pl", "300"}));

  options.elevation = elevation_e::terminal;
  options.terminal = {"alacritty", "-e"};
  EXPECT_EQ(plan("nvidia-smi", options)->argv, (strings {"alacritty", "-e", "sudo", "--", "/usr/lib/apollo/apollo-run-elevated", "/srv/games dir", "nvidia-smi"}));
  options.terminal.clear();
  EXPECT_FALSE(plan("nvidia-smi", options));

  // Without the installed helper (build tree): an equivalent sh -c; no working directory means /.
  EXPECT_EQ(plan("nvidia-smi", {.elevated = true})->argv, (strings {"pkexec", "/bin/sh", "-c", chdir_script, "apollo-run-elevated", "/", "nvidia-smi"}));
}

// Bugs caught: 0, False positives: 0
TEST(LinuxCommandTest, ParseElevation) {
  EXPECT_EQ(platf::command::parse_elevation("pkexec"), elevation_e::pkexec);
  EXPECT_EQ(platf::command::parse_elevation("sudo"), elevation_e::sudo);
  EXPECT_EQ(platf::command::parse_elevation("terminal"), elevation_e::terminal);
  EXPECT_EQ(platf::command::parse_elevation(""), elevation_e::pkexec);
  EXPECT_EQ(platf::command::parse_elevation("doas"), elevation_e::pkexec);
}

// Bugs caught: 0, False positives: 0
TEST(LinuxCommandTest, TerminalPrefix) {
  std::map<std::string, std::string> env;
  std::vector<std::string> path;
  auto getenv = [&](const char *name) -> const char * {
    auto it = env.find(name);
    return it == env.end() ? nullptr : it->second.c_str();
  };
  auto in_path = [&](const std::string &name) {
    return std::ranges::find(path, name) != path.end();
  };
  auto prefix = [&]() {
    return platf::command::terminal_prefix(getenv, in_path);
  };

  EXPECT_TRUE(prefix().empty());
  path = {"xterm", "kitty"};
  EXPECT_EQ(prefix(), (strings {"kitty"}));
  env["TERMINAL"] = "/usr/bin/wezterm";
  EXPECT_EQ(prefix(), (strings {"/usr/bin/wezterm", "start", "--"}));
  env["TERMINAL"] = "footclient --title 'Apollo x'";
  EXPECT_EQ(prefix(), (strings {"footclient", "--title", "Apollo x", "-e"}));
  path.push_back("xdg-terminal-exec");
  EXPECT_EQ(prefix(), (strings {"xdg-terminal-exec"}));
}

// Bugs caught: 0, False positives: 0
TEST(LinuxCommandTest, ToStringQuotes) {
  EXPECT_EQ(platf::command::to_string({"pkexec", "/bin/sh", "-c", "cd -- \"$1\"", "it's"}), R"(pkexec /bin/sh -c 'cd -- "$1"' 'it'\''s')");
}

// Bugs caught: 0, False positives: 0
TEST(LinuxCommandTest, RunCommandOpensUrlsWithXdgOpen) {
  fake_tools_t tools {"pkexec"};
  EXPECT_EQ(tools.run(false, "steam://open/bigpicture", "/"), 0);
  EXPECT_EQ(read_file(tools.log), (tools.bin / "xdg-open").string() + "\n[steam://open/bigpicture]\n");
}

// Bugs caught: 0, False positives: 1
TEST(LinuxCommandTest, RunCommandElevatesInTheWorkingDirectory) {
  fake_tools_t tools {"pkexec"};
  auto out = tools.tmp.path / "out";
  auto work = tools.tmp.path / "work dir";
  fs::create_directories(work);

  // The fake pkexec runs the rest unprivileged: the sh -c fallback changes to the working directory first.
  // (or the installed helper, if this machine has one.)
  EXPECT_EQ(tools.run(true, "/bin/sh -c '/bin/pwd > $0' " + out.string(), work.string()), 0);
  EXPECT_EQ(read_file(out), work.string() + "\n");
  EXPECT_TRUE(read_file(tools.log).starts_with((tools.bin / "pkexec").string() + "\n"));

  // Not elevated: no pkexec.
  fs::remove(tools.log);
  EXPECT_EQ(tools.run(false, "/bin/sh -c \"exit 3\"", work.string()), 3);
  EXPECT_FALSE(fs::exists(tools.log));
}

// Bugs caught: 0, False positives: 1
TEST(LinuxCommandTest, RunCommandSplitsPlainCommandsLikeTheShell) {
  fake_tools_t tools {"pkexec"};
  auto out = tools.tmp.path / "out";

  // Boost.Process only knew double quotes, so this ran sh -c "'exit" and failed.
  EXPECT_EQ(tools.run(false, "/bin/sh -c 'exit 4'", "/"), 4);
  EXPECT_EQ(tools.run(false, R"(/bin/sh -c 'printf "%s|" "$0" "$1" > "$OUT"' 'a b' "c'd")", "/", {{"OUT", out.string()}}), 0);
  EXPECT_EQ(read_file(out), "a b|c'd|");

  // A program without a directory is found in the child's PATH, not Apollo's (which holds only the fake tools).
  EXPECT_EQ(tools.run(false, "sh -c 'exit 5'", "/", {{"PATH", "/usr/bin:/bin"}}), 5);
  EXPECT_EQ(tools.run(false, "sh -c 'exit 5'", "/"), -1);
  EXPECT_EQ(tools.run(false, "echo 'unbalanced", "/"), -1);
  EXPECT_FALSE(fs::exists(tools.log));
}

// Bugs caught: 0, False positives: 0
TEST(LinuxCommandTest, RunCommandReportsDenial) {
  fake_tools_t tools {"pkexec"};
  EXPECT_EQ(tools.run(true, "/bin/sh -c 'exit 0'", "/", {{"FAKE_DENY", "1"}}), platf::command::exit_auth_denied);
}

// Bugs caught: 0, False positives: 0
TEST(LinuxCommandTest, RunCommandWithSudoAndTerminal) {
  {
    fake_tools_t tools {"sudo"};
    EXPECT_EQ(tools.run(true, "/bin/sh -c 'exit 0'", "/"), 0);
    EXPECT_TRUE(read_file(tools.log).starts_with((tools.bin / "sudo").string() + "\n[-n]\n[--]\n"));
  }
  {
    fake_tools_t tools {"terminal"};
    EXPECT_EQ(tools.run(true, "/bin/sh -c 'exit 0'", "/"), 0);
    auto log = read_file(tools.log);
    EXPECT_TRUE(log.starts_with((tools.bin / "xdg-terminal-exec").string() + "\n[sudo]\n[--]\n")) << log;
    EXPECT_NE(log.find((tools.bin / "sudo").string() + "\n[--]\n"), std::string::npos) << log;
  }
  {
    // pkexec missing: the command fails to start instead of running unelevated.
    fake_tools_t tools {"pkexec"};
    fs::remove(tools.bin / "pkexec");
    EXPECT_EQ(tools.run(true, "/bin/sh -c 'exit 0'", "/"), -1);
  }
}

// Bugs caught: 0, False positives: 0
TEST(LinuxCommandTest, HelperScriptChangesDirectory) {
  temp_dir_t tmp;
  auto helper = fs::path {SUNSHINE_SOURCE_DIR} / "src_assets/linux/misc/apollo-run-elevated";
  ASSERT_TRUE(fs::exists(helper));
  auto out = tmp.path / "out";
  auto cmd = helper.string() + " '" + tmp.path.string() + "' /bin/sh -c '/bin/pwd > out'";
  EXPECT_EQ(std::system(cmd.c_str()), 0);
  EXPECT_EQ(read_file(out), tmp.path.string() + "\n");
  // Missing arguments: usage error, not 126.
  EXPECT_EQ(WEXITSTATUS(std::system((helper.string() + " / 2>/dev/null").c_str())), 2);
}

// Bugs caught: 0, False positives: 0
TEST(LinuxCommandTest, ElevatedRunnersRefuseAMissingDirectory) {
  temp_dir_t tmp;
  auto helper = fs::path {SUNSHINE_SOURCE_DIR} / "src_assets/linux/misc/apollo-run-elevated";
  ASSERT_TRUE(fs::exists(helper));
  auto missing = tmp.path / "gone";
  auto marker = tmp.path / "ran";
  // A relative path in the command must not resolve against / (or anywhere else) instead.
  auto command = " '" + missing.string() + "' /bin/sh -c 'touch " + marker.string() + "' 2>/dev/null";

  EXPECT_NE(WEXITSTATUS(std::system((helper.string() + command).c_str())), 0);
  EXPECT_FALSE(fs::exists(marker)) << "the helper ran the command";

  // The sh -c fallback used without an installed helper behaves the same.
  auto fallback = "/bin/sh -c '" + chdir_script + "' apollo-run-elevated" + command;
  EXPECT_NE(WEXITSTATUS(std::system(fallback.c_str())), 0);
  EXPECT_FALSE(fs::exists(marker)) << "the fallback ran the command";
}

#endif
