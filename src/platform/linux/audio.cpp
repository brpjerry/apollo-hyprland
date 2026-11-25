/**
 * @file src/platform/linux/audio.cpp
 * @brief Definitions for audio control on Linux.
 */
// standard includes
#include <bitset>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <sstream>
#include <thread>

// lib includes
#include <pulse/error.h>
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>

// local includes
#include "audio_sinks.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/utility.h"

namespace platf {
  using namespace std::literals;

  constexpr pa_channel_position_t position_mapping[] {
    PA_CHANNEL_POSITION_FRONT_LEFT,
    PA_CHANNEL_POSITION_FRONT_RIGHT,
    PA_CHANNEL_POSITION_FRONT_CENTER,
    PA_CHANNEL_POSITION_LFE,
    PA_CHANNEL_POSITION_REAR_LEFT,
    PA_CHANNEL_POSITION_REAR_RIGHT,
    PA_CHANNEL_POSITION_SIDE_LEFT,
    PA_CHANNEL_POSITION_SIDE_RIGHT,
  };

  std::string to_string(const char *name, const std::uint8_t *mapping, int channels) {
    std::stringstream ss;

    ss << "rate=48000 sink_name="sv << name << " format=float channels="sv << channels << " channel_map="sv;
    std::for_each_n(mapping, channels - 1, [&ss](std::uint8_t pos) {
      ss << pa_channel_position_to_string(position_mapping[pos]) << ',';
    });

    ss << pa_channel_position_to_string(position_mapping[mapping[channels - 1]]);

    ss << " sink_properties=device.description="sv << name;
    auto result = ss.str();

    BOOST_LOG(debug) << "null-sink args: "sv << result;
    return result;
  }

  struct mic_attr_t: public mic_t {
    util::safe_ptr<pa_simple, pa_simple_free> mic;

    /** Bumped by the sink policy when capture should follow a new default sink; null to never reinitialise. */
    std::shared_ptr<const std::atomic<std::uint64_t>> generation;
    std::uint64_t start_generation = 0;

    bool generation_changed() const {
      return generation && generation->load() != start_generation;
    }

    capture_e sample(std::vector<float> &sample_buf) override {
      if (generation_changed()) {
        return capture_e::reinit;
      }

      auto sample_size = sample_buf.size();

      auto buf = sample_buf.data();
      int status;
      if (pa_simple_read(mic.get(), buf, sample_size * sizeof(float), &status)) {
        // The captured sink may have been removed. The server then picks a new default, and the event for that can
        // arrive just after the read failed: give it a moment, and reinitialise onto the new default if it comes.
        for (int x = 0; generation && x < 10; ++x) {
          if (generation_changed()) {
            BOOST_LOG(info) << "Audio capture stopped ("sv << pa_strerror(status) << ") after the default sink changed"sv;
            return capture_e::reinit;
          }
          std::this_thread::sleep_for(100ms);
        }
        BOOST_LOG(error) << "pa_simple_read() failed: "sv << pa_strerror(status);

        return capture_e::error;
      }

      return capture_e::ok;
    }
  };

  std::unique_ptr<mic_attr_t> microphone(const std::uint8_t *mapping, int channels, std::uint32_t sample_rate, std::uint32_t frame_size, std::string source_name) {
    auto mic = std::make_unique<mic_attr_t>();

    pa_sample_spec ss {PA_SAMPLE_FLOAT32, sample_rate, (std::uint8_t) channels};
    pa_channel_map pa_map;

    pa_map.channels = channels;
    std::for_each_n(pa_map.map, pa_map.channels, [mapping](auto &channel) mutable {
      channel = position_mapping[*mapping++];
    });

    pa_buffer_attr pa_attr = {
      .maxlength = uint32_t(-1),
      .tlength = uint32_t(-1),
      .prebuf = uint32_t(-1),
      .minreq = uint32_t(-1),
      .fragsize = uint32_t(frame_size * channels * sizeof(float))
    };

    int status;

    mic->mic.reset(
      pa_simple_new(nullptr, "sunshine", pa_stream_direction_t::PA_STREAM_RECORD, source_name.c_str(), "sunshine-record", &ss, &pa_map, &pa_attr, &status)
    );

    if (!mic->mic) {
      auto err_str = pa_strerror(status);
      BOOST_LOG(error) << "pa_simple_new() failed: "sv << err_str;
      return nullptr;
    }

    return mic;
  }

