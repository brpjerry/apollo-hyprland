/**
 * @file tests/unit/platform/hyprland_fakes.h
 * @brief Fake Hyprland sockets for unit tests of the code in src/platform/linux/hyprland.
 */
#pragma once

#ifdef SUNSHINE_BUILD_WAYLAND

  // standard includes
  #include <atomic>
  #include <chrono>
  #include <condition_variable>
  #include <cstdlib>
  #include <cstring>
  #include <filesystem>
  #include <functional>
  #include <mutex>
  #include <stdexcept>
  #include <string>
  #include <string_view>
  #include <thread>
  #include <vector>

  // platform includes
  #include <poll.h>
  #include <sys/socket.h>
  #include <sys/un.h>
  #include <unistd.h>

namespace hyprland_fakes {
  namespace fs = std::filesystem;
  using namespace std::chrono_literals;

  /**
   * @brief A temporary directory, removed on destruction. Kept short so socket paths fit in sun_path.
   */
  class temp_dir_t {
  public:
    temp_dir_t() {
      auto base = fs::temp_directory_path();
      if (base.native().size() > 40) {
        base = "/tmp";
      }
      auto tmpl = (base / "ahyp-XXXXXX").string();
      if (::mkdtemp(tmpl.data()) == nullptr) {
        throw std::runtime_error("mkdtemp failed");
      }
      path_ = tmpl;
    }

    ~temp_dir_t() {
      std::error_code ec;
      fs::remove_all(path_, ec);
    }

    const fs::path &path() const {
      return path_;
    }

  private:
    fs::path path_;
  };

  inline int bind_unix(const fs::path &path) {
    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || ::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0 || ::listen(fd, 16) < 0) {
      throw std::runtime_error("cannot bind " + path.string());
    }
    return fd;
  }

  /**
   * @brief Leave a socket file nobody listens on, like a crashed Hyprland does.
   */
  inline void make_dead_socket(const fs::path &path) {
    ::close(bind_unix(path));
  }

  /**
   * @brief Serves a fake `.socket.sock` from a thread: reads one request per connection, replies, closes.
   */
  class fake_request_socket_t {
  public:
    using handler_t = std::function<std::string(const std::string &)>;

    fake_request_socket_t(const fs::path &path, handler_t handler):
        handler_ {std::move(handler)},
        listen_fd_ {bind_unix(path)} {
      thread_ = std::thread {[this]() {
        serve();
      }};
    }

    ~fake_request_socket_t() {
      stopping_ = true;
      ::shutdown(listen_fd_, SHUT_RDWR);
      thread_.join();
      ::close(listen_fd_);
    }

    std::vector<std::string> requests() {
      std::lock_guard lock {mutex_};
      return requests_;
    }

  private:
    void serve() {
      while (!stopping_) {
        int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0) {
          return;
        }
        std::string request;
        char buf[4096];
        // Like Hyprland: take whatever has arrived once the socket goes quiet.
        pollfd pfd {fd, POLLIN, 0};
        while (::poll(&pfd, 1, request.empty() ? 1000 : 20) > 0) {
          auto n = ::recv(fd, buf, sizeof(buf), 0);
          if (n <= 0) {
            break;
          }
          request.append(buf, static_cast<std::size_t>(n));
        }
        {
          std::lock_guard lock {mutex_};
          requests_.push_back(request);
        }
        auto reply = handler_(request);
        ::send(fd, reply.data(), reply.size(), MSG_NOSIGNAL);
        ::close(fd);
      }
    }

    handler_t handler_;
    int listen_fd_;
    std::atomic<bool> stopping_ {false};
    std::thread thread_;
    std::mutex mutex_;
    std::vector<std::string> requests_;
  };

  /**
   * @brief Serves a fake `.socket2.sock`: the test pushes raw bytes to the current client or drops it.
   */
  class fake_event_socket_t {
  public:
    explicit fake_event_socket_t(const fs::path &path):
        listen_fd_ {bind_unix(path)} {
      thread_ = std::thread {[this]() {
        while (true) {
          int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
          if (fd < 0) {
            return;
          }
          std::lock_guard lock {mutex_};
          if (client_ >= 0) {
            ::close(client_);
          }
          client_ = fd;
          ++connections_;
          cv_.notify_all();
        }
      }};
    }

    ~fake_event_socket_t() {
      ::shutdown(listen_fd_, SHUT_RDWR);
      thread_.join();
      ::close(listen_fd_);
      if (client_ >= 0) {
        ::close(client_);
      }
    }

    bool wait_for_connections(int count) {
      std::unique_lock lock {mutex_};
      return cv_.wait_for(lock, 5s, [&]() {
        return connections_ >= count;
      });
    }

    void send(std::string_view bytes) {
      std::lock_guard lock {mutex_};
      ::send(client_, bytes.data(), bytes.size(), MSG_NOSIGNAL);
    }

    void drop_client() {
      std::lock_guard lock {mutex_};
      ::close(client_);
      client_ = -1;
    }

  private:
    int listen_fd_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    int client_ = -1;
    int connections_ = 0;
  };
}  // namespace hyprland_fakes

#endif
