/**
 * @file src/process.h
 * @brief Declarations for the startup and shutdown of the apps started by a streaming Session.
 */
#pragma once

#ifndef __kernel_entry
  #define __kernel_entry
#endif

#ifndef BOOST_PROCESS_VERSION
  #define BOOST_PROCESS_VERSION 1
#endif

// standard includes
#include <optional>
#include <unordered_map>

// lib includes
#include <boost/process/v1/child.hpp>
#include <boost/process/v1/group.hpp>
#include <boost/process/v1/environment.hpp>
#include <boost/process/v1/search_path.hpp>
#include <boost/property_tree/ptree.hpp>
#include <nlohmann/json.hpp>

// local includes
#include "config.h"
#include "platform/common.h"
#include "platform/vdisplay.h"
#include "rtsp.h"
#include "utility.h"
#include "uuid.h"

#define VIRTUAL_DISPLAY_UUID "8902CB19-674A-403D-A587-41B092E900BA"
#define FALLBACK_DESKTOP_UUID "EAAC6159-089A-46A9-9E24-6436885F6610"
#define REMOTE_INPUT_UUID "8CB5C136-DA67-4F99-B4A1-F9CD35005CF4"
#define TERMINATE_APP_UUID "E16CBE1B-295D-4632-9A76-EC4180C857D3"

namespace proc {
  using file_t = util::safe_ptr_v2<FILE, int, fclose>;

  typedef config::prep_cmd_t cmd_t;

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
    std::vector<cmd_t> prep_cmds;
    std::vector<cmd_t> state_cmds;

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

    std::string idx;
    std::string uuid;
    std::string name;
    std::string cmd;
    std::string working_dir;
    std::string output;
    std::string image_path;
    std::string id;
    std::string gamepad;
    bool elevated;
    bool auto_detach;
    bool wait_all;
    bool virtual_display;
    bool use_app_identity;
    bool per_client_app_identity;
    bool allow_client_commands;
    bool terminate_on_pause;
    int  scale_factor;
    std::chrono::seconds exit_timeout;
  };

  class proc_t {
  public:
    KITTY_DEFAULT_CONSTR_MOVE_THROW(proc_t)

    std::string display_name;
    std::string initial_display;
    std::string mode_changed_display;
    bool initial_hdr = false;
    bool virtual_display = false;
    bool allow_client_commands = false;

    proc_t(
      boost::process::v1::environment &&env,
      std::vector<ctx_t> &&apps
    ):
        _env(std::move(env)),
        _apps(std::move(apps)) {
    }

    void launch_input_only();

    int execute(const ctx_t& _app, std::shared_ptr<rtsp_stream::launch_session_t> launch_session);

    /**
     * @return `_app_id` if a process is running, otherwise returns `0`
     */
    int running();

    ~proc_t();

    const std::vector<ctx_t> &get_apps() const;
    std::vector<ctx_t> &get_apps();
    std::string get_app_image(int app_id);
    std::string get_last_run_app_name();
    std::string get_running_app_uuid();
    boost::process::v1::environment get_env();
    void resume();
    void pause();
    void terminate(bool immediate = false, bool needs_refresh = true);

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

  std::string validate_app_image_path(std::string app_image_path);
  void refresh(const std::string &file_name, bool needs_terminate = true);
  void migrate_apps(nlohmann::json* fileTree_p, nlohmann::json* inputTree_p);
  std::optional<proc::proc_t> parse(const std::string &file_name);

  /**
   * @brief Initialize proc functions
   * @return Unique pointer to `deinit_t` to manage cleanup
   */
  std::unique_ptr<platf::deinit_t> init();

  /**
   * @brief Terminates all child processes in a process group.
   * @param proc The child process itself.
   * @param group The group of all children in the process tree.
   * @param exit_timeout The timeout to wait for the process group to gracefully exit.
   */
  void terminate_process_group(boost::process::v1::child &proc, boost::process::v1::group &group, std::chrono::seconds exit_timeout);

  extern proc_t proc;

  extern int input_only_app_id;
  extern std::string input_only_app_id_str;
  extern int terminate_app_id;
  extern std::string terminate_app_id_str;
}  // namespace proc

#ifdef BOOST_PROCESS_VERSION
  #undef BOOST_PROCESS_VERSION
#endif
