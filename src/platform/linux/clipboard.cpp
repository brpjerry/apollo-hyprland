/**
 * @file src/platform/linux/clipboard.cpp
 * @brief Wayland clipboard through ext-data-control-v1, or zwlr_data_control_manager_v1 as a fallback.
 */
// standard includes
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <functional>
#include <future>
#include <list>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

// platform includes
#include <ext-data-control-v1.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wlr-data-control-unstable-v1.h>

// local includes
#include "clipboard.h"
#include "clipboard_logic.h"
#include "src/logging.h"

using namespace std::literals;

namespace platf::clipboard {
  namespace {
    using steady_clock = std::chrono::steady_clock;

    /**
     * @brief Called with the text of one of our sources and the descriptor a paste target reads from.
     */
    using send_fn_t = std::function<void(std::shared_ptr<const std::string>, int)>;

    /**
     * @brief The part of the clipboard that differs between ext-data-control and wlr-data-control.
     * @details Everything runs on the event thread.
     */
    class backend_t {
    public:
      virtual ~backend_t() = default;
      /// False once the compositor sent `finished` for the data device.
      virtual bool ok() const = 0;
      /// MIME types of the current selection, or nullptr when the selection is empty.
      virtual const std::vector<std::string> *selection_mimes() const = 0;
      /// Asks the selection owner to write the selection in @p mime to @p fd.
      virtual void receive(const std::string &mime, int fd) = 0;
      /// Creates a source that serves @p text and makes it the selection.
      virtual void set_selection(std::shared_ptr<const std::string> text) = 0;
      /// Sources not yet cancelled.
      virtual std::size_t active_sources() const = 0;
    };

    struct ext_traits_t {
      using manager_t = ext_data_control_manager_v1;
      using device_t = ext_data_control_device_v1;
      using source_t = ext_data_control_source_v1;
      using offer_t = ext_data_control_offer_v1;
      using device_listener_t = ext_data_control_device_v1_listener;
      using source_listener_t = ext_data_control_source_v1_listener;
      using offer_listener_t = ext_data_control_offer_v1_listener;
      static constexpr std::string_view name = "ext-data-control-v1"sv;

      static device_t *get_data_device(manager_t *m, wl_seat *seat) {
        return ext_data_control_manager_v1_get_data_device(m, seat);
      }

      static source_t *create_data_source(manager_t *m) {
        return ext_data_control_manager_v1_create_data_source(m);
      }

      static void destroy(manager_t *m) {
        ext_data_control_manager_v1_destroy(m);
      }

      static void add_listener(device_t *d, const device_listener_t *l, void *data) {
        ext_data_control_device_v1_add_listener(d, l, data);
      }

      static void set_selection(device_t *d, source_t *s) {
        ext_data_control_device_v1_set_selection(d, s);
      }

      static void destroy(device_t *d) {
        ext_data_control_device_v1_destroy(d);
      }

      static void add_listener(source_t *s, const source_listener_t *l, void *data) {
        ext_data_control_source_v1_add_listener(s, l, data);
      }

      static void offer(source_t *s, const char *mime) {
        ext_data_control_source_v1_offer(s, mime);
      }

      static void destroy(source_t *s) {
        ext_data_control_source_v1_destroy(s);
      }

      static void add_listener(offer_t *o, const offer_listener_t *l, void *data) {
        ext_data_control_offer_v1_add_listener(o, l, data);
      }

      static void receive(offer_t *o, const char *mime, int fd) {
        ext_data_control_offer_v1_receive(o, mime, fd);
      }

      static void destroy(offer_t *o) {
        ext_data_control_offer_v1_destroy(o);
      }
    };

