/**
 * @file src/platform/linux/hyprland/ipc.h
 * @brief Declarations for a client of Hyprland's IPC sockets.
 *
 * Hyprland exposes two UNIX sockets per instance under `$XDG_RUNTIME_DIR/hypr/<signature>/`:
 * - `.socket.sock` takes one request per connection (e.g. `j/monitors all`), replies, then closes.
 * - `.socket2.sock` streams events as `EVENT>>DATA\n` lines.
 *
 * See the comment block at the top of ipc.cpp for the protocol details verified against Hyprland 0.57.2.
 * Only compiled when Wayland support is enabled (`SUNSHINE_BUILD_WAYLAND`).
 */
#pragma once

// standard includes
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

namespace hyprland {
  /**
   * @brief Which config language the running Hyprland uses.
   *
   * Hyprland 0.55 and 0.56 support both; 0.57 only supports Lua; 0.54 and older only hyprlang.
   */
  enum class config_dialect_e {
    lua,  ///< `hl.*` Lua config: `eval`, `dispatch <lua call>`, no `keyword`.
    hyprlang,  ///< Legacy hyprlang config: `keyword`, `dispatch <name> <args>`, no `eval`.
  };

  /**
   * @brief One entry of `j/monitors all`.
   */
  struct monitor_info_t {
    std::string name;  ///< Connector name, e.g. `DP-3` or `HEADLESS-2`.
    std::string description;
    int id = -1;
    int width = 0;  ///< Mode width in pixels.
    int height = 0;  ///< Mode height in pixels.
    double refresh_hz = 0.0;
    int x = 0;  ///< Position in the layout (logical pixels).
    int y = 0;
    double scale = 1.0;
    int transform = 0;  ///< wl_output_transform value (0-7).
    bool disabled = false;
    bool dpms_on = true;  ///< `dpmsStatus`: false while the output is switched off (it then captures as black).
    std::string current_format;  ///< DRM format name, e.g. `XRGB8888` or `XRGB2101010`.
    bool focused = false;
    std::string cm_preset;  ///< `colorManagementPreset`: `srgb`, `wide`, `hdr`, `hdredid`, ... (empty if not reported).
    std::string active_workspace;  ///< Name of the workspace shown (`activeWorkspace.name`), e.g. `1` or `apollo-1a2b3c4d`.
  };

  /**
   * @brief A monitor rule. Fields left empty are not emitted.
   *
   * With the Lua dialect `hl.monitor()` merges into an existing rule for the same output, so empty fields keep
   * their previous value. With hyprlang the whole rule is replaced and empty fields fall back to
   * `preferred`, `auto` and `auto`.
   */
  struct monitor_rule_t {
    std::string output;  ///< Connector name; required.
    std::optional<std::string> mode;  ///< e.g. `1280x720@60`, `preferred`, `highrr`.
    std::optional<std::string> position;  ///< e.g. `0x0`, `auto`, `auto-right`.
    std::optional<double> scale;
    std::optional<int> transform;  ///< 0-7.
    std::optional<int> bitdepth;  ///< 8 or 10.
    std::optional<bool> disabled;

    // Color management (Hyprland 0.57 names). The Lua dialect emits all of them; hyprlang's `monitor` keyword only
    // takes `cm`, `sdrbrightness` and `sdrsaturation` (the others need `monitorv2`), so the rest is left out there.
    std::optional<std::string> cm;  ///< Preset: `auto`, `srgb`, `wide`, `hdr`, `hdredid`, ...
    std::optional<int> supports_hdr;  ///< 1 forces HDR support on (e.g. headless outputs, which have no EDID), -1 off.
    std::optional<int> supports_wide_color;  ///< Same for wide color.
    std::optional<double> sdr_brightness;  ///< `sdrbrightness`: SDR content brightness in HDR mode.
    std::optional<double> sdr_saturation;  ///< `sdrsaturation`.
    std::optional<double> min_luminance;  ///< cd/m², overrides the EDID.
    std::optional<int> max_luminance;  ///< cd/m², overrides the EDID.
    std::optional<int> max_avg_luminance;  ///< cd/m², overrides the EDID.

    friend bool operator==(const monitor_rule_t &, const monitor_rule_t &) = default;
  };

  /**
   * @brief Payload of `monitoraddedv2` and `monitorremovedv2` events: `ID,NAME,DESCRIPTION`.
   */
  struct monitor_event_t {
    int id = -1;
    std::string name;
    std::string description;
  };

  /**
   * @brief The directory holding the instance directories: `$XDG_RUNTIME_DIR/hypr`, or `/run/user/<uid>/hypr`.
   */
  std::filesystem::path runtime_dir();

