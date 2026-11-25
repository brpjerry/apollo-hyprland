/**
 * @file src/platform/linux/hyprland/ipc.cpp
 * @brief Definitions for a client of Hyprland's IPC sockets.
 *
 * Protocol notes, checked against Hyprland 0.57.2 (live session and the v0.57.2 sources: hyprctl/src/main.cpp,
 * src/ipc/s1/{S1,Unix,Commands}.cpp, src/ipc/s2/S2.cpp, src/config/lua/...), plus the v0.54.3 and v0.56.2 sources
 * for the hyprlang side:
 *
 * Instances
 * - Each instance has a directory `$XDG_RUNTIME_DIR/hypr/<signature>/` (hyprctl falls back to
 *   `/run/user/<uid>/hypr`). The signature is `<commit>_<unix time>_<random>`.
 * - `hyprland.lock` in that directory holds the compositor PID on line 1 and its Wayland socket name
 *   (e.g. `wayland-1`) on line 2. A dead instance can leave its directory behind with `.socket2.sock` and
 *   `hyprland.log` but no `.socket.sock` and no lock file, and HYPRLAND_INSTANCE_SIGNATURE can still point at it.
 *
 * Request socket (`.socket.sock`)
 * - One request per connection. The client writes the request (no terminator); the server reads until EAGAIN,
 *   so the request should go out in one write and the client must not shut down its write side first.
 *   The server writes the reply and closes; read until EOF.
 * - hyprctl frames a request as `<flags>/<args joined by spaces>`: `hyprctl -j monitors all` sends
 *   `j/monitors all`, `hyprctl output create headless X` sends `/output create headless X`. The server takes
 *   everything before the first `/` as flags (j = JSON, r = refresh, a = all, c = config) only if no space
 *   comes first; a request without a `/` is taken as the bare command.
 * - Replies are plain text. Success for `output`, `eval`, `dispatch` and `keyword` is exactly `ok`.
 *   Failures are free text: `error: ...` for Lua errors, `unknown request`, `Name already taken`,
 *   `output not found`, `cannot remove a real display. Use the monitor keyword.`, ...
 *   hyprctl itself exits 0 for most of these; only replies starting with `error:` give exit status 7.
 * - `output create headless NAME` / `output remove NAME` (`add`/`destroy` are aliases). Only outputs created this
 *   way can be removed. A new headless output starts at 1920x1080@60 placed to the right of the layout.
 * - `j/status` (0.56+) replies `{"configProvider": "lua"|"hyprlang", "backend": "drm"}`. 0.54 and older reply
 *   `unknown request`. This is how the dialect is detected.
 *
 * Config dialects
 * - 0.57 is Lua-only: there is no `keyword` request (`unknown request`), `eval` and `repl` run Lua, and
 *   `dispatch X` runs `return hl.dispatch(X)` (so `dispatch hl.dsp.exec_cmd("true")` replies `ok`).
 * - 0.55/0.56 have both providers. With hyprlang, `eval` replies `eval is only supported with the lua config
 *   manager` and `dispatch NAME ARGS` uses the old dispatchers; with Lua, `keyword` replies
 *   `keyword can't work with non-legacy parsers. Use eval.`
 * - `eval CODE` replies `ok` or the error text; it does not return values. `repl CODE` returns the values the code
 *   returns (`repl return 1+1` replies `2`), or `ok` if there are none. Both try `return CODE;` first.
 *   Errors that a binding reports without raising (e.g. `hl.monitor: unknown field 'x'`) also come back as
 *   `error: ...` lines.
 * - Monitor rule, Lua: `hl.monitor({ output = "NAME", mode = "1280x720@60", position = "auto-right", scale = 1,
 *   transform = 0, bitdepth = 10, disabled = false })`. It merges into an existing rule for the same output
 *   (fields not given keep their values) and schedules a monitor reload. There is no way to delete a rule short
 *   of a config reload. `scale` is parsed as a string but a number works.
 * - Monitor rule, hyprlang: `keyword monitor NAME,MODE,POSITION,SCALE[,transform,T][,bitdepth,B]` or
 *   `keyword monitor NAME,disable`. It replaces the rule for that output.
 * - Per-device input option, Lua: `hl.device({ name = "DEVICE", sensitivity = -0.5 })` (device names have spaces
 *   replaced with `-`, as in `j/devices`). Values merge per device. Booleans must be Lua booleans or 0/1
 *   (`"yes"` is rejected); string fields accept numbers.
 * - Per-device input option, hyprlang: `keyword device[DEVICE]:sensitivity -0.5`.
 *
 * Event socket (`.socket2.sock`)
 * - Lines of `EVENT>>DATA\n`; DATA is cut at 1024 bytes and newlines in it become spaces.
 * - `output create headless APOLLO-TEST-probe` emitted, in order:
 *     createworkspace>>11
 *     createworkspacev2>>11,11
 *     monitoradded>>APOLLO-TEST-probe
 *     monitoraddedv2>>2,APOLLO-TEST-probe,
 *   and `output remove APOLLO-TEST-probe` emitted:
 *     moveworkspace>>11,HDMI-A-1
 *     moveworkspacev2>>11,11,HDMI-A-1
 *     focusedmon>>HDMI-A-1,7
 *     focusedmonv2>>HDMI-A-1,7
 *     destroyworkspace>>11
 *     destroyworkspacev2>>11,11
 *     monitorremoved>>APOLLO-TEST-probe
 *     monitorremovedv2>>2,APOLLO-TEST-probe,
 *   So `monitoraddedv2` / `monitorremovedv2` data is `ID,NAME,DESCRIPTION` (description empty for headless
 *   outputs; it may itself contain commas, the name cannot).
 */