    struct wlr_traits_t {
      using manager_t = zwlr_data_control_manager_v1;
      using device_t = zwlr_data_control_device_v1;
      using source_t = zwlr_data_control_source_v1;
      using offer_t = zwlr_data_control_offer_v1;
      using device_listener_t = zwlr_data_control_device_v1_listener;
      using source_listener_t = zwlr_data_control_source_v1_listener;
      using offer_listener_t = zwlr_data_control_offer_v1_listener;
      static constexpr std::string_view name = "wlr-data-control-unstable-v1"sv;

      static device_t *get_data_device(manager_t *m, wl_seat *seat) {
        return zwlr_data_control_manager_v1_get_data_device(m, seat);
      }

      static source_t *create_data_source(manager_t *m) {
        return zwlr_data_control_manager_v1_create_data_source(m);
      }

      static void destroy(manager_t *m) {
        // The destroy request exists since version 2.
        if (zwlr_data_control_manager_v1_get_version(m) >= ZWLR_DATA_CONTROL_MANAGER_V1_DESTROY_SINCE_VERSION) {
          zwlr_data_control_manager_v1_destroy(m);
        } else {
          wl_proxy_destroy(reinterpret_cast<wl_proxy *>(m));
        }
      }

      static void add_listener(device_t *d, const device_listener_t *l, void *data) {
        zwlr_data_control_device_v1_add_listener(d, l, data);
      }

      static void set_selection(device_t *d, source_t *s) {
        zwlr_data_control_device_v1_set_selection(d, s);
      }

      static void destroy(device_t *d) {
        zwlr_data_control_device_v1_destroy(d);
      }

      static void add_listener(source_t *s, const source_listener_t *l, void *data) {
        zwlr_data_control_source_v1_add_listener(s, l, data);
      }

      static void offer(source_t *s, const char *mime) {
        zwlr_data_control_source_v1_offer(s, mime);
      }

      static void destroy(source_t *s) {
        zwlr_data_control_source_v1_destroy(s);
      }

      static void add_listener(offer_t *o, const offer_listener_t *l, void *data) {
        zwlr_data_control_offer_v1_add_listener(o, l, data);
      }

      static void receive(offer_t *o, const char *mime, int fd) {
        zwlr_data_control_offer_v1_receive(o, mime, fd);
      }

      static void destroy(offer_t *o) {
        zwlr_data_control_offer_v1_destroy(o);
      }
    };

    /**
     * @brief A data-control device and the offers and sources it owns. Both protocols have the same shape.
     */
    template<class T>
    class data_control_t final: public backend_t {
      using manager_t = typename T::manager_t;
      using device_t = typename T::device_t;
      using source_t = typename T::source_t;
      using offer_t = typename T::offer_t;

    public:
      data_control_t(manager_t *manager, wl_seat *seat, send_fn_t send):
          manager {manager},
          send {std::move(send)} {
        device = T::get_data_device(manager, seat);
        T::add_listener(device, &device_listener, this);
      }

      ~data_control_t() override {
        for (auto &[offer, state] : offers) {
          T::destroy(offer);
        }
        for (auto &source : sources) {
          T::destroy(source->source);
        }
        if (device) {
          T::destroy(device);
        }
        T::destroy(manager);
      }

      bool ok() const override {
        return device != nullptr;
      }

      const std::vector<std::string> *selection_mimes() const override {
        if (!selection) {
          return nullptr;
        }
        auto it = offers.find(selection);
        return it == offers.end() ? nullptr : &it->second->mimes;
      }

      void receive(const std::string &mime, int fd) override {
        if (selection) {
          T::receive(selection, mime.c_str(), fd);
        }
      }

      void set_selection(std::shared_ptr<const std::string> text) override {
        if (!device) {
          return;
        }
        auto state = std::make_unique<source_state_t>();
        state->self = this;
        state->text = std::move(text);
        state->source = T::create_data_source(manager);
        T::add_listener(state->source, &source_listener, state.get());
        for (auto mime : offered_text_mimes) {
          T::offer(state->source, std::string {mime}.c_str());
        }
        T::set_selection(device, state->source);
        sources.emplace_back(std::move(state));
      }

