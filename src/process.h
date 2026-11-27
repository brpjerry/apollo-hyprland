/**
 * @file src/process.h
 * @brief Declarations for the startup and shutdown of the apps started by a streaming Session.
 */
#pragma once

#ifndef __kernel_entry
  /**
   * @def __kernel_entry
   * @brief Macro for kernel entry.
   */
  #define __kernel_entry
#endif

#ifndef BOOST_PROCESS_VERSION
  #define BOOST_PROCESS_VERSION 1
#endif

// standard includes
#include <optional>
#include <unordered_map>

// lib includes
#include <boost/property_tree/ptree.hpp>
#include <nlohmann/json.hpp>

// local includes
#include "config.h"
#include "platform/common.h"
#include "platform/vdisplay.h"
#include "rtsp.h"
#include "src/compat/boost_process.h"
#include "utility.h"
#include "uuid.h"

#define VIRTUAL_DISPLAY_UUID "8902CB19-674A-403D-A587-41B092E900BA"
#define FALLBACK_DESKTOP_UUID "EAAC6159-089A-46A9-9E24-6436885F6610"
#define REMOTE_INPUT_UUID "8CB5C136-DA67-4F99-B4A1-F9CD35005CF4"
#define TERMINATE_APP_UUID "E16CBE1B-295D-4632-9A76-EC4180C857D3"

/**
 * @def DEFAULT_APP_IMAGE_PATH
 * @brief Macro for DEFAULT APP IMAGE PATH.
 */
#define DEFAULT_APP_IMAGE_PATH SUNSHINE_ASSETS_DIR "/box.png"

namespace proc {
  /**
   * @brief Boost.Process pipe stream used for child-process I/O.
   */
  using file_t = util::safe_ptr_v2<FILE, int, &fclose>;

  /**
   * @brief Parsed command arguments used when launching a child process.
   */
  using cmd_t = config::prep_cmd_t;

  /**
   * pre_cmds -- guaranteed to be executed unless any of the commands fail.
   * detached -- commands detached from Sunshine
   * cmd -- Runs indefinitely until:
   *    No session is running and a different set of commands it to be executed
   *    Command exits
   * working_dir -- the process working directory. This is required for some games to run properly.
   * cmd_output --
   *    empty    -- The output of the commands are appended to the output of sunshine
   *    "null"   -- The output of the commands are discarded
   *    filename -- The output of the commands are appended to filename
   */
  struct ctx_t {
    std::vector<cmd_t> prep_cmds;  ///< Prep cmds.
    std::vector<cmd_t> state_cmds;  ///< Apollo: commands run when the stream pauses or resumes.

    /**
     * Some applications, such as Steam, either exit quickly, or keep running indefinitely.
     *
     * Apps that launch normal child processes and terminate will be handled by the process
     * grouping logic (wait_all). However, apps that launch child processes indirectly or
     * into another process group (such as UWP apps) can only be handled by the auto-detach
     * heuristic which catches processes that exit 0 very quickly, but we won't have proper
     * process tracking for those.
     *
     * For cases where users just want to kick off a background process and never manage the
     * lifetime of that process, they can use detached commands for that.
     */
    std::vector<std::string> detached;

    std::string idx;  ///< Apollo: position of the app in the list.
    std::string uuid;  ///< Apollo: stable UUID of the app.
    std::string name;  ///< Human-readable name for this item.
    std::string cmd;  ///< Command line used to launch the application.
    std::string working_dir;  ///< Working dir.
    std::string output;  ///< Captured output from the launched process.
    std::string image_path;  ///< Image path.
    std::string id;  ///< Stable identifier for the configured application.
    std::string gamepad;  ///< Apollo: per-app gamepad override ("disabled" turns controllers off).
    bool elevated;  ///< Whether the process should be launched elevated.
    bool auto_detach;  ///< Whether the process should detach automatically.
    bool wait_all;  ///< Whether Sunshine waits for all child processes.
    bool virtual_display;  ///< Apollo: always stream this app on a virtual display.
    bool use_app_identity;  ///< Apollo: the virtual display belongs to the app instead of the client.
    bool per_client_app_identity;  ///< Apollo: one virtual display per client and app.
    bool allow_client_commands;  ///< Apollo: run the client's do/undo commands for this app.
    bool terminate_on_pause;  ///< Apollo: terminate the app when the last client disconnects.
    int scale_factor;  ///< Apollo: render scale factor in percent.
    std::chrono::seconds exit_timeout;  ///< Exit timeout.
  };

  /**
   * @brief Tracks launched child processes and terminates them during shutdown.
   */
  class proc_t {
  public:
    KITTY_DEFAULT_CONSTR_MOVE_THROW(proc_t)

    std::string display_name;  ///< Apollo: name of the virtual display created for the running app.
    std::string initial_display;  ///< Apollo: configured output name to restore after the app ends.
    std::string mode_changed_display;  ///< Apollo: display whose HDR state was changed for the app.
    bool initial_hdr = false;  ///< Apollo: HDR state of mode_changed_display before the app started.
    bool virtual_display = false;  ///< Apollo: the running app streams a virtual display.
    bool allow_client_commands = false;  ///< Apollo: client do/undo commands may run for the running app.