  /**
   * @brief Find the signature of a live Hyprland instance.
   *
   * Uses `HYPRLAND_INSTANCE_SIGNATURE` if its socket answers. Otherwise scans `runtime_dir()` for instance
   * directories whose `.socket.sock` answers, preferring the one whose `hyprland.lock` names `WAYLAND_DISPLAY`,
   * then the most recently started one.
   * @return The signature (directory name), or nullopt if no instance answers.
   */
  std::optional<std::string> find_instance_signature();

  /**
   * @brief Testable form of find_instance_signature().
   * @param hypr_dir Directory containing the instance directories.
   * @param env_signature Value of `HYPRLAND_INSTANCE_SIGNATURE`, or nullptr.
   * @param wayland_display Value of `WAYLAND_DISPLAY`, or nullptr.
   */
  std::optional<std::string> find_instance_signature(
    const std::filesystem::path &hypr_dir,
    const char *env_signature,
    const char *wayland_display
  );

  /**
   * @brief Send one request to a `.socket.sock` and read the whole reply.
   * @return The reply, or nullopt if the socket could not be reached or timed out.
   */
  std::optional<std::string> raw_request(
    const std::filesystem::path &socket_path,
    std::string_view cmd,
    std::chrono::milliseconds timeout = std::chrono::seconds {5}
  );

  /**
   * @brief Parse a `j/monitors` reply. Entries that are not objects are skipped.
   */
  std::vector<monitor_info_t> parse_monitors(const nlohmann::json &json);

  /**
   * @brief Determine the config dialect from a `j/status` reply.
   *
   * `{"configProvider": "lua", ...}` means Lua. Any other provider, or a reply that is not that JSON
   * (Hyprland 0.54 and older answer `unknown request`), means hyprlang.
   */
  config_dialect_e parse_dialect(std::string_view status_reply);

  /**
   * @brief Parse the data of a `monitoraddedv2` / `monitorremovedv2` event.
   */
  std::optional<monitor_event_t> parse_monitor_event(std::string_view data);

  /**
   * @brief Quote a string as a Lua double-quoted string literal, escaping as needed.
   */
  std::string lua_quote(std::string_view str);

  /**
   * @brief Format a Lua value for a config field: `true`/`false` stay booleans, decimal numbers stay numbers,
   * everything else becomes a quoted string.
   */
  std::string lua_value(std::string_view value);

  /**
   * @brief The Lua call that applies a monitor rule, e.g. `hl.monitor({ output = "DP-1", mode = "1280x720@60" })`.
   * @return The call, or nullopt if the rule is invalid (empty output, non-finite scale).
   */
  std::optional<std::string> lua_monitor_call(const monitor_rule_t &rule);

  /**
   * @brief The hyprlang `monitor` keyword value for a rule, e.g. `DP-1,1280x720@60,auto,auto`.
   * @return The value, or nullopt if the rule cannot be expressed (empty output, separators in fields).
   */
  std::optional<std::string> hyprlang_monitor_value(const monitor_rule_t &rule);

  /**
   * @brief The Lua call that sets a per-device input option, e.g. `hl.device({ name = "my-mouse", sensitivity = -0.5 })`.
   * @return The call, or nullopt if the device or key is empty.
   */
  std::optional<std::string> lua_device_call(const std::string &device, const std::string &key, const std::string &value);

  /**
   * @brief The Lua dispatcher call switching one output on or off, e.g.
   * `hl.dsp.dpms({ action = "on", monitor = "APOLLO-1a2b3c4d" })`.
   * @return nullopt if the output name is empty (that would switch every output).
   */
  std::optional<std::string> lua_dpms_call(const std::string &output, bool on);

  /**
   * @brief A client for a Hyprland instance's request socket.
   *
   * Each request opens its own connection, so one ipc_t may be used from several threads.
   */
  class ipc_t {
  public:
    /**
     * @brief Connect to the live instance found by find_instance_signature().
     * @return nullptr if no live instance answers.
     */
    static std::unique_ptr<ipc_t> connect();

    /**
     * @brief Connect to a specific instance.
     * @return nullptr if its socket does not answer.
     */
    static std::unique_ptr<ipc_t> connect(const std::filesystem::path &hypr_dir, const std::string &signature);

    /**
     * @brief Raw request, sent as given (e.g. `j/monitors all`, `/version`).
     * @return The reply, or nullopt on socket failure.
     */
    std::optional<std::string> request(std::string_view cmd) const;

    /**
     * @brief Request a JSON reply. `j/` is prepended unless `cmd` already starts with flags and a `/`.
     * @return The parsed JSON, or nullopt on socket failure or if the reply is not JSON.
     */
    std::optional<nlohmann::json> request_json(std::string_view cmd) const;

    /**
     * @brief The monitors, from `j/monitors all` (or `j/monitors` if `all` is false, which omits disabled ones).
     * @return Empty on failure.
     */
    std::vector<monitor_info_t> monitors(bool all = true) const;

    /**
     * @brief `output create headless NAME`.
     */
    bool create_headless(const std::string &name) const;