  namespace pa {
    /**
     * @brief Calls @p f from a PulseAudio callback and wakes the thread waiting in server_t::run().
     *        Use `&waiter_t<Args...>::cb` as the callback and the waiter as its userdata.
     */
    template<class... Args>
    struct waiter_t {
      pa_threaded_mainloop *loop;
      std::function<void(Args...)> f;

      static void cb(pa_context *, Args... args, void *userdata) {
        auto self = static_cast<waiter_t *>(userdata);
        self->f(args...);
        pa_threaded_mainloop_signal(self->loop, 0);
      }
    };

    struct sink_entry_t {
      sink_desc_t desc;
      std::uint32_t owner_module;
    };

    class server_t: public audio_control_t, public backend_t {
    public:
      pa_threaded_mainloop *loop = nullptr;
      pa_context *ctx = nullptr;
      bool loop_running = false;

      struct {
        std::uint32_t stereo = PA_INVALID_INDEX;
        std::uint32_t surround51 = PA_INVALID_INDEX;
        std::uint32_t surround71 = PA_INVALID_INDEX;
      } index;

      sink_manager_t policy {*this};

      // Server events, collected on the mainloop thread and handled on event_worker (handling them needs blocking
      // calls, which the mainloop thread cannot make).
      std::mutex events_lock;
      std::condition_variable events_cv;
      bool events_stop = false;
      bool server_changed = false;
      std::set<std::uint32_t> inputs_changed;
      std::set<std::uint32_t> inputs_removed;
      std::thread event_worker;

      /**
       * @brief Start an operation with the mainloop locked and wait until it finished.
       * @param start Returns the pa_operation, or nullptr if it could not be started. The operation's callback must
       *              wake the waiter (use waiter_t), or this never returns.
       * @return false if the operation could not be started or did not complete.
       */
      template<class F>
      bool run(F &&start) {
        pa_threaded_mainloop_lock(loop);
        pa_operation *op = start();
        if (!op) {
          BOOST_LOG(error) << "Couldn't start pulseaudio operation: "sv << pa_strerror(pa_context_errno(ctx));
          pa_threaded_mainloop_unlock(loop);
          return false;
        }
        while (pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
          pa_threaded_mainloop_wait(loop);
        }
        bool done = pa_operation_get_state(op) == PA_OPERATION_DONE;
        pa_operation_unref(op);
        pa_threaded_mainloop_unlock(loop);
        return done;
      }

      int context_errno() {
        pa_threaded_mainloop_lock(loop);
        auto err = pa_context_errno(ctx);
        pa_threaded_mainloop_unlock(loop);
        return err;
      }

      static void state_cb(pa_context *, void *userdata) {
        pa_threaded_mainloop_signal(static_cast<server_t *>(userdata)->loop, 0);
      }

      static void subscribe_cb(pa_context *, pa_subscription_event_type_t t, std::uint32_t idx, void *userdata) {
        auto self = static_cast<server_t *>(userdata);
        auto facility = t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
        auto type = t & PA_SUBSCRIPTION_EVENT_TYPE_MASK;

        {
          std::lock_guard lg {self->events_lock};
          if (facility == PA_SUBSCRIPTION_EVENT_SERVER) {
            self->server_changed = true;
          } else if (facility == PA_SUBSCRIPTION_EVENT_SINK_INPUT) {
            if (type == PA_SUBSCRIPTION_EVENT_REMOVE) {
              self->inputs_changed.erase(idx);
              self->inputs_removed.insert(idx);
            } else {
              self->inputs_changed.insert(idx);
            }
          } else {
            return;
          }
        }
        self->events_cv.notify_one();
      }

      void handle_events() {
        std::unique_lock ul {events_lock};
        while (true) {
          events_cv.wait(ul, [this] {
            return events_stop || server_changed || !inputs_changed.empty() || !inputs_removed.empty();
          });
          if (events_stop) {
            return;
          }
          auto server = std::exchange(server_changed, false);
          auto changed = std::exchange(inputs_changed, {});
          auto removed = std::exchange(inputs_removed, {});
          ul.unlock();

          for (auto idx : removed) {
            policy.on_sink_input_removed(idx);
          }
          if (server) {
            policy.on_server_changed({config::audio.keep_default, config::audio.auto_capture});
          }
          for (auto idx : changed) {
            policy.on_sink_input_changed(idx);
          }

          ul.lock();
        }
      }