    /**
     * @brief Construct a process manager.
     *
     * @param env Environment used when launching processes.
     * @param apps Application launch contexts.
     */
    proc_t(
      boost::process::v1::environment &&env,
      std::vector<ctx_t> &&apps
    ):
        _env(std::move(env)),
        _apps(std::move(apps)) {
    }

    /**
     * @brief Mark the input-only ("Remote Input") app as running, without launching anything (Apollo).
     */
    void launch_input_only();

    /**
     * @brief Launch an application for a streaming session (Apollo).
     *
     * Creates the session's virtual display when one is needed (on Hyprland a backend failure ends the launch with
     * 503 instead of streaming the host's desktop), configures the display, probes encoders and runs the commands.
     *
     * @param _app Application to launch.
     * @param launch_session Launch session.
     * @return 0 on success, 503 when video capture or the virtual display could not be prepared, -1 on command errors.
     */
    int execute(const ctx_t &_app, std::shared_ptr<rtsp_stream::launch_session_t> launch_session);

    /**
     * @brief Launch the configured application with the given ID.
     *
     * @param app_id App ID.
     * @param launch_session Launch session.
     * @return Like execute(const ctx_t &, ...), or 404 when no app has that ID.
     */
    int execute(int app_id, std::shared_ptr<rtsp_stream::launch_session_t> launch_session);

    /**
     * @return `_app_id` if a process is running, otherwise returns `0`
     */
    int running();

    ~proc_t();

    /**
     * @brief Return the configured applications.
     *
     * @return Immutable application list owned by the process manager.
     */
    const std::vector<ctx_t> &get_apps() const;
    /**
     * @brief Return the configured applications.
     *
     * @return Mutable application list owned by the process manager.
     */
    std::vector<ctx_t> &get_apps();
    /**
     * @brief Get app image.
     *
     * @param app_id App ID.
     * @return Validated image path for the requested application.
     */
    std::string get_app_image(int app_id);
    /**
     * @brief Get last run app name.
     *
     * @return Name of the most recently launched application.
     */
    std::string get_last_run_app_name();
    /**
     * @brief Get the UUID of the running app (Apollo).
     *
     * @return UUID of the running app, or an empty string.
     */
    std::string get_running_app_uuid();
    /**
     * @brief Get a copy of the environment apps are launched with (Apollo).
     *
     * @return Environment including the session's SUNSHINE_* and APOLLO_* variables.
     */
    boost::process::v1::environment get_env();
    /**
     * @brief Run the app's resume (state) commands when a client reconnects (Apollo).
     */
    void resume();
    /**
     * @brief Run the app's pause (state) commands when the last client disconnects (Apollo).
     */
    void pause();
    /**
     * @brief Terminate the launched application process.
     *
     * @param immediate Kill the process group without waiting for a graceful exit.
     * @param needs_refresh Reload the app list afterwards.
     */
    void terminate(bool immediate = false, bool needs_refresh = true);

    /**
     * @brief Update the cached app list and environment without disturbing running process state.
     *
     * This replaces only the application configuration data (_apps and _env) from a newly parsed
     * proc_t, preserving the current _app_id, _process, _process_group, and other fields that
     * track an in-flight streaming session.
     *
     * @param other The newly parsed proc_t whose apps and env will be moved into this instance.
     */
    void update_apps_and_env(proc_t &&other);


  private:
    int _app_id = 0;
    std::string _app_name;

    boost::process::v1::environment _env;

    std::shared_ptr<rtsp_stream::launch_session_t> _launch_session;
    std::shared_ptr<config::input_t> _saved_input_config;

    std::vector<ctx_t> _apps;
    ctx_t _app;
    std::chrono::steady_clock::time_point _app_launch_time;

    // If no command associated with _app_id, yet it's still running
    bool placebo {};

    boost::process::v1::child _process;
    boost::process::v1::group _process_group;

    file_t _pipe;
    std::vector<cmd_t>::const_iterator _app_prep_it;
    std::vector<cmd_t>::const_iterator _app_prep_begin;
  };

  boost::filesystem::path
  find_working_directory(const std::string &cmd, const boost::process::v1::environment &env);

  /**
   * @brief Calculate a stable id based on name and image data
   * @return Tuple of id calculated without index (for use if no collision) and one with.
   *
   * @param app_name App name.
   * @param app_image_path App image path.
   * @param index Zero-based index of the item being addressed.
   */
  std::tuple<std::string, std::string> calculate_app_id(const std::string &app_name, std::string app_image_path, int index);

  /**
   * @brief Identity of the virtual display created for a session.
   */
  struct vdisplay_identity_t {
    uuid_util::uuid_t uuid;  ///< Passed to platf::vdisplay::create() and remove().
    std::string uuid_str;  ///< Textual form handed to the backend (the configured UUID string where there is one).
    std::string name;  ///< Human-readable display name.
  };

