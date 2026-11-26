/**
 * @file src/main.cpp
 * @brief Definitions for the main entry point for Sunshine.
 */
// standard includes
#include <atomic>
#include <cerrno>
#include <codecvt>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

#ifndef _WIN32
  #include <fcntl.h>
  #include <thread>
  #include <unistd.h>
#endif

// platform includes
#ifdef __APPLE__
  #include "platform/macos/misc.h"

  #include <mach-o/dyld.h>
#endif
#ifdef __linux__
  #include "platform/common.h"
  #include "platform/linux/graphics.h"
  #include "platform/linux/misc.h"

  #include <sys/auxv.h>
#endif

// lib includes
#include <rs.h>
#ifdef _WIN32
  #include <libvirtualhid/license.hpp>
#endif

// local includes
#include "confighttp.h"
#include "display_device.h"
#include "entry_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "main.h"
#include "nvhttp.h"
#include "platform/vdisplay.h"
#include "platform/permissions.h"
#include "process.h"
#include "system_tray.h"
#include "upnp.h"
#include "uuid.h"
#include "video.h"

#ifdef _WIN32
  #include "platform/windows/misc.h"
#endif

#define PROBE_DISPLAY_UUID "38F72B96-B00C-4F21-8B6C-E1BFF1602B0E"

using namespace std::literals;

std::map<int, std::function<void()>> signal_handlers;  ///< Signal handlers.

#ifdef _WIN32
// Windows runs signal handlers on a thread of their own, not on the thread that was interrupted.
void on_signal_forwarder(int sig) {
  signal_handlers.at(sig)();
}

void start_signal_thread() {}

void stop_signal_thread() {}
#else
namespace {
  // POSIX signal handlers interrupt whatever thread receives the signal, maybe while it holds a lock the shutdown
  // handlers need (logging, the running app, the tray). So the handler only writes the signal number to a pipe,
  // and signal_thread runs the registered handler. A 0 byte stops the thread.
  int signal_pipe[2] {-1, -1};
  std::thread signal_thread;

  void on_signal_forwarder(int sig) {
    const int saved_errno = errno;
    const auto byte = static_cast<unsigned char>(sig);
    [[maybe_unused]] auto written = write(signal_pipe[1], &byte, 1);
    errno = saved_errno;
  }

  void run_signal_handlers() {
    while (true) {
      unsigned char sig;
      auto n = read(signal_pipe[0], &sig, 1);
      if (n < 0 && errno == EINTR) {
        continue;
      }
      if (n != 1 || sig == 0) {
        return;
      }
      if (auto handler = signal_handlers.find(sig); handler != signal_handlers.end()) {
        handler->second();
      }
    }
  }

  bool open_signal_pipe() {
    if (signal_pipe[0] >= 0) {
      return true;
    }
    // The write end never blocks the signal handler; a full pipe already holds a pending shutdown.
  #ifdef __APPLE__
    // Darwin has no pipe2().
    if (pipe(signal_pipe) < 0) {
      return false;
    }
    const bool cloexec = fcntl(signal_pipe[0], F_SETFD, FD_CLOEXEC) == 0 && fcntl(signal_pipe[1], F_SETFD, FD_CLOEXEC) == 0;
  #else
    if (pipe2(signal_pipe, O_CLOEXEC) < 0) {
      return false;
    }
    constexpr bool cloexec = true;
  #endif
    if (!cloexec || fcntl(signal_pipe[1], F_SETFL, O_NONBLOCK) < 0) {
      close(signal_pipe[0]);
      close(signal_pipe[1]);
      signal_pipe[0] = signal_pipe[1] = -1;
      return false;
    }
    return true;
  }
}  // namespace

/**
 * @brief Starts running the handlers of signals received so far and from now on. Call it once every handler is
 *        registered: the thread reads signal_handlers without a lock.
 */
void start_signal_thread() {
  if (signal_pipe[0] >= 0 && !signal_thread.joinable()) {
    signal_thread = std::thread {run_signal_handlers};
  }
}

/**
 * @brief Restores the default action of the registered signals and stops the thread running their handlers, after
 *        any handler in progress has finished. The handlers refer to main()'s locals, so main() calls this first
 *        when it returns.
 */