      int init() {
        loop = pa_threaded_mainloop_new();
        if (!loop) {
          return -1;
        }
        ctx = pa_context_new(pa_threaded_mainloop_get_api(loop), "sunshine");
        if (!ctx) {
          return -1;
        }
        pa_context_set_state_callback(ctx, state_cb, this);
        pa_context_set_subscribe_callback(ctx, subscribe_cb, this);

        if (pa_context_connect(ctx, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
          BOOST_LOG(error) << "Couldn't connect to pulseaudio: "sv << pa_strerror(pa_context_errno(ctx));
          return -1;
        }

        pa_threaded_mainloop_lock(loop);
        if (pa_threaded_mainloop_start(loop) < 0) {
          pa_threaded_mainloop_unlock(loop);
          BOOST_LOG(error) << "Couldn't run pulseaudio main loop"sv;
          return -1;
        }
        loop_running = true;

        while (true) {
          auto state = pa_context_get_state(ctx);
          if (state == PA_CONTEXT_READY) {
            break;
          }
          if (!PA_CONTEXT_IS_GOOD(state)) {
            BOOST_LOG(error) << "Couldn't connect to pulseaudio: "sv << pa_strerror(pa_context_errno(ctx));
            pa_threaded_mainloop_unlock(loop);
            return -1;
          }
          pa_threaded_mainloop_wait(loop);
        }
        pa_threaded_mainloop_unlock(loop);

        // Default sink changes (server) and new streams (sink inputs).
        int subscribed = 0;
        waiter_t<int> w {loop, [&](int s) {
                           subscribed = s;
                         }};
        auto mask = (pa_subscription_mask_t) (PA_SUBSCRIPTION_MASK_SERVER | PA_SUBSCRIPTION_MASK_SINK_INPUT);
        if (!run([&] {
              return pa_context_subscribe(ctx, mask, &decltype(w)::cb, &w);
            }) ||
            !subscribed) {
          BOOST_LOG(warning) << "Couldn't subscribe to pulseaudio events; default sink changes will not be followed"sv;
        }

        event_worker = std::thread {[this] {
          handle_events();
        }};

        policy.prime();
        return 0;
      }

      // backend_t

      std::optional<std::string> default_sink() override {
        std::optional<std::string> name;
        waiter_t<const pa_server_info *> w {loop, [&](const pa_server_info *info) {
                                              if (info) {
                                                name = info->default_sink_name ? info->default_sink_name : "";
                                              }
                                            }};
        if (!run([&] {
              return pa_context_get_server_info(ctx, &decltype(w)::cb, &w);
            }) ||
            !name) {
          BOOST_LOG(error) << "Couldn't get pulseaudio server info: "sv << pa_strerror(context_errno());
          return std::nullopt;
        }
        return name;
      }

      int set_default_sink(const std::string &name) override {
        int success = 0;
        waiter_t<int> w {loop, [&](int s) {
                           success = s;
                         }};
        if (!run([&] {
              return pa_context_set_default_sink(ctx, name.c_str(), &decltype(w)::cb, &w);
            }) ||
            !success) {
          BOOST_LOG(error) << "Couldn't set default-sink ["sv << name << "]: "sv << pa_strerror(context_errno());
          return -1;
        }
        return 0;
      }

      std::optional<std::vector<sink_entry_t>> sink_entries() {
        std::vector<sink_entry_t> result;
        bool failed = false;
        waiter_t<const pa_sink_info *, int> w {loop, [&](const pa_sink_info *info, int eol) {
                                                 failed = failed || eol < 0;
                                                 if (info && !eol) {
                                                   result.push_back({{info->index, info->name ? info->name : "", info->description ? info->description : ""}, info->owner_module});
                                                 }
                                               }};
        if (!run([&] {
              return pa_context_get_sink_info_list(ctx, &decltype(w)::cb, &w);
            }) ||
            failed) {
          BOOST_LOG(error) << "Couldn't get pulseaudio sink info: "sv << pa_strerror(context_errno());
          return std::nullopt;
        }
        return result;
      }

      std::optional<std::vector<sink_desc_t>> sinks() override {
        auto entries = sink_entries();
        if (!entries) {
          return std::nullopt;
        }
        std::vector<sink_desc_t> result;
        for (auto &entry : *entries) {
          result.push_back(std::move(entry.desc));
        }
        return result;
      }

      static sink_input_desc_t to_desc(const pa_sink_input_info *info) {
        std::string name;
        if (auto app = pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_NAME)) {
          name = app;
          name += ": "sv;
        }
        name += info->name ? info->name : "";
        return {info->index, info->sink, std::move(name)};
      }