      std::size_t active_sources() const override {
        return sources.size();
      }

    private:
      struct offer_state_t {
        std::vector<std::string> mimes;
      };

      struct source_state_t {
        data_control_t *self;
        source_t *source;
        std::shared_ptr<const std::string> text;
      };

      void drop_offer(offer_t *offer) {
        if (!offer || offer == selection || offer == primary) {
          return;
        }
        if (offers.erase(offer)) {
          T::destroy(offer);
        }
      }

      static void on_data_offer(void *data, device_t *, offer_t *offer) {
        auto self = static_cast<data_control_t *>(data);
        auto state = std::make_unique<offer_state_t>();
        T::add_listener(offer, &offer_listener, state.get());
        self->offers[offer] = std::move(state);
      }

      static void on_selection(void *data, device_t *, offer_t *offer) {
        auto self = static_cast<data_control_t *>(data);
        auto old = self->selection;
        self->selection = offer;
        self->drop_offer(old);
      }

      static void on_primary_selection(void *data, device_t *, offer_t *offer) {
        // Only the regular selection is synced; the primary selection's offers are just released.
        auto self = static_cast<data_control_t *>(data);
        auto old = self->primary;
        self->primary = offer;
        self->drop_offer(old);
      }

      static void on_finished(void *data, device_t *) {
        auto self = static_cast<data_control_t *>(data);
        BOOST_LOG(warning) << "Clipboard: the compositor invalidated the "sv << T::name << " device"sv;
        self->selection = nullptr;
        self->primary = nullptr;
        for (auto &[offer, state] : self->offers) {
          T::destroy(offer);
        }
        self->offers.clear();
        T::destroy(self->device);
        self->device = nullptr;
      }

      static void on_offer(void *data, offer_t *, const char *mime) {
        static_cast<offer_state_t *>(data)->mimes.emplace_back(mime);
      }

      static void on_send(void *data, source_t *, const char *mime, int32_t fd) {
        auto state = static_cast<source_state_t *>(data);
        BOOST_LOG(debug) << "Clipboard: serving "sv << state->text->size() << " bytes as "sv << mime;
        state->self->send(state->text, fd);
      }

      static void on_cancelled(void *data, source_t *source) {
        auto state = static_cast<source_state_t *>(data);
        auto self = state->self;
        BOOST_LOG(debug) << "Clipboard: our selection was replaced"sv;
        T::destroy(source);
        std::erase_if(self->sources, [state](const auto &s) {
          return s.get() == state;
        });
      }

      static constexpr typename T::device_listener_t device_listener {
        .data_offer = on_data_offer,
        .selection = on_selection,
        .finished = on_finished,
        .primary_selection = on_primary_selection,
      };
      static constexpr typename T::offer_listener_t offer_listener {
        .offer = on_offer,
      };
      static constexpr typename T::source_listener_t source_listener {
        .send = on_send,
        .cancelled = on_cancelled,
      };

      manager_t *manager;
      device_t *device = nullptr;
      send_fn_t send;
      std::map<offer_t *, std::unique_ptr<offer_state_t>> offers;
      offer_t *selection = nullptr;
      offer_t *primary = nullptr;
      std::vector<std::unique_ptr<source_state_t>> sources;
    };

    struct globals_t {
      uint32_t seat = 0;
      uint32_t ext = 0;
      uint32_t wlr = 0;
      uint32_t wlr_version = 0;
    };

    void on_global(void *data, wl_registry *, uint32_t name, const char *interface, uint32_t version) {
      auto globals = static_cast<globals_t *>(data);
      std::string_view iface {interface};
      if (iface == wl_seat_interface.name) {
        if (!globals->seat) {
          globals->seat = name;
        }
      } else if (iface == ext_data_control_manager_v1_interface.name) {
        globals->ext = name;
      } else if (iface == zwlr_data_control_manager_v1_interface.name) {
        globals->wlr = name;
        globals->wlr_version = version;
      }
    }