void stop_signal_thread() {
  for (const auto &[sig, handler] : signal_handlers) {
    std::signal(sig, SIG_DFL);
  }
  if (!signal_thread.joinable()) {
    return;
  }
  // The pipe stays open: a handler started just before SIG_DFL took effect may still write to it.
  const unsigned char stop = 0;
  while (write(signal_pipe[1], &stop, 1) < 0 && (errno == EINTR || errno == EAGAIN)) {
    std::this_thread::yield();
  }
  signal_thread.join();
}
#endif

/**
 * @brief Register the handler invoked for a POSIX signal.
 *
 * @param sig Native signal number being handled.
 * @param fn Signal handler function to install.
 */
template<class FN>
void on_signal(int sig, FN &&fn) {
  signal_handlers.emplace(sig, std::forward<FN>(fn));

#ifndef _WIN32
  if (!open_signal_pipe()) {
    BOOST_LOG(error) << "Couldn't create the signal pipe: "sv << std::strerror(errno);
    return;
  }
#endif
  std::signal(sig, on_signal_forwarder);
}

/**
 * @brief Dispatch commands through a common signature, ignoring arguments each command does not need.
 */
std::map<std::string_view, std::function<int(const char *name, int argc, char **argv)>> cmd_to_func {
  {"creds"sv, [](const char *name, int argc, char **argv) {
     return args::creds(name, argc, argv);
   }},
  {"help"sv, [](const char *name, int, char **) {
     return args::help(name);
   }},
  {"version"sv, [](const char *, int, char **) {
     return args::version();
   }},
#ifdef _WIN32
  {"restore-nvprefs-undo"sv, [](const char *, int, char **) {
     return args::restore_nvprefs_undo();
   }},
#endif
};

#ifdef _WIN32
/**
 * @brief Handle Windows session-change messages for the monitor window.
 *
 * @param hwnd Window handle receiving the Windows control event.
 * @param uMsg U msg.
 * @param wParam W param.
 * @param lParam L param.
 * @return Process or platform callback exit code.
 */
LRESULT CALLBACK SessionMonitorWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  switch (uMsg) {
    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    case WM_ENDSESSION:
      {
        // Terminate ourselves with a blocking exit call
        std::cout << "Received WM_ENDSESSION"sv << std::endl;
        lifetime::exit_sunshine(0, false);
        return 0;
      }
    default:
      return DefWindowProc(hwnd, uMsg, wParam, lParam);
  }
}

/**
 * @brief Handle Windows console control events for shutdown.
 *
 * @param type Protocol, message, or resource type selector.
 * @return Process or platform callback exit code.
 */
WINAPI BOOL ConsoleCtrlHandler(DWORD type) {
  if (type == CTRL_CLOSE_EVENT) {
    BOOST_LOG(info) << "Console closed handler called";
    lifetime::exit_sunshine(0, false);
  }
  return FALSE;
}
#endif

#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
constexpr bool tray_is_enabled = true;  ///< Compile-time flag indicating tray support is enabled.
#else
constexpr bool tray_is_enabled = false;
#endif

/**
 * @brief Run the main event loop until Sunshine is asked to exit.
 *
 * @param shutdown_event Shutdown event.
 */
void mainThreadLoop(const std::shared_ptr<safe::event_t<bool>> &shutdown_event) {
  bool run_loop = false;

  // Conditions that would require the main thread event loop
#ifndef _WIN32
  run_loop = tray_is_enabled && config::sunshine.system_tray;  // On Windows, tray runs in separate thread, so no main loop needed for tray
#endif

  if (!run_loop) {
    BOOST_LOG(info) << "No main thread features enabled, skipping event loop"sv;
    // Wait for shutdown
    shutdown_event->view();
    return;
  }

  // Main thread event loop
  BOOST_LOG(info) << "Starting main loop"sv;
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
  system_tray::run_tray_until_exit(shutdown_event);
#endif
  BOOST_LOG(info) << "Main loop has exited"sv;
}

/**
 * @brief Run the main application or worker loop.
 *
 * @param argc The number of arguments.
 * @param argv The arguments.
 * @return Process or platform callback exit code.
 */