      std::optional<std::vector<sink_input_desc_t>> sink_inputs() override {
        std::vector<sink_input_desc_t> result;
        bool failed = false;
        waiter_t<const pa_sink_input_info *, int> w {loop, [&](const pa_sink_input_info *info, int eol) {
                                                       failed = failed || eol < 0;
                                                       if (info && !eol) {
                                                         result.push_back(to_desc(info));
                                                       }
                                                     }};
        if (!run([&] {
              return pa_context_get_sink_input_info_list(ctx, &decltype(w)::cb, &w);
            }) ||
            failed) {
          BOOST_LOG(error) << "Couldn't list pulseaudio sink inputs: "sv << pa_strerror(context_errno());
          return std::nullopt;
        }
        return result;
      }

      std::optional<sink_input_desc_t> sink_input(std::uint32_t idx) override {
        std::optional<sink_input_desc_t> result;
        waiter_t<const pa_sink_input_info *, int> w {loop, [&](const pa_sink_input_info *info, int eol) {
                                                       if (info && !eol) {
                                                         result = to_desc(info);
                                                       }
                                                     }};
        // A stream that is already gone fails the operation; that is not worth a log line.
        run([&] {
          return pa_context_get_sink_input_info(ctx, idx, &decltype(w)::cb, &w);
        });
        return result;
      }

      int move_sink_input(std::uint32_t idx, const std::string &sink) override {
        int success = 0;
        waiter_t<int> w {loop, [&](int s) {
                           success = s;
                         }};
        if (!run([&] {
              return pa_context_move_sink_input_by_name(ctx, idx, sink.c_str(), &decltype(w)::cb, &w);
            }) ||
            !success) {
          BOOST_LOG(debug) << "Couldn't move sink input ["sv << idx << "] to ["sv << sink << "]: "sv << pa_strerror(context_errno());
          return -1;
        }
        return 0;
      }

      // audio_control_t

      std::uint32_t load_null(const char *name, const std::uint8_t *channel_mapping, int channels) {
        std::uint32_t module = PA_INVALID_INDEX;
        waiter_t<std::uint32_t> w {loop, [&](std::uint32_t idx) {
                                     module = idx;
                                   }};
        auto args = to_string(name, channel_mapping, channels);
        run([&] {
          return pa_context_load_module(ctx, "module-null-sink", args.c_str(), &decltype(w)::cb, &w);
        });
        return module;
      }

      int unload_null(std::uint32_t i) {
        if (i == PA_INVALID_INDEX) {
          return 0;
        }

        int success = 0;
        waiter_t<int> w {loop, [&](int s) {
                           success = s;
                         }};
        if (!run([&] {
              return pa_context_unload_module(ctx, i, &decltype(w)::cb, &w);
            }) ||
            !success) {
          BOOST_LOG(error) << "Couldn't unload null-sink with index ["sv << i << "]: "sv << pa_strerror(context_errno());
          return -1;
        }

        return 0;
      }