    void on_global_remove(void *, wl_registry *, uint32_t) {}

    constexpr wl_registry_listener registry_listener {
      .global = on_global,
      .global_remove = on_global_remove,
    };
  }  // namespace

  struct wayland_clipboard_t::impl_t {
    struct read_t {
      int fd;
      pipe_reader_t reader;
      steady_clock::time_point deadline;
      std::shared_ptr<std::promise<std::optional<std::string>>> result;
      std::size_t poll_index = SIZE_MAX;
    };

    struct write_t {
      int fd;
      pipe_writer_t writer;
      steady_clock::time_point deadline;
      std::size_t poll_index = SIZE_MAX;
    };

    struct sync_t {
      impl_t *self;
      wl_callback *callback;
      std::shared_ptr<std::promise<bool>> result;
    };

    explicit impl_t(const options_t &options):
        options {options} {}

    ~impl_t() {
      {
        std::lock_guard lock {task_mutex};
        stop = true;
        accepting = false;
      }
      wake();
      if (thread.joinable()) {
        thread.join();
      }
      fail_pending();
      backend.reset();
      if (seat) {
        wl_seat_destroy(seat);
      }
      if (registry) {
        wl_registry_destroy(registry);
      }
      if (display) {
        wl_display_disconnect(display);
      }
      if (wake_fd >= 0) {
        close(wake_fd);
      }
    }

    void wake() const {
      if (wake_fd >= 0) {
        uint64_t one = 1;
        [[maybe_unused]] auto n = write(wake_fd, &one, sizeof(one));
      }
    }

    /**
     * @brief Runs @p task on the event thread.
     * @return false if the event thread no longer runs.
     */
    bool post(std::function<void()> task) {
      {
        std::lock_guard lock {task_mutex};
        if (!accepting) {
          return false;
        }
        tasks.emplace_back(std::move(task));
      }
      wake();
      return true;
    }

    /// Completes everything still in flight with a failure and closes its descriptors.
    void fail_pending() {
      for (auto &r : reads) {
        close(r.fd);
        r.result->set_value(std::nullopt);
      }
      reads.clear();
      for (auto &w : writes) {
        close(w.fd);
      }
      writes.clear();
      for (auto &s : syncs) {
        wl_callback_destroy(s.callback);
        s.result->set_value(false);
      }
      syncs.clear();
    }

    void run() {
      while (true) {
        std::vector<std::function<void()>> pending;
        {
          std::lock_guard lock {task_mutex};
          if (stop) {
            return;
          }
          pending.swap(tasks);
        }
        for (auto &task : pending) {
          task();
        }
        if (!step()) {
          break;
        }
      }

      if (auto err = wl_display_get_error(display)) {
        BOOST_LOG(warning) << "Clipboard: lost the Wayland connection: "sv << std::strerror(err);
      } else {
        BOOST_LOG(warning) << "Clipboard: the data-control device is gone"sv;
      }
      broken = true;
      {
        std::lock_guard lock {task_mutex};
        accepting = false;
        tasks.clear();
      }
      fail_pending();
    }