    /**
     * @brief `output remove NAME`. Hyprland only removes outputs created this way, not physical ones.
     */
    bool remove_output(const std::string &name) const;

    /**
     * @brief Apply a monitor rule: `eval hl.monitor({...})` on Lua, `keyword monitor ...` on hyprlang.
     */
    bool set_monitor(const monitor_rule_t &rule) const;

    /**
     * @brief Set a per-device input option: `eval hl.device({...})` on Lua, `keyword device[NAME]:KEY VALUE` on hyprlang.
     * @param device Device name as listed by `j/devices`.
     */
    bool set_device_option(const std::string &device, const std::string &key, const std::string &value) const;

    /**
     * @brief Switch one output on or off: `dispatch hl.dsp.dpms({...})` on Lua, `dispatch dpms on|off NAME` on
     * hyprlang. Never touches other outputs; an empty name is refused.
     */
    bool set_dpms(const std::string &output, bool on) const;

    /**
     * @brief `dispatch <lua_call>` (Lua dialect only), e.g. `hl.dsp.focus({ workspace = 4 })`.
     * @return false on hyprlang configs or if Hyprland reports an error.
     */
    bool dispatch_lua(const std::string &lua_call) const;

    /**
     * @brief `dispatch <dispatcher> <args>` (hyprlang dialect only), e.g. `focusmonitor DP-1`.
     * @return false on Lua configs or if Hyprland reports an error.
     */
    bool dispatch_legacy(const std::string &dispatcher, const std::string &args) const;

    /**
     * @brief Run Lua code (Lua dialect only).
     *
     * Sent as `repl <lua>` so returned values come back: `eval("return 1+1")` yields `"2"`. Code that returns
     * nothing yields `"ok"`.
     * @return The reply, or nullopt on hyprlang configs, socket failure, or a Lua error.
     */
    std::optional<std::string> eval(const std::string &lua) const;

    /**
     * @brief `keyword NAME VALUE` (hyprlang dialect only).
     */
    bool keyword(const std::string &name, const std::string &value) const;

    /**
     * @brief The config dialect, determined from `j/status` when connecting.
     */
    config_dialect_e dialect() const;

    const std::string &signature() const;
    std::filesystem::path socket_path() const;
    std::filesystem::path event_socket_path() const;

  private:
    ipc_t(std::filesystem::path instance_dir, std::string signature, config_dialect_e dialect);

    std::filesystem::path instance_dir_;
    std::string signature_;
    config_dialect_e dialect_;
  };

  /**
   * @brief Reads `.socket2.sock` on its own thread and passes each event to the subscribers.
   *
   * Callbacks run on the listener thread and must not call stop() or destroy the listener.
   * If the socket drops (e.g. Hyprland restarts), the listener reconnects, rediscovering the instance
   * unless it was given a fixed socket path.
   */
  class event_listener_t {
  public:
    using callback_t = std::function<void(std::string_view event, std::string_view data)>;
    using token_t = std::uint64_t;

    /**
     * @brief Listen to the instance found by find_instance_signature() (looked up again on every reconnect).
     */
    event_listener_t();

    /**
     * @brief Listen to a fixed `.socket2.sock` path.
     */
    explicit event_listener_t(std::filesystem::path socket2_path);

    ~event_listener_t();

    event_listener_t(const event_listener_t &) = delete;
    event_listener_t &operator=(const event_listener_t &) = delete;

    /**
     * @brief Register a callback for every event. May be called before or after start().
     * @return A token for unsubscribe().
     */
    token_t subscribe(callback_t callback);

    /**
     * @brief Remove a callback. It may still be running on the listener thread when this returns.
     */
    void unsubscribe(token_t token);

    /**
     * @brief Start the listener thread. Blocks until the first connection attempt has finished, so events
     * caused after a successful start() are seen.
     * @return true if connected; false if the first attempt failed (the thread keeps retrying).
     */
    bool start();

    /**
     * @brief Stop and join the listener thread.
     */
    void stop();

    /**
     * @brief Whether the listener currently has the event socket open.
     */
    bool connected() const;

    /**
     * @brief Delay between reconnect attempts. Set before start().
     */
    void set_reconnect_interval(std::chrono::milliseconds interval);

  private:
    void run();
    int open_socket();
    void dispatch_line(std::string_view line);

    std::optional<std::filesystem::path> fixed_path_;
    std::chrono::milliseconds reconnect_interval_ {1000};

    std::mutex subscribers_mutex_;
    std::map<token_t, std::shared_ptr<callback_t>> subscribers_;
    token_t next_token_ = 1;

    std::thread thread_;
    int wake_fd_ = -1;  ///< eventfd used to interrupt poll() on stop().
    std::atomic<bool> stopping_ {false};
    std::atomic<bool> connected_ {false};

    std::mutex start_mutex_;
    std::condition_variable start_cv_;
    bool first_attempt_done_ = false;
  };
}  // namespace hyprland