// this include
#include "ipc.h"

// standard includes
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <tuple>

// platform includes
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// local includes
#include "src/logging.h"

using namespace std::literals;

namespace hyprland {
  namespace {
    constexpr auto probe_timeout = std::chrono::milliseconds {1000};
    constexpr std::size_t max_reply_size = 64 * 1024 * 1024;
    constexpr std::size_t max_event_line = 1024 * 1024;

    /**
     * @brief Owns a file descriptor.
     */
    class fd_t {
    public:
      explicit fd_t(int fd = -1):
          fd_ {fd} {
      }

      ~fd_t() {
        reset();
      }

      fd_t(const fd_t &) = delete;
      fd_t &operator=(const fd_t &) = delete;

      int get() const {
        return fd_;
      }

      int release() {
        return std::exchange(fd_, -1);
      }

      void reset(int fd = -1) {
        if (fd_ >= 0) {
          ::close(fd_);
        }
        fd_ = fd;
      }

    private:
      int fd_;
    };

    /**
     * @brief Connect a new stream socket to a UNIX socket path.
     * @return The connected socket, or -1.
     */
    int connect_unix(const std::filesystem::path &path, std::chrono::milliseconds timeout) {
      const auto &native = path.native();
      sockaddr_un addr {};
      if (native.size() >= sizeof(addr.sun_path)) {
        BOOST_LOG(debug) << "Hyprland: socket path too long: "sv << native;
        return -1;
      }
      addr.sun_family = AF_UNIX;
      std::memcpy(addr.sun_path, native.c_str(), native.size() + 1);

      fd_t fd {::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
      if (fd.get() < 0) {
        return -1;
      }

      // SO_SNDTIMEO also bounds connect() on a listener whose backlog is full.
      timeval tv {};
      tv.tv_sec = timeout.count() / 1000;
      tv.tv_usec = (timeout.count() % 1000) * 1000;
      ::setsockopt(fd.get(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
      ::setsockopt(fd.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

      int rc;
      do {
        rc = ::connect(fd.get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
      } while (rc < 0 && errno == EINTR);
      if (rc < 0) {
        return -1;
      }
      return fd.release();
    }

    bool socket_answers(const std::filesystem::path &socket_path) {
      auto reply = raw_request(socket_path, "j/status"sv, probe_timeout);
      return reply && !reply->empty();
    }

    bool valid_signature(std::string_view sig) {
      return !sig.empty() && sig != "."sv && sig != ".."sv && sig.find('/') == std::string_view::npos;
    }

    /**
     * @brief The start time encoded in a signature (`<commit>_<time>_<random>`), or 0.
     */
    std::uint64_t signature_time(std::string_view sig) {
      auto first = sig.find('_');
      if (first == std::string_view::npos) {
        return 0;
      }
      auto rest = sig.substr(first + 1);
      auto second = rest.find('_');
      auto field = rest.substr(0, second);
      std::uint64_t value = 0;
      std::from_chars(field.data(), field.data() + field.size(), value);
      return value;
    }

    /**
     * @brief The Wayland socket name from line 2 of `hyprland.lock`.
     */
    std::optional<std::string> lock_wayland_display(const std::filesystem::path &instance_dir) {
      std::ifstream in {instance_dir / "hyprland.lock"};
      if (!in) {
        return std::nullopt;
      }
      std::string pid, display;
      if (!std::getline(in, pid) || !std::getline(in, display)) {
        return std::nullopt;
      }
      return display;
    }

    bool is_ok(const std::optional<std::string> &reply) {
      return reply && *reply == "ok"sv;
    }

    /**
     * @brief A Lua eval/repl reply counts as success if no line reports an error.
     */
    bool lua_reply_ok(const std::optional<std::string> &reply) {
      if (!reply || *reply == "unknown request"sv) {
        return false;
      }
      std::string_view rest {*reply};
      while (!rest.empty()) {
        auto nl = rest.find('\n');
        auto line = rest.substr(0, nl);
        if (line.starts_with("error"sv)) {
          return false;
        }
        if (nl == std::string_view::npos) {
          break;
        }
        rest.remove_prefix(nl + 1);
      }
      return true;
    }

    bool is_lua_identifier(std::string_view str) {
      if (str.empty() || std::isdigit(static_cast<unsigned char>(str.front()))) {
        return false;
      }
      return std::ranges::all_of(str, [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
      });
    }

    bool is_decimal_number(std::string_view str) {
      if (str.starts_with('-')) {
        str.remove_prefix(1);
      }
      auto dot = str.find('.');
      auto int_part = str.substr(0, dot);
      auto all_digits = [](std::string_view s) {
        return !s.empty() && std::ranges::all_of(s, [](char c) {
          return c >= '0' && c <= '9';
        });
      };
      if (!all_digits(int_part)) {
        return false;
      }
      return dot == std::string_view::npos || all_digits(str.substr(dot + 1));
    }

    bool has_any(std::string_view str, std::string_view chars) {
      return str.find_first_of(chars) != std::string_view::npos;
    }

    std::string format_number(double value) {
      return std::format("{}", value);
    }

    void log_failure(std::string_view what, const std::optional<std::string> &reply) {
      BOOST_LOG(warning) << "Hyprland: "sv << what << " failed: "sv << (reply ? *reply : "no reply from IPC socket"s);
    }

    template<class T>
    T json_get(const nlohmann::json &obj, const char *key, T fallback) {
      auto it = obj.find(key);
      if (it == obj.end() || it->is_null()) {
        return fallback;
      }
      try {
        return it->get<T>();
      } catch (const nlohmann::json::exception &) {
        return fallback;
      }
    }
  }  // namespace

  std::filesystem::path runtime_dir() {
    if (const char *xdg = std::getenv("XDG_RUNTIME_DIR"); xdg && *xdg) {
      return std::filesystem::path {xdg} / "hypr";
    }
    return std::filesystem::path {std::format("/run/user/{}", ::getuid())} / "hypr";
  }

  std::optional<std::string> raw_request(const std::filesystem::path &socket_path, std::string_view cmd, std::chrono::milliseconds timeout) {
    fd_t fd {connect_unix(socket_path, timeout)};
    if (fd.get() < 0) {
      return std::nullopt;
    }

    // The server handles whatever it has read once the socket runs dry, so send the request in one go.
    std::size_t sent = 0;
    while (sent < cmd.size()) {
      auto n = ::send(fd.get(), cmd.data() + sent, cmd.size() - sent, MSG_NOSIGNAL);
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        return std::nullopt;
      }
      sent += static_cast<std::size_t>(n);
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string reply;
    char buffer[8192];
    while (true) {
      auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
      if (remaining.count() <= 0) {
        return std::nullopt;
      }
      pollfd pfd {fd.get(), POLLIN, 0};
      auto rc = ::poll(&pfd, 1, static_cast<int>(remaining.count()));
      if (rc < 0) {
        if (errno == EINTR) {
          continue;
        }
        return std::nullopt;
      }
      if (rc == 0) {
        return std::nullopt;
      }

      auto n = ::recv(fd.get(), buffer, sizeof(buffer), 0);
      if (n < 0) {
        if (errno == EINTR || errno == EAGAIN) {
          continue;
        }
        return std::nullopt;
      }
      if (n == 0) {
        break;
      }
      reply.append(buffer, static_cast<std::size_t>(n));
      if (reply.size() > max_reply_size) {
        return std::nullopt;
      }
    }
    return reply;
  }

  std::optional<std::string> find_instance_signature(const std::filesystem::path &hypr_dir, const char *env_signature, const char *wayland_display) {
    std::string env_sig = env_signature ? env_signature : "";
    if (valid_signature(env_sig) && socket_answers(hypr_dir / env_sig / ".socket.sock")) {
      return env_sig;
    }
    if (!env_sig.empty()) {
      BOOST_LOG(debug) << "Hyprland: HYPRLAND_INSTANCE_SIGNATURE="sv << env_sig << " does not answer, scanning "sv << hypr_dir.string();
    }

    struct candidate_t {
      std::string signature;
      std::uint64_t time;
      bool matches_display;
    };

    std::vector<candidate_t> candidates;
    std::error_code ec;
    std::filesystem::directory_iterator it {hypr_dir, std::filesystem::directory_options::skip_permission_denied, ec};
    if (ec) {
      return std::nullopt;
    }
    for (const auto &entry : it) {
      if (!entry.is_directory(ec)) {
        continue;
      }
      auto sig = entry.path().filename().string();
      if (sig == env_sig || !valid_signature(sig)) {
        continue;
      }
      if (!socket_answers(entry.path() / ".socket.sock")) {
        continue;
      }
      auto display = lock_wayland_display(entry.path());
      bool matches = wayland_display && *wayland_display && display && *display == wayland_display;
      candidates.push_back({std::move(sig), signature_time(entry.path().filename().string()), matches});
    }

    if (candidates.empty()) {
      return std::nullopt;
    }

    auto best = std::ranges::max_element(candidates, [](const candidate_t &a, const candidate_t &b) {
      return std::tie(a.matches_display, a.time, a.signature) < std::tie(b.matches_display, b.time, b.signature);
    });
    return best->signature;
  }

  std::optional<std::string> find_instance_signature() {
    return find_instance_signature(runtime_dir(), std::getenv("HYPRLAND_INSTANCE_SIGNATURE"), std::getenv("WAYLAND_DISPLAY"));
  }

  std::vector<monitor_info_t> parse_monitors(const nlohmann::json &json) {
    std::vector<monitor_info_t> result;
    if (!json.is_array()) {
      return result;
    }
    for (const auto &obj : json) {
      if (!obj.is_object()) {
        continue;
      }
      monitor_info_t m;
      m.name = json_get<std::string>(obj, "name", "");
      m.description = json_get<std::string>(obj, "description", "");
      m.id = json_get<int>(obj, "id", -1);
      m.width = json_get<int>(obj, "width", 0);
      m.height = json_get<int>(obj, "height", 0);
      m.refresh_hz = json_get<double>(obj, "refreshRate", 0.0);
      m.x = json_get<int>(obj, "x", 0);
      m.y = json_get<int>(obj, "y", 0);
      m.scale = json_get<double>(obj, "scale", 1.0);
      m.transform = json_get<int>(obj, "transform", 0);
      m.disabled = json_get<bool>(obj, "disabled", false);
      m.dpms_on = json_get<bool>(obj, "dpmsStatus", true);
      m.current_format = json_get<std::string>(obj, "currentFormat", "");
      m.focused = json_get<bool>(obj, "focused", false);
      m.cm_preset = json_get<std::string>(obj, "colorManagementPreset", "");
      if (auto ws = obj.find("activeWorkspace"); ws != obj.end() && ws->is_object()) {
        m.active_workspace = json_get<std::string>(*ws, "name", "");
      }
      result.push_back(std::move(m));
    }
    return result;
  }

  config_dialect_e parse_dialect(std::string_view status_reply) {
    auto json = nlohmann::json::parse(status_reply, nullptr, false);
    if (json.is_object() && json_get<std::string>(json, "configProvider", "") == "lua") {
      return config_dialect_e::lua;
    }
    return config_dialect_e::hyprlang;
  }

  std::optional<monitor_event_t> parse_monitor_event(std::string_view data) {
    auto comma = data.find(',');
    if (comma == std::string_view::npos) {
      return std::nullopt;
    }
    monitor_event_t event;
    auto id = data.substr(0, comma);
    auto [ptr, ec] = std::from_chars(id.data(), id.data() + id.size(), event.id);
    if (ec != std::errc {} || ptr != id.data() + id.size()) {
      return std::nullopt;
    }
    auto rest = data.substr(comma + 1);
    auto comma2 = rest.find(',');
    event.name = std::string {rest.substr(0, comma2)};
    if (comma2 != std::string_view::npos) {
      event.description = std::string {rest.substr(comma2 + 1)};
    }
    if (event.name.empty()) {
      return std::nullopt;
    }
    return event;
  }

  std::string lua_quote(std::string_view str) {
    std::string out;
    out.reserve(str.size() + 2);
    out += '"';
    for (char ch : str) {
      auto c = static_cast<unsigned char>(ch);
      switch (c) {
        case '\\':
          out += "\\\\";
          break;
        case '"':
          out += "\\\"";
          break;
        case '\n':
          out += "\\n";
          break;
        case '\r':
          out += "\\r";
          break;
        case '\t':
          out += "\\t";
          break;
        default:
          if (c < 0x20 || c == 0x7f) {
            // Three digits so a following digit is not read as part of the escape.
            out += std::format("\\{:03d}", c);
          } else {
            out += ch;
          }
      }
    }
    out += '"';
    return out;
  }

  std::string lua_value(std::string_view value) {
    if (value == "true"sv || value == "false"sv || is_decimal_number(value)) {
      return std::string {value};
    }
    return lua_quote(value);
  }

  namespace {
    bool finite_fields(const monitor_rule_t &rule) {
      for (const auto &value : {rule.scale, rule.sdr_brightness, rule.sdr_saturation, rule.min_luminance}) {
        if (value && !std::isfinite(*value)) {
          return false;
        }
      }
      return true;
    }
  }  // namespace

  std::optional<std::string> lua_monitor_call(const monitor_rule_t &rule) {
    if (rule.output.empty() || !finite_fields(rule)) {
      return std::nullopt;
    }
    std::string call = "hl.monitor({ output = " + lua_quote(rule.output);
    if (rule.mode) {
      call += ", mode = " + lua_quote(*rule.mode);
    }
    if (rule.position) {
      call += ", position = " + lua_quote(*rule.position);
    }
    if (rule.scale) {
      call += ", scale = " + format_number(*rule.scale);
    }
    if (rule.transform) {
      call += std::format(", transform = {}", *rule.transform);
    }
    if (rule.bitdepth) {
      call += std::format(", bitdepth = {}", *rule.bitdepth);
    }
    if (rule.disabled) {
      call += *rule.disabled ? ", disabled = true" : ", disabled = false";
    }
    if (rule.cm) {
      call += ", cm = " + lua_quote(*rule.cm);
    }
    if (rule.supports_hdr) {
      call += std::format(", supports_hdr = {}", *rule.supports_hdr);
    }
    if (rule.supports_wide_color) {
      call += std::format(", supports_wide_color = {}", *rule.supports_wide_color);
    }
    if (rule.sdr_brightness) {
      call += ", sdrbrightness = " + format_number(*rule.sdr_brightness);
    }
    if (rule.sdr_saturation) {
      call += ", sdrsaturation = " + format_number(*rule.sdr_saturation);
    }
    if (rule.min_luminance) {
      call += ", min_luminance = " + format_number(*rule.min_luminance);
    }
    if (rule.max_luminance) {
      call += std::format(", max_luminance = {}", *rule.max_luminance);
    }
    if (rule.max_avg_luminance) {
      call += std::format(", max_avg_luminance = {}", *rule.max_avg_luminance);
    }
    call += " })";
    return call;
  }

  std::optional<std::string> hyprlang_monitor_value(const monitor_rule_t &rule) {
    constexpr auto separators = ",\n\r"sv;
    if (rule.output.empty() || has_any(rule.output, separators) || !finite_fields(rule) || (rule.cm && has_any(*rule.cm, separators))) {
      return std::nullopt;
    }
    if (rule.disabled.value_or(false)) {
      return rule.output + ",disable";
    }
    auto mode = rule.mode.value_or("preferred");
    auto position = rule.position.value_or("auto");
    if (has_any(mode, separators) || has_any(position, separators)) {
      return std::nullopt;
    }
    auto value = std::format("{},{},{},{}", rule.output, mode, position, rule.scale ? format_number(*rule.scale) : "auto"s);
    if (rule.transform) {
      value += std::format(",transform,{}", *rule.transform);
    }
    if (rule.bitdepth) {
      value += std::format(",bitdepth,{}", *rule.bitdepth);
    }
    if (rule.cm) {
      if (has_any(*rule.cm, separators)) {
        return std::nullopt;
      }
      value += ",cm," + *rule.cm;
    }
    if (rule.sdr_brightness) {
      value += ",sdrbrightness," + format_number(*rule.sdr_brightness);
    }
    if (rule.sdr_saturation) {
      value += ",sdrsaturation," + format_number(*rule.sdr_saturation);
    }
    return value;
  }

  std::optional<std::string> lua_device_call(const std::string &device, const std::string &key, const std::string &value) {
    if (device.empty() || key.empty()) {
      return std::nullopt;
    }
    auto field = is_lua_identifier(key) ? key : "[" + lua_quote(key) + "]";
    return std::format("hl.device({{ name = {}, {} = {} }})", lua_quote(device), field, lua_value(value));
  }

  std::optional<std::string> lua_dpms_call(const std::string &output, bool on) {
    if (output.empty()) {
      return std::nullopt;
    }
    return std::format("hl.dsp.dpms({{ action = {}, monitor = {} }})", on ? "\"on\""sv : "\"off\""sv, lua_quote(output));
  }

  ipc_t::ipc_t(std::filesystem::path instance_dir, std::string signature, config_dialect_e dialect):
      instance_dir_ {std::move(instance_dir)},
      signature_ {std::move(signature)},
      dialect_ {dialect} {
  }

  std::unique_ptr<ipc_t> ipc_t::connect(const std::filesystem::path &hypr_dir, const std::string &signature) {
    if (!valid_signature(signature)) {
      return nullptr;
    }
    auto instance_dir = hypr_dir / signature;
    auto status = raw_request(instance_dir / ".socket.sock", "j/status"sv, probe_timeout);
    if (!status || status->empty()) {
      return nullptr;
    }
    auto dialect = parse_dialect(*status);
    BOOST_LOG(debug) << "Hyprland: connected to instance "sv << signature << " ("sv << (dialect == config_dialect_e::lua ? "lua"sv : "hyprlang"sv) << " config)"sv;
    return std::unique_ptr<ipc_t> {new ipc_t {std::move(instance_dir), signature, dialect}};
  }

  std::unique_ptr<ipc_t> ipc_t::connect() {
    auto sig = find_instance_signature();
    if (!sig) {
      return nullptr;
    }
    return connect(runtime_dir(), *sig);
  }

  std::optional<std::string> ipc_t::request(std::string_view cmd) const {
    return raw_request(socket_path(), cmd);
  }

  std::optional<nlohmann::json> ipc_t::request_json(std::string_view cmd) const {
    std::string full;
    auto slash = cmd.find('/');
    auto space = cmd.find(' ');
    if (slash != std::string_view::npos && slash < space) {
      // Already has a flags prefix; make sure it asks for JSON.
      auto flags = cmd.substr(0, slash);
      full = flags.find('j') == std::string_view::npos ? "j" + std::string {cmd} : std::string {cmd};
    } else {
      full = "j/" + std::string {cmd};
    }

    auto reply = request(full);
    if (!reply) {
      return std::nullopt;
    }
    auto json = nlohmann::json::parse(*reply, nullptr, false);
    if (json.is_discarded()) {
      BOOST_LOG(debug) << "Hyprland: non-JSON reply to "sv << full << ": "sv << *reply;
      return std::nullopt;
    }
    return json;
  }

  std::vector<monitor_info_t> ipc_t::monitors(bool all) const {
    auto json = request_json(all ? "monitors all"sv : "monitors"sv);
    if (!json) {
      return {};
    }
    return parse_monitors(*json);
  }

  bool ipc_t::create_headless(const std::string &name) const {
    if (name.empty() || has_any(name, " \t\n\r"sv)) {
      return false;
    }
    auto reply = request("/output create headless " + name);
    if (!is_ok(reply)) {
      log_failure("output create headless " + name, reply);
      return false;
    }
    return true;
  }

  bool ipc_t::remove_output(const std::string &name) const {
    if (name.empty() || has_any(name, " \t\n\r"sv)) {
      return false;
    }
    auto reply = request("/output remove " + name);
    if (!is_ok(reply)) {
      log_failure("output remove " + name, reply);
      return false;
    }
    return true;
  }

  bool ipc_t::set_monitor(const monitor_rule_t &rule) const {
    if (dialect_ == config_dialect_e::lua) {
      auto call = lua_monitor_call(rule);
      if (!call) {
        return false;
      }
      auto reply = request("/eval " + *call);
      if (!lua_reply_ok(reply)) {
        log_failure(*call, reply);
        return false;
      }
      return true;
    }

    auto value = hyprlang_monitor_value(rule);
    if (!value) {
      return false;
    }
    return keyword("monitor", *value);
  }

  bool ipc_t::set_device_option(const std::string &device, const std::string &key, const std::string &value) const {
    if (dialect_ == config_dialect_e::lua) {
      auto call = lua_device_call(device, key, value);
      if (!call) {
        return false;
      }
      auto reply = request("/eval " + *call);
      if (!lua_reply_ok(reply)) {
        log_failure(*call, reply);
        return false;
      }
      return true;
    }

    if (device.empty() || key.empty() || has_any(device, "] \t\n\r"sv) || has_any(key, " \t\n\r"sv)) {
      return false;
    }
    return keyword(std::format("device[{}]:{}", device, key), value);
  }

  bool ipc_t::set_dpms(const std::string &output, bool on) const {
    if (dialect_ == config_dialect_e::lua) {
      auto call = lua_dpms_call(output, on);
      return call && dispatch_lua(*call);
    }
    if (output.empty() || has_any(output, " \t\n\r"sv)) {
      return false;
    }
    return dispatch_legacy("dpms", std::format("{} {}", on ? "on" : "off", output));
  }

  bool ipc_t::dispatch_lua(const std::string &lua_call) const {
    if (dialect_ != config_dialect_e::lua) {
      BOOST_LOG(warning) << "Hyprland: Lua dispatch on a hyprlang config: "sv << lua_call;
      return false;
    }
    auto reply = request("/dispatch " + lua_call);
    if (!is_ok(reply)) {
      log_failure("dispatch " + lua_call, reply);
      return false;
    }
    return true;
  }

  bool ipc_t::dispatch_legacy(const std::string &dispatcher, const std::string &args) const {
    if (dialect_ != config_dialect_e::hyprlang) {
      BOOST_LOG(warning) << "Hyprland: legacy dispatch on a Lua config: "sv << dispatcher;
      return false;
    }
    auto cmd = args.empty() ? "/dispatch " + dispatcher : std::format("/dispatch {} {}", dispatcher, args);
    auto reply = request(cmd);
    if (!is_ok(reply)) {
      log_failure(cmd, reply);
      return false;
    }
    return true;
  }

  std::optional<std::string> ipc_t::eval(const std::string &lua) const {
    if (dialect_ != config_dialect_e::lua) {
      return std::nullopt;
    }
    auto reply = request("/repl " + lua);
    if (!lua_reply_ok(reply)) {
      log_failure("eval " + lua, reply);
      return std::nullopt;
    }
    return reply;
  }

  bool ipc_t::keyword(const std::string &name, const std::string &value) const {
    if (dialect_ != config_dialect_e::hyprlang) {
      BOOST_LOG(warning) << "Hyprland: keyword on a Lua config: "sv << name;
      return false;
    }
    auto cmd = std::format("/keyword {} {}", name, value);
    auto reply = request(cmd);
    if (!is_ok(reply)) {
      log_failure(cmd, reply);
      return false;
    }
    return true;
  }

  config_dialect_e ipc_t::dialect() const {
    return dialect_;
  }

  const std::string &ipc_t::signature() const {
    return signature_;
  }

  std::filesystem::path ipc_t::socket_path() const {
    return instance_dir_ / ".socket.sock";
  }

  std::filesystem::path ipc_t::event_socket_path() const {
    return instance_dir_ / ".socket2.sock";
  }

  event_listener_t::event_listener_t():
      wake_fd_ {::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)} {
  }

  event_listener_t::event_listener_t(std::filesystem::path socket2_path):
      fixed_path_ {std::move(socket2_path)},
      wake_fd_ {::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)} {
  }

  event_listener_t::~event_listener_t() {
    stop();
    if (wake_fd_ >= 0) {
      ::close(wake_fd_);
    }
  }

  event_listener_t::token_t event_listener_t::subscribe(callback_t callback) {
    std::lock_guard lock {subscribers_mutex_};
    auto token = next_token_++;
    subscribers_.emplace(token, std::make_shared<callback_t>(std::move(callback)));
    return token;
  }

  void event_listener_t::unsubscribe(token_t token) {
    std::lock_guard lock {subscribers_mutex_};
    subscribers_.erase(token);
  }

  void event_listener_t::set_reconnect_interval(std::chrono::milliseconds interval) {
    reconnect_interval_ = interval;
  }

  bool event_listener_t::connected() const {
    return connected_;
  }

  bool event_listener_t::start() {
    if (wake_fd_ < 0) {
      return false;
    }
    if (thread_.joinable()) {
      return connected_;
    }

    // Clear a wake-up left over from a previous stop().
    eventfd_t ignored;
    ::eventfd_read(wake_fd_, &ignored);
    stopping_ = false;
    {
      std::lock_guard lock {start_mutex_};
      first_attempt_done_ = false;
    }

    thread_ = std::thread {&event_listener_t::run, this};

    std::unique_lock lock {start_mutex_};
    start_cv_.wait(lock, [this] {
      return first_attempt_done_;
    });
    return connected_;
  }

  void event_listener_t::stop() {
    stopping_ = true;
    if (wake_fd_ >= 0) {
      ::eventfd_write(wake_fd_, 1);
    }
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
      thread_.join();
    }
  }

  int event_listener_t::open_socket() {
    std::filesystem::path path;
    if (fixed_path_) {
      path = *fixed_path_;
    } else {
      auto sig = find_instance_signature();
      if (!sig) {
        return -1;
      }
      path = runtime_dir() / *sig / ".socket2.sock";
    }

    int fd = connect_unix(path, probe_timeout);
    if (fd < 0) {
      return -1;
    }
    // Reads are driven by poll(); drop the timeouts connect_unix() set.
    timeval tv {};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    BOOST_LOG(debug) << "Hyprland: listening for events on "sv << path.string();
    return fd;
  }

  void event_listener_t::dispatch_line(std::string_view line) {
    auto sep = line.find(">>"sv);
    auto event = line.substr(0, sep);
    auto data = sep == std::string_view::npos ? std::string_view {} : line.substr(sep + 2);

    std::vector<std::shared_ptr<callback_t>> callbacks;
    {
      std::lock_guard lock {subscribers_mutex_};
      callbacks.reserve(subscribers_.size());
      for (const auto &[token, callback] : subscribers_) {
        callbacks.push_back(callback);
      }
    }
    for (const auto &callback : callbacks) {
      try {
        (*callback)(event, data);
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "Hyprland: event callback for "sv << event << " threw: "sv << e.what();
      }
    }
  }

  void event_listener_t::run() {
    auto signal_first_attempt = [this]() {
      std::lock_guard lock {start_mutex_};
      if (!first_attempt_done_) {
        first_attempt_done_ = true;
        start_cv_.notify_all();
      }
    };

    // Wait for the reconnect interval; returns false if stop() was called.
    auto wait_interval = [this]() {
      pollfd pfd {wake_fd_, POLLIN, 0};
      int rc;
      do {
        rc = ::poll(&pfd, 1, static_cast<int>(reconnect_interval_.count()));
      } while (rc < 0 && errno == EINTR);
      return !stopping_;
    };

    bool logged_failure = false;
    while (!stopping_) {
      fd_t fd {open_socket()};
      if (fd.get() < 0) {
        if (!logged_failure) {
          BOOST_LOG(warning) << "Hyprland: cannot connect to the event socket, retrying"sv;
          logged_failure = true;
        }
        signal_first_attempt();
        if (!wait_interval()) {
          break;
        }
        continue;
      }
      logged_failure = false;
      connected_ = true;
      signal_first_attempt();

      std::string buffer;
      char chunk[4096];
      while (!stopping_) {
        pollfd pfds[2] {{fd.get(), POLLIN, 0}, {wake_fd_, POLLIN, 0}};
        auto rc = ::poll(pfds, 2, -1);
        if (rc < 0) {
          if (errno == EINTR) {
            continue;
          }
          break;
        }
        if (pfds[1].revents & POLLIN) {
          break;
        }
        if (!(pfds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
          continue;
        }

        auto n = ::recv(fd.get(), chunk, sizeof(chunk), 0);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
          continue;
        }
        if (n <= 0) {
          BOOST_LOG(debug) << "Hyprland: event socket closed"sv;
          break;
        }

        buffer.append(chunk, static_cast<std::size_t>(n));
        std::size_t start = 0;
        for (auto nl = buffer.find('\n'); nl != std::string::npos; nl = buffer.find('\n', start)) {
          dispatch_line(std::string_view {buffer}.substr(start, nl - start));
          start = nl + 1;
        }
        buffer.erase(0, start);
        if (buffer.size() > max_event_line) {
          buffer.clear();
        }
      }

      connected_ = false;
      if (!stopping_ && !wait_interval()) {
        break;
      }
    }

    connected_ = false;
    signal_first_attempt();
  }
}  // namespace hyprland