int main(int argc, char *argv[]) {
#ifdef __linux__
  const bool privileged_execution = getauxval(AT_SECURE) != 0 || platf::has_elevated_privileges(true);
  if (privileged_execution && !platf::sanitize_process_environment()) {
    std::cerr << "Failed to sanitize the environment for privileged execution: " << std::strerror(errno) << '\n';
    return 1;
  }
#endif

#ifdef __APPLE__
  // Bundle assets are referenced relative to the executable
  // (e.g. ../Resources/assets), so anchor cwd to Contents/MacOS.
  {
    char executable[2048];
    uint32_t size = sizeof(executable);
    if (_NSGetExecutablePath(executable, &size) == 0) {
      std::error_code ec;
      auto exec_dir = std::filesystem::weakly_canonical(std::filesystem::path {executable}, ec).parent_path();
      if (!ec) {
        std::filesystem::current_path(exec_dir, ec);
      }
      if (ec) {
        std::cerr << "Failed to set working directory to executable path: " << ec.message() << '\n';
      }
    }
  }
#endif
#if defined(__linux__)
  // On Linux, child threads inherit capabilities from the parent at creation time.
  // We ensure the privileged worker starts while capabilities are still held,
  // and then immediately strip global privileges from the main thread and future tasks.
  // Please ensure that these calls remain ordered as early as possible in startup initialization.
  #if defined(SUNSHINE_BUILD_DRM)
  platf::kms::ensure_privileged_drm_worker_started();
  #endif
  // Check and drop capabilities but use 'false' to retain CAP_SYS_NICE for EGL high priority contexts
  if (platf::has_elevated_privileges(false)) {
    platf::drop_elevated_privileges(false);
  }

  // Next, initialize privileged EGL worker thread
  egl::ensure_privileged_egl_worker_started();
  // Finally, check and drop all capabilities via 'true', which includes CAP_SYS_NICE.
  if (platf::has_elevated_privileges(true)) {
    platf::drop_elevated_privileges(true);
  }
#endif

  lifetime::argv = argv;

  task_pool_util::TaskPool::task_id_t force_shutdown = nullptr;

#ifdef _WIN32
  // Avoid searching the PATH in case a user has configured their system insecurely
  // by placing a user-writable directory in the system-wide PATH variable.
  SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);

  setlocale(LC_ALL, "C");
#endif

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  // Use UTF-8 conversion for the default C++ locale (used by boost::log)
  std::locale utf8_locale(std::locale(), new std::codecvt_utf8<wchar_t>);
  std::locale::global(utf8_locale);
  boost::filesystem::path::imbue(utf8_locale);
#pragma GCC diagnostic pop

  mail::man = std::make_shared<safe::mail_raw_t>();

  // parse config file
  if (config::parse(argc, argv)) {
    return 0;
  }

  auto log_deinit_guard = logging::init(config::sunshine.min_log_level, config::sunshine.log_file);
  if (!log_deinit_guard) {
    BOOST_LOG(error) << "Logging failed to initialize"sv;
  }

  // logging can begin at this point
  // if anything is logged prior to this point, it will appear in stdout, but not in the log viewer in the UI
  // the version should be printed to the log before anything else
  BOOST_LOG(info) << PROJECT_NAME << " version: " << PROJECT_VERSION << " commit: " << PROJECT_VERSION_COMMIT;

  // Log publisher metadata
  log_publisher_data();

  // Log modified_config_settings
  config::log_config_settings(config::modified_config_settings, false);
  config::modified_config_settings.clear();

  if (!config::sunshine.cmd.name.empty()) {
    auto fn = cmd_to_func.find(config::sunshine.cmd.name);
    if (fn == std::end(cmd_to_func)) {
      BOOST_LOG(fatal) << "Unknown command: "sv << config::sunshine.cmd.name;

      BOOST_LOG(info) << "Possible commands:"sv;
      for (auto &[key, _] : cmd_to_func) {
        BOOST_LOG(info) << '\t' << key;
      }

      return 7;
    }

    return fn->second(argv[0], config::sunshine.cmd.argc, config::sunshine.cmd.argv);
  }

#ifdef _WIN32
  config::select_all_gamepad_drivers_if_licensed(lvh::get_license_status().license.licensed());
#endif

  // Adding guard here first as it also performs recovery after crash,
  // otherwise people could theoretically end up without display output.
  // It also should be destroyed before forced shutdown to expedite the cleanup.
  auto display_device_deinit_guard = display_device::init(platf::appdata() / "display_device.state", config::video);
  if (!display_device_deinit_guard) {
    BOOST_LOG(error) << "Display device session failed to initialize"sv;
  }