    /// One round of the event loop: flush, wait for the display, the wake-up eventfd and the transfers, dispatch.
    bool step() {
      while (wl_display_prepare_read(display) != 0) {
        if (wl_display_dispatch_pending(display) < 0) {
          return false;
        }
      }

      short display_events = POLLIN;
      if (wl_display_flush(display) < 0) {
        if (errno != EAGAIN) {
          wl_display_cancel_read(display);
          return false;
        }
        display_events |= POLLOUT;
      }

      std::vector<pollfd> fds;
      fds.push_back({wl_display_get_fd(display), display_events, 0});
      fds.push_back({wake_fd, POLLIN, 0});
      std::optional<steady_clock::time_point> next_deadline;
      auto track = [&](auto &transfer, short events) {
        transfer.poll_index = fds.size();
        fds.push_back({transfer.fd, events, 0});
        if (!next_deadline || transfer.deadline < *next_deadline) {
          next_deadline = transfer.deadline;
        }
      };
      for (auto &r : reads) {
        track(r, POLLIN);
      }
      for (auto &w : writes) {
        track(w, POLLOUT);
      }

      int timeout = -1;
      if (next_deadline) {
        auto left = std::chrono::ceil<std::chrono::milliseconds>(*next_deadline - steady_clock::now());
        timeout = static_cast<int>(std::max<std::chrono::milliseconds::rep>(0, left.count()));
      }

      auto n = poll(fds.data(), fds.size(), timeout);
      if (n < 0 && errno != EINTR) {
        wl_display_cancel_read(display);
        return false;
      }
      if (n > 0 && (fds[0].revents & (POLLIN | POLLERR | POLLHUP))) {
        if (wl_display_read_events(display) < 0) {
          return false;
        }
      } else {
        wl_display_cancel_read(display);
      }
      if (wl_display_dispatch_pending(display) < 0 || !backend->ok()) {
        return false;
      }
      if (n > 0 && (fds[1].revents & POLLIN)) {
        uint64_t value;
        [[maybe_unused]] auto r = read(wake_fd, &value, sizeof(value));
      }

      auto ready = [&](const auto &transfer) {
        return n > 0 && transfer.poll_index < fds.size() && fds[transfer.poll_index].revents != 0;
      };
      auto now = steady_clock::now();
      for (auto it = reads.begin(); it != reads.end();) {
        auto status = ready(*it) ? it->reader.read_some(it->fd) : io_status_e::again;
        if (status == io_status_e::again && now < it->deadline) {
          ++it;
          continue;
        }
        close(it->fd);
        if (status == io_status_e::done) {
          BOOST_LOG(debug) << "Clipboard: read "sv << it->reader.data.size() << " bytes"sv;
          it->result->set_value(std::move(it->reader.data));
        } else {
          if (status == io_status_e::too_large) {
            BOOST_LOG(warning) << "Clipboard: the selection is larger than "sv << options.max_size << " bytes"sv;
          } else if (status == io_status_e::error) {
            BOOST_LOG(warning) << "Clipboard: reading the selection failed: "sv << std::strerror(errno);
          } else {
            BOOST_LOG(warning) << "Clipboard: the selection owner did not send its data within "sv
                               << options.read_timeout.count() << " ms"sv;
          }
          it->result->set_value(std::nullopt);
        }
        it = reads.erase(it);
      }
      for (auto it = writes.begin(); it != writes.end();) {
        auto status = ready(*it) ? it->writer.write_some(it->fd) : io_status_e::again;
        if (status == io_status_e::again && now < it->deadline) {
          ++it;
          continue;
        }
        if (status == io_status_e::error) {
          BOOST_LOG(debug) << "Clipboard: paste target closed its pipe early"sv;
        } else if (status == io_status_e::again) {
          BOOST_LOG(warning) << "Clipboard: paste target did not read the text within "sv
                             << options.write_timeout.count() << " ms"sv;
        }
        close(it->fd);
        it = writes.erase(it);
      }
      return true;
    }

    /// Event thread: serves @p text to a paste target.
    void start_write(std::shared_ptr<const std::string> text, int fd) {
      int flags = fcntl(fd, F_GETFL);
      if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return;
      }
      pipe_writer_t writer {std::move(text)};
      auto status = writer.write_some(fd);
      if (status != io_status_e::again) {
        close(fd);
        return;
      }
      writes.push_back({fd, std::move(writer), steady_clock::now() + options.write_timeout});
    }