      std::optional<sink_t> sink_info() override {
        constexpr auto stereo = "sink-sunshine-stereo";
        constexpr auto surround51 = "sink-sunshine-surround51";
        constexpr auto surround71 = "sink-sunshine-surround71";

        auto entries = sink_entries();
        if (!entries) {
          return std::nullopt;
        }

        sink_t sink;
        sink.host = default_sink().value_or(std::string {});

        // A configured virtual sink replaces Apollo's null sinks for every channel layout.
        if (!config::audio.virtual_sink.empty()) {
          auto name = policy.find_sink(config::audio.virtual_sink);
          if (name) {
            BOOST_LOG(info) << "Using virtual audio sink ["sv << *name << ']';
            policy.set_mute_sinks({*name});
            sink.null = std::make_optional(sink_t::null_t {*name, *name, *name});
            return std::make_optional(std::move(sink));
          }
          BOOST_LOG(warning) << "Couldn't find the specified virtual audio sink ["sv << config::audio.virtual_sink << "]; using Apollo's null sinks"sv;
        }

        // Count of all virtual sinks that are created by us
        int nullcount = 0;

        // Ensure Sunshine won't create a sink that already exists.
        for (const auto &entry : *entries) {
          if (entry.desc.name == stereo) {
            index.stereo = entry.owner_module;
            ++nullcount;
          } else if (entry.desc.name == surround51) {
            index.surround51 = entry.owner_module;
            ++nullcount;
          } else if (entry.desc.name == surround71) {
            index.surround71 = entry.owner_module;
            ++nullcount;
          }
        }

        if (index.stereo == PA_INVALID_INDEX) {
          index.stereo = load_null(stereo, speaker::map_stereo, sizeof(speaker::map_stereo));
          if (index.stereo == PA_INVALID_INDEX) {
            BOOST_LOG(warning) << "Couldn't create virtual sink for stereo: "sv << pa_strerror(context_errno());
          } else {
            ++nullcount;
          }
        }

        if (index.surround51 == PA_INVALID_INDEX) {
          index.surround51 = load_null(surround51, speaker::map_surround51, sizeof(speaker::map_surround51));
          if (index.surround51 == PA_INVALID_INDEX) {
            BOOST_LOG(warning) << "Couldn't create virtual sink for surround-51: "sv << pa_strerror(context_errno());
          } else {
            ++nullcount;
          }
        }

        if (index.surround71 == PA_INVALID_INDEX) {
          index.surround71 = load_null(surround71, speaker::map_surround71, sizeof(speaker::map_surround71));
          if (index.surround71 == PA_INVALID_INDEX) {
            BOOST_LOG(warning) << "Couldn't create virtual sink for surround-71: "sv << pa_strerror(context_errno());
          } else {
            ++nullcount;
          }
        }

        if (sink.host.empty()) {
          BOOST_LOG(warning) << "Couldn't find an active default sink. Continuing with virtual audio only."sv;
        }

        if (nullcount == 3) {
          policy.set_mute_sinks({stereo, surround51, surround71});
          sink.null = std::make_optional(sink_t::null_t {stereo, surround51, surround71});
        }

        return std::make_optional(std::move(sink));
      }

      std::string get_monitor_name(const std::string &sink_name) {
        std::string monitor_name;

        if (sink_name.empty()) {
          return monitor_name;
        }

        waiter_t<const pa_sink_info *, int> w {loop, [&](const pa_sink_info *info, int eol) {
                                                 if (info && !eol && info->monitor_source_name) {
                                                   monitor_name = info->monitor_source_name;
                                                 }
                                               }};
        if (!run([&] {
              return pa_context_get_sink_info_by_name(ctx, sink_name.c_str(), &decltype(w)::cb, &w);
            })) {
          BOOST_LOG(error) << "Couldn't get pulseaudio sink info for ["sv << sink_name << "]: "sv << pa_strerror(context_errno());
        }

        // No need to check status. If it failed just return default name.
        BOOST_LOG(info) << "Found default monitor by name: "sv << monitor_name;
        return monitor_name;
      }

      std::unique_ptr<mic_t> microphone(const std::uint8_t *mapping, int channels, std::uint32_t sample_rate, std::uint32_t frame_size) override {
        // Read the generation first: a default change while this mic is set up then reinitialises it.
        auto generation = policy.capture_generation();
        auto start_generation = generation->load();

        // Sink choice priority:
        // 1. The sink Apollo made the default (a virtual sink when host audio is off), or the default it followed
        // 2. Config sink
        // 3. Default Sink
        auto sink_name = policy.capture_sink(config::audio.sink);

        auto mic = ::platf::microphone(mapping, channels, sample_rate, frame_size, get_monitor_name(sink_name));
        if (mic && config::audio.auto_capture) {
          mic->generation = std::move(generation);
          mic->start_generation = start_generation;
        }
        return mic;
      }

      bool is_sink_available(const std::string &sink) override {
        return policy.is_sink_available(sink);
      }

      int set_sink(const std::string &sink) override {
        return policy.set_sink(sink);
      }

      ~server_t() override {
        if (event_worker.joinable()) {
          {
            std::lock_guard lg {events_lock};
            events_stop = true;
          }
          events_cv.notify_one();
          event_worker.join();
        }

        if (loop_running) {
          // Streams moved for a mute go back before their sink disappears.
          policy.unmute();

          unload_null(index.stereo);
          unload_null(index.surround51);
          unload_null(index.surround71);

          pa_threaded_mainloop_lock(loop);
          pa_context_disconnect(ctx);
          pa_threaded_mainloop_unlock(loop);
          pa_threaded_mainloop_stop(loop);
        }

        if (ctx) {
          pa_context_unref(ctx);
        }
        if (loop) {
          pa_threaded_mainloop_free(loop);
        }
      }
    };
  }  // namespace pa

  std::unique_ptr<audio_control_t> audio_control() {
    auto audio = std::make_unique<pa::server_t>();

    if (audio->init()) {
      return nullptr;
    }

    return audio;
  }
}  // namespace platf