#ifdef _WIN32
  // Modify relevant NVIDIA control panel settings if the system has corresponding gpu
  if (nvprefs_instance.load()) {
    // Restore global settings to the undo file left by improper termination of sunshine.exe
    nvprefs_instance.restore_from_and_delete_undo_file_if_exists();
    // Modify application settings for sunshine.exe
    nvprefs_instance.modify_application_profile();
    // Modify global settings, undo file is produced in the process to restore after improper termination
    nvprefs_instance.modify_global_profile();
    // Unload dynamic library to survive driver re-installation
    nvprefs_instance.unload();
  }

  // Wait as long as possible to terminate Sunshine.exe during logoff/shutdown
  SetProcessShutdownParameters(0x100, SHUTDOWN_NORETRY);

  // We must create a hidden window to receive shutdown notifications since we load gdi32.dll
  std::promise<HWND> session_monitor_hwnd_promise;
  auto session_monitor_hwnd_future = session_monitor_hwnd_promise.get_future();
  std::promise<void> session_monitor_join_thread_promise;
  auto session_monitor_join_thread_future = session_monitor_join_thread_promise.get_future();

  std::jthread session_monitor_thread([&]() {
    platf::set_thread_name("session_monitor");
    session_monitor_join_thread_promise.set_value_at_thread_exit();

    WNDCLASSA wnd_class {};
    wnd_class.lpszClassName = "SunshineSessionMonitorClass";
    wnd_class.lpfnWndProc = SessionMonitorWindowProc;
    if (!RegisterClassA(&wnd_class)) {
      session_monitor_hwnd_promise.set_value(nullptr);
      BOOST_LOG(error) << "Failed to register session monitor window class"sv << std::endl;
      return;
    }

    auto wnd = CreateWindowExA(
      0,
      wnd_class.lpszClassName,
      "Sunshine Session Monitor Window",
      0,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      nullptr,
      nullptr,
      nullptr,
      nullptr
    );

    session_monitor_hwnd_promise.set_value(wnd);

    if (!wnd) {
      BOOST_LOG(error) << "Failed to create session monitor window"sv << std::endl;
      return;
    }

    ShowWindow(wnd, SW_HIDE);

    // Run the message loop for our window
    MSG msg {};
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
  });

  auto session_monitor_join_thread_guard = util::fail_guard([&]() {
    if (session_monitor_hwnd_future.wait_for(1s) == std::future_status::ready) {
      if (HWND session_monitor_hwnd = session_monitor_hwnd_future.get()) {
        PostMessage(session_monitor_hwnd, WM_CLOSE, 0, 0);
      }

      if (session_monitor_join_thread_future.wait_for(1s) == std::future_status::ready) {
        session_monitor_thread.join();
        return;
      } else {
        BOOST_LOG(warning) << "session_monitor_join_thread_future reached timeout";
      }
    } else {
      BOOST_LOG(warning) << "session_monitor_hwnd_future reached timeout";
    }

    session_monitor_thread.detach();
  });

#endif

  task_pool.start(1);

  // Create signal handler after logging has been initialized
  auto shutdown_event = mail::man->event<bool>(mail::shutdown);
  // Returns a handler that starts the normal shutdown: optionally stop the running app (undo its prep commands and
  // remove its display) while everything is still initialised, then raise the shutdown event and arm a watchdog.
  // Only the first signal shuts down; later ones (a second Ctrl+C, SIGTERM after SIGHUP) are only logged.
  std::atomic_bool shutdown_started {false};
  auto make_shutdown_handler = [&force_shutdown, &display_device_deinit_guard, &shutdown_started, shutdown_event](std::string_view message, bool terminate_app) {
    return [&force_shutdown, &display_device_deinit_guard, &shutdown_started, shutdown_event, message, terminate_app]() {
      BOOST_LOG(info) << message;
      if (shutdown_started.exchange(true)) {
        BOOST_LOG(info) << "Shutdown already in progress"sv;
        return;
      }

      auto task = []() {
        BOOST_LOG(fatal) << "10 seconds passed, yet Sunshine's still running: Forcing shutdown"sv;
        logging::log_flush();
        lifetime::debug_trap();
      };

      if (terminate_app) {
        proc::proc.terminate();
      }

      force_shutdown = task_pool.pushDelayed(task, 10s).task_id;

      // Break out of the main loop
      shutdown_event->raise(true);

      if (tray_is_enabled && config::sunshine.system_tray) {
        system_tray::end_tray();
      }

      display_device_deinit_guard = nullptr;
    };
  };

  on_signal(SIGINT, make_shutdown_handler("Interrupt handler called"sv, true));