  /**
   * @brief Derive the virtual display identity for an app launched by a client.
   *
   * - By default the display belongs to the client: client UUID and client name.
   * - With `use_app_identity` it belongs to the app: app UUID and app name.
   * - With `per_client_app_identity` as well, the UUID is the client UUID XOR the app UUID,
   *   so every client gets its own display per app.
   * @throws std::invalid_argument if a UUID involved is not a 36-character UUID string.
   */
  vdisplay_identity_t vdisplay_identity(const ctx_t &app, const rtsp_stream::launch_session_t &launch_session);

  /**
   * @brief Refresh rate in mHz for a session's virtual display.
   * @param client_fps The fps the client asked for, in mHz or (below 1000) in Hz; 0 means 60 Hz.
   * @param double_refreshrate Double the result (config option `double_refreshrate`).
   */
  int vdisplay_refresh_millihz(int client_fps, bool double_refreshrate);

  /**
   * @brief Create the virtual display for a session, if the platform backend is available.
   *
   * Re-initialises the backend when it is not ready. Sets `launch_session.display_guid`, and sets
   * `launch_session.virtual_display` to whether a display may now exist and must be removed on terminate.
   * @return The display name (for `proc_t::display_name`), or "" if no usable display was created (outside
   *         Windows also when the display could not be activated).
   */
  std::string create_virtual_display(const ctx_t &app, rtsp_stream::launch_session_t &launch_session, uint32_t render_width, uint32_t render_height);

  /**
   * @brief Whether a launch that needed a virtual display but got none may go on with the configured display.
   *
   * Windows keeps upstream's fallback to the physical display. Elsewhere that is only allowed when the platform has
   * no virtual display backend at all (`unavailable`, e.g. not running under Hyprland): a backend that exists but
   * failed, or a display that could not be set up, ends the launch instead of streaming the host's desktop.
   * @param status The backend status after create_virtual_display().
   * @param display_attempted `launch_session.virtual_display` after create_virtual_display().
   */
  bool may_stream_without_virtual_display(platf::vdisplay::status_e status, bool display_attempted);

  /**
   * @brief Prepare a configured command for execution.
   *
   * @param command Configured command line.
   * @return Command line with any package-specific launcher prefix applied.
   */
  std::string prepare_command(const std::string &command);

  bool check_valid_png(const std::filesystem::path &path);
  /**
   * @brief Validate app image path.
   *
   * @param app_image_path Candidate image path from the application configuration.
   * @return Existing PNG path, or the default application image when validation fails.
   */
  std::string validate_app_image_path(std::string app_image_path);
  /**
   * @brief Reload the application list when the apps file changed, and re-initialise the virtual display backend.
   *
   * @param file_name File name.
   * @param needs_terminate Terminate the running app first and reload unconditionally (Apollo).
   */
  void refresh(const std::string &file_name, bool needs_terminate = false);
  /**
   * @brief Merge a new or edited app into the apps file tree, giving every app a UUID (Apollo).
   *
   * @param fileTree_p Apps file contents, updated in place.
   * @param inputTree_p App to add (empty UUID) or replace (known UUID), or nullptr to only migrate.
   */
  void migrate_apps(nlohmann::json *fileTree_p, nlohmann::json *inputTree_p);
  /**
   * @brief Parse serialized text into the corresponding runtime representation.
   *
   * @param file_name File name.
   * @return Parsed value or parse status.
   */
  std::optional<proc::proc_t> parse(const std::string &file_name);

  /**
   * @brief The "elevated" flag of a prep-cmd or state-cmd entry of the app list.
   *
   * Accepts a boolean, 1 or 0, and "true"/"false" or "1"/"0" (older Web UIs saved strings; a hand-edited file may
   * hold any of them). Anything else is not elevated.
   *
   * @param command Command entry.
   * @return Whether the command runs elevated.
   */
  bool command_elevated(const nlohmann::json &command);

  /**
   * @brief Migrate a v1 app list to v2 (Apollo): legacy string values become booleans and integers, apps get UUIDs.
   *
   * @param fileTree The apps.json document, changed in place.
   */
  void migration_v2(nlohmann::json &fileTree);

  /**
   * @brief SUNSHINE_CLIENT_FPS in environment-variable compatibility mode: the rate as a whole number.
   *
   * @param fps_millihz Frame rate of the launch session in mHz (60 fps is 60000).
   * @return The rounded rate, e.g. "60".
   */
  std::string compatible_client_fps(int fps_millihz);

  /**
   * @brief Initialize proc functions
   * @return Unique pointer to `deinit_t` to manage cleanup
   */
  std::unique_ptr<platf::deinit_t> init();

  /**
   * @brief Terminates all child processes in a process group.
   * @param child_process The child process itself.
   * @param group The group of all children in the process tree.
   * @param exit_timeout The timeout to wait for the process group to gracefully exit.
   */
  void terminate_process_group(boost::process::v1::child &child_process, boost::process::v1::group &group, std::chrono::seconds exit_timeout);

  extern proc_t proc;

  extern int input_only_app_id;
  extern std::string input_only_app_id_str;
  extern int terminate_app_id;
  extern std::string terminate_app_id_str;
}  // namespace proc

#ifdef BOOST_PROCESS_VERSION
  #undef BOOST_PROCESS_VERSION
#endif