    /// Event thread: starts reading the selection into @p result.
    void start_read(std::shared_ptr<std::promise<std::optional<std::string>>> result) {
      auto mimes = backend->selection_mimes();
      if (!mimes) {
        result->set_value(std::string {});
        return;
      }
      auto mime = choose_text_mime(*mimes);
      if (!mime) {
        BOOST_LOG(debug) << "Clipboard: the selection holds no text ("sv << mimes->size() << " types offered)"sv;
        result->set_value(std::string {});
        return;
      }
      int fds[2];
      if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) < 0) {
        BOOST_LOG(warning) << "Clipboard: pipe2 failed: "sv << std::strerror(errno);
        result->set_value(std::nullopt);
        return;
      }
      // libwayland duplicates the descriptor while marshalling, so ours can be closed right away.
      backend->receive(*mime, fds[1]);
      close(fds[1]);
      reads.push_back({fds[0], pipe_reader_t {options.max_size}, steady_clock::now() + options.read_timeout, std::move(result)});
    }

    /// Event thread: completes @p result once the compositor has handled every request sent so far.
    void start_sync(std::shared_ptr<std::promise<bool>> result) {
      static constexpr wl_callback_listener listener {
        .done = [](void *data, wl_callback *callback, uint32_t) {
          auto sync = static_cast<sync_t *>(data);
          auto self = sync->self;
          wl_callback_destroy(callback);
          sync->result->set_value(self->backend->ok());
          self->syncs.remove_if([sync](const sync_t &s) {
            return &s == sync;
          });
        },
      };
      auto &sync = syncs.emplace_back(sync_t {this, wl_display_sync(display), std::move(result)});
      wl_callback_add_listener(sync.callback, &listener, &sync);
    }

    options_t options;
    wl_display *display = nullptr;
    wl_registry *registry = nullptr;
    wl_seat *seat = nullptr;
    std::unique_ptr<backend_t> backend;
    std::string_view protocol_name;
    int wake_fd = -1;
    std::thread thread;
    std::atomic<bool> broken {false};

    std::mutex task_mutex;
    std::vector<std::function<void()>> tasks;
    bool accepting = true;
    bool stop = false;

    // Event thread only.
    std::list<read_t> reads;
    std::list<write_t> writes;
    std::list<sync_t> syncs;
  };

  std::unique_ptr<wayland_clipboard_t> wayland_clipboard_t::connect(const char *display_name, const options_t &options) {
    if (!display_name) {
      display_name = std::getenv("WAYLAND_DISPLAY");
    }
    if (!display_name) {
      BOOST_LOG(warning) << "Clipboard: WAYLAND_DISPLAY is not set"sv;
      return nullptr;
    }

    auto impl = std::make_unique<impl_t>(options);
    impl->display = wl_display_connect(display_name);
    if (!impl->display) {
      BOOST_LOG(warning) << "Clipboard: could not connect to Wayland display "sv << display_name;
      return nullptr;
    }

    globals_t globals;
    impl->registry = wl_display_get_registry(impl->display);
    wl_registry_add_listener(impl->registry, &registry_listener, &globals);
    if (wl_display_roundtrip(impl->display) < 0) {
      BOOST_LOG(warning) << "Clipboard: Wayland registry roundtrip failed"sv;
      return nullptr;
    }
    if (!globals.seat) {
      BOOST_LOG(warning) << "Clipboard: the compositor has no seat"sv;
      return nullptr;
    }
    bool use_ext = globals.ext && options.protocol != protocol_e::wlr;
    bool use_wlr = !use_ext && globals.wlr && options.protocol != protocol_e::ext;
    if (!use_ext && !use_wlr) {
      BOOST_LOG(warning) << "Clipboard: the compositor supports neither ext-data-control-v1 nor wlr-data-control"sv;
      return nullptr;
    }

    impl->seat = static_cast<wl_seat *>(wl_registry_bind(impl->registry, globals.seat, &wl_seat_interface, 1));
    auto raw = impl.get();
    send_fn_t send = [raw](std::shared_ptr<const std::string> text, int fd) {
      raw->start_write(std::move(text), fd);
    };
    if (use_ext) {
      auto manager = static_cast<ext_data_control_manager_v1 *>(
        wl_registry_bind(impl->registry, globals.ext, &ext_data_control_manager_v1_interface, 1)
      );
      impl->backend = std::make_unique<data_control_t<ext_traits_t>>(manager, impl->seat, std::move(send));
      impl->protocol_name = ext_traits_t::name;
    } else {
      auto manager = static_cast<zwlr_data_control_manager_v1 *>(
        wl_registry_bind(impl->registry, globals.wlr, &zwlr_data_control_manager_v1_interface, std::min(globals.wlr_version, 2u))
      );
      impl->backend = std::make_unique<data_control_t<wlr_traits_t>>(manager, impl->seat, std::move(send));
      impl->protocol_name = wlr_traits_t::name;
    }

    // Receive the current selection before the first get().
    if (wl_display_roundtrip(impl->display) < 0 || !impl->backend->ok()) {
      BOOST_LOG(warning) << "Clipboard: setting up the "sv << impl->protocol_name << " device failed"sv;
      return nullptr;
    }

    impl->wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (impl->wake_fd < 0) {
      BOOST_LOG(warning) << "Clipboard: eventfd failed: "sv << std::strerror(errno);
      return nullptr;
    }
    impl->thread = std::thread {&impl_t::run, raw};

    BOOST_LOG(info) << "Clipboard: using "sv << impl->protocol_name << " on "sv << display_name;
    return std::unique_ptr<wayland_clipboard_t>(new wayland_clipboard_t(std::move(impl)));
  }

  wayland_clipboard_t::wayland_clipboard_t(std::unique_ptr<impl_t> impl):
      impl {std::move(impl)} {}

  wayland_clipboard_t::~wayland_clipboard_t() = default;

  std::optional<std::string> wayland_clipboard_t::get() {
    auto result = std::make_shared<std::promise<std::optional<std::string>>>();
    auto future = result->get_future();
    if (!impl->post([this, result]() mutable {
          impl->start_read(std::move(result));
        })) {
      return std::nullopt;
    }
    // The event thread enforces read_timeout; the margin only covers a stuck event thread.
    if (future.wait_for(impl->options.read_timeout + 2s) != std::future_status::ready) {
      BOOST_LOG(warning) << "Clipboard: no answer from the clipboard thread"sv;
      return std::nullopt;
    }
    try {
      return future.get();
    } catch (const std::future_error &) {
      return std::nullopt;
    }
  }

  bool wayland_clipboard_t::set(std::string_view text) {
    auto content = std::make_shared<const std::string>(crlf_to_lf(text));
    auto result = std::make_shared<std::promise<bool>>();
    auto future = result->get_future();
    if (!impl->post([this, content, result]() mutable {
          impl->backend->set_selection(content);
          impl->start_sync(std::move(result));
        })) {
      return false;
    }
    if (future.wait_for(impl->options.read_timeout + 2s) != std::future_status::ready) {
      BOOST_LOG(warning) << "Clipboard: no answer from the clipboard thread"sv;
      return false;
    }
    try {
      return future.get();
    } catch (const std::future_error &) {
      return false;
    }
  }

  bool wayland_clipboard_t::alive() const {
    return !impl->broken;
  }

  std::string_view wayland_clipboard_t::protocol() const {
    return impl->protocol_name;
  }

  std::size_t wayland_clipboard_t::active_sources() {
    auto result = std::make_shared<std::promise<std::size_t>>();
    auto future = result->get_future();
    if (!impl->post([this, result]() {
          result->set_value(impl->backend->active_sources());
        })) {
      return 0;
    }
    if (future.wait_for(5s) != std::future_status::ready) {
      return 0;
    }
    try {
      return future.get();
    } catch (const std::future_error &) {
      return 0;
    }
  }
}  // namespace platf::clipboard