#ifdef _WIN32
  on_signal(SIGTERM, make_shutdown_handler("Terminate handler called"sv, false));
#else
  // systemd stops the user service with SIGTERM when the graphical session ends, and the session leader's
  // terminal or compositor going away delivers SIGHUP. Both must clean up like SIGINT so no app, virtual
  // display or input device is left behind.
  on_signal(SIGTERM, make_shutdown_handler("Terminate handler called"sv, true));
  on_signal(SIGHUP, make_shutdown_handler("Hangup handler called"sv, true));
#endif
  start_signal_thread();

#ifdef _WIN32
  // Terminate gracefully on Windows when console window is closed
  SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
#endif

  proc::refresh(config::stream.file_apps);

  // If any of the following fail, we log an error and continue event though sunshine will not function correctly.
  // This allows access to the UI to fix configuration problems or view the logs.

  auto platf_deinit_guard = platf::init();
  if (!platf_deinit_guard) {
    BOOST_LOG(error) << "Platform failed to initialize"sv;
  }

  // Capture the pre-request state so access granted during this launch causes
  // one clean restart after every verifiable required permission is available.
  const bool permission_restart_needed = !platf::required_permissions_granted(platf::get_permission_statuses());
#ifdef __APPLE__
  platf::request_startup_permissions(tray_is_enabled && config::sunshine.system_tray);
#endif

  auto proc_deinit_guard = proc::init();
  if (!proc_deinit_guard) {
    BOOST_LOG(error) << "Proc failed to initialize"sv;
  }

  reed_solomon_init();
  auto input_deinit_guard = input::init();

  if (config::input.controller && input::probe_gamepads()) {
    BOOST_LOG(warning) << "No gamepad input is available"sv;
  }

  if (video::probe_encoders()) {
    bool allow_probing = video::allow_encoder_probing();
    // Create a temporary virtual display for encoder capability probing
    if (platf::vdisplay::status() == platf::vdisplay::status_e::ok) {
      const std::string probe_uuid_str = PROBE_DISPLAY_UUID;
      const auto probe_uuid = uuid_util::uuid_t::parse(probe_uuid_str);
#ifdef _WIN32
      // SudoVDA has always been given 60 here, not 60000.
      constexpr int probe_refresh = 60;
#else
      constexpr int probe_refresh = 60000;
#endif

      BOOST_LOG(info) << "Creating a temporary virtual display to probe for encoders..."sv;

      if (!config::video.adapter_name.empty()) {
        platf::vdisplay::set_render_adapter(config::video.adapter_name);
      }

      [[maybe_unused]] const auto probe_display = platf::vdisplay::create(probe_uuid, "Probe", {800, 600, probe_refresh}, probe_uuid_str);

#ifndef _WIN32
      // Capture picks the output by name: probe on the display just created, not on a configured output that
      // may be disabled, switched off or missing (which may be why probing failed).
      const auto saved_output_name = config::video.output_name;
      if (!probe_display.empty()) {
        config::video.output_name = platf::vdisplay::output_name_for(probe_display);
      }
#endif

      std::this_thread::sleep_for(500ms);

      // Probe again anyways
      if (video::probe_encoders()) {
        if (allow_probing) {
          BOOST_LOG(error) << "Video failed to find working encoder: allow probing but failed"sv;
        } else {
          BOOST_LOG(error) << "Video failed to find working encoder even after attempted with a virtual display"sv;
        }
      }

#ifndef _WIN32
      config::video.output_name = saved_output_name;
#endif

      platf::vdisplay::remove(probe_uuid);
    } else if (!allow_probing) {
      BOOST_LOG(error) << "Video failed to find working encoder: probe failed and virtual display driver isn't initialized"sv;
    }
#ifndef _WIN32
    else {
      BOOST_LOG(error) << "Video failed to find working encoder: probing failed."sv;
    }
#endif
  }

  // Declared after every deinit guard above and the locals the handlers refer to, so on any return the signal
  // thread is stopped (and a handler in progress has finished) before they are torn down. Signals arriving after
  // that get their default action.
  auto signal_thread_guard = util::fail_guard(stop_signal_thread);

  if (http::init()) {
    BOOST_LOG(fatal) << "HTTP interface failed to initialize"sv;

#ifdef _WIN32
    BOOST_LOG(fatal) << "To relaunch Apollo successfully, use the shortcut in the Start Menu. Do not run sunshine.exe manually."sv;
    std::this_thread::sleep_for(10s);
#endif

    return -1;
  }

  std::unique_ptr<platf::deinit_t> mDNS;
  auto sync_mDNS = std::async(std::launch::async, [&mDNS]() {
    if (config::sunshine.enable_discovery) {
      mDNS = platf::publish::start();
    }
  });

  std::unique_ptr<platf::deinit_t> upnp_unmap;
  auto sync_upnp = std::async(std::launch::async, [&upnp_unmap]() {
    upnp_unmap = upnp::start();
  });

  // FIXME: Temporary workaround: Simple-Web_server needs to be updated or replaced
  if (shutdown_event->peek()) {
    return lifetime::desired_exit_code;
  }

  std::jthread httpThread {nvhttp::start};
  std::jthread configThread {confighttp::start};
  std::jthread rtspThread {rtsp_stream::start};

#ifdef _WIN32
  // If we're using the default port and GameStream is enabled, warn the user
  if (config::sunshine.port == 47989 && is_gamestream_enabled()) {
    BOOST_LOG(fatal) << "GameStream is still enabled in GeForce Experience! This *will* cause streaming problems with Apollo!"sv;
    BOOST_LOG(fatal) << "Disable GameStream on the SHIELD tab in GeForce Experience or change the Port setting on the Advanced tab in the Apollo Web UI."sv;
  }
#endif

  if (tray_is_enabled && config::sunshine.system_tray) {
    BOOST_LOG(info) << "Starting system tray"sv;
#if defined(_WIN32) || defined(__APPLE__)
    system_tray::prepare_tray_virtualhid_license();
    system_tray::prepare_tray_virtualhid_driver();
#endif
#ifdef _WIN32
    // TODO: Windows has a weird bug where when running as a service and on the first Windows boot,
    // the tray icon would not appear even though Sunshine is running correctly otherwise.
    // Restarting the service would allow the icon to appear normally.
    // For now we will keep the Windows tray icon on a separate thread.
    // Ideally, we would run the system tray on the main thread for all platforms.
    system_tray::init_tray_threaded();
#else
    system_tray::init_tray();
#endif
  }

#ifdef __APPLE__
  std::jthread macos_audio_permission_requester;
  if (!permission_restart_needed) {
    macos_audio_permission_requester = std::jthread([]() {
      platf::request_startup_system_audio_permission();
    });
  }
#endif

  std::jthread permission_watcher;
  if (permission_restart_needed) {
    permission_watcher = std::jthread([](std::stop_token stop) {
      while (!stop.stop_requested()) {
        if (platf::required_permissions_granted(platf::get_permission_statuses())) {
          BOOST_LOG(info) << "Required permissions granted; restarting Sunshine"sv;
          platf::restart();
          return;
        }
        std::this_thread::sleep_for(2s);
      }
    });
  }

  mainThreadLoop(shutdown_event);

  permission_watcher.request_stop();
  if (permission_watcher.joinable()) {
    permission_watcher.join();
  }
#ifdef __APPLE__
  if (macos_audio_permission_requester.joinable()) {
    macos_audio_permission_requester.join();
  }
#endif

  // The handler that raised the shutdown event may still be releasing the display device: let it finish before
  // tearing anything down.
  signal_thread_guard.disable();
  stop_signal_thread();

  httpThread.join();
  configThread.join();
  rtspThread.join();

  task_pool.stop();
  task_pool.join();

#ifdef _WIN32
  // Restore global NVIDIA control panel settings
  if (nvprefs_instance.owning_undo_file() && nvprefs_instance.load()) {
    nvprefs_instance.restore_global_profile();
    nvprefs_instance.unload();
  }
#endif

  return lifetime::desired_exit_code;
}
