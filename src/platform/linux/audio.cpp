/**
 * @file src/platform/linux/audio.cpp
 * @brief Definitions for audio control on Linux.
 */
// standard includes
#include <algorithm>
#include <array>
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

  /**
   * @brief PulseAudio position for each `speaker::speaker_e`, in `speaker_e` order.
   */
  constexpr std::array<pa_channel_position_t, speaker::MAX_SPEAKERS> position_mapping {
    PA_CHANNEL_POSITION_FRONT_LEFT,
    PA_CHANNEL_POSITION_FRONT_RIGHT,
    PA_CHANNEL_POSITION_FRONT_CENTER,
    PA_CHANNEL_POSITION_LFE,
    PA_CHANNEL_POSITION_REAR_LEFT,
    PA_CHANNEL_POSITION_REAR_RIGHT,
    PA_CHANNEL_POSITION_SIDE_LEFT,
    PA_CHANNEL_POSITION_SIDE_RIGHT,
  };

  /**
   * @brief Convert a PulseAudio operation result to a log string.
   *
   * @param name Human-readable name to assign.
   * @param mapping Opus channel mapping table for the requested layout.
   * @param channels Number of audio channels in the stream.
   * @return Value converted to string.
   */
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

  /**
   * @brief PulseAudio recording stream and channel metadata.
   */
  struct mic_attr_t: public mic_t {
    util::safe_ptr<pa_simple, &pa_simple_free> mic;  ///< PulseAudio simple recording stream for microphone capture.

    /**
     * @brief Deliver a captured audio sample to Sunshine's audio pipeline.
     *
     * @param sample_buf Sample buf.
     * @return Capture status reported to the streaming pipeline.
     */
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

        return capture_e::capture_error;
      }

      return capture_e::ok;
    }
  };

  /**
   * @brief Fill a PulseAudio channel map with the canonical layout for a channel count.
   *
   * Capture always uses the canonical layout, never the Opus channel mapping: the Opus mapping
   * is applied by the multistream encoder and undone by the client, so applying it here too
   * would leave the samples permuted. `speaker::map_stereo`, `map_surround51` and
   * `map_surround71` are the identity over `speaker::speaker_e`, so the canonical position of
   * channel `i` is `position_mapping[i]`.
   *
   * @param channels Number of audio channels in the stream.
   * @param pa_map Channel map to fill; left untouched when `channels` is out of range.
   * @return `true` when the channel count maps onto a known speaker layout.
   */
  bool fill_canonical_channel_map(int channels, pa_channel_map &pa_map) {
    if (channels < 1 || channels > speaker::MAX_SPEAKERS) {
      return false;
    }

    pa_channel_map_init(&pa_map);
    pa_map.channels = static_cast<std::uint8_t>(channels);
    for (int i = 0; i < channels; ++i) {
      pa_map.map[i] = position_mapping[i];
    }

    return true;
  }

  /**
   * @brief Create a microphone capture stream for the requested layout.
   * @details Capture always uses the canonical channel layout; Opus mapping is applied by the encoder.
   *
   * @param channels Number of audio channels in the stream.
   * @param sample_rate Audio sample rate in hertz.
   * @param frame_size Number of samples captured per audio frame.
   * @param source_name Source name.
   * @return Microphone capture object for the requested audio layout, or nullptr on failure.
   */
  std::unique_ptr<mic_t> microphone(int channels, std::uint32_t sample_rate, std::uint32_t frame_size, std::string source_name) {
    // Capture in the canonical channel layout, not the one described by the Opus mapping.
    //
    // The Opus mapping is applied by the multistream encoder in audio.cpp and is undone by
    // the client when it decodes, so the two cancel out. Passing it to PulseAudio as the
    // capture channel map makes PulseAudio reorder the samples as well, and that reordering
    // is never undone: the client ends up rendering permuted channels.
    //
    // This is invisible with the built-in mappings, which are all identities, but it
    // scrambles the audio whenever a client requests custom surround-params (the webOS
    // client asks for 0,1,4,5,2,3). The WASAPI backend ignores the mapping for this reason.
    //
    pa_channel_map pa_map;
    if (!fill_canonical_channel_map(channels, pa_map)) {
      BOOST_LOG(error) << "Unsupported channel count for audio capture: "sv << channels;
      return nullptr;
    }

    auto mic = std::make_unique<mic_attr_t>();

    pa_sample_spec ss {PA_SAMPLE_FLOAT32, sample_rate, (std::uint8_t) channels};

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

    /**
     * @brief Template helper that preserves constness for const inputs.
     */
    template<class T>
    struct add_const_helper<true, T> {
      /**
       * @brief PulseAudio object type passed to the safe pointer wrapper.
       */
      using type = const std::remove_pointer_t<T> *;
    };

    /**
     * @brief Template helper that leaves non-const inputs mutable.
     */
    template<class T>
    struct add_const_helper<false, T> {
      /**
       * @brief PulseAudio object type passed to the safe pointer wrapper.
       */
      using type = const T *;
    };

    /**
     * @brief PulseAudio callback info type with pointer constness normalized.
     */
    template<class T>
    using add_const_t = typename add_const_helper<std::is_pointer_v<T>, T>::type;

    /**
     * @brief Release memory allocated by PulseAudio.
     *
     * @param p Pointer allocated by PulseAudio and released with `pa_xfree`.
     */
    template<class T>
    void pa_free(T *p) {
      pa_xfree(p);
    }

    /**
     * @brief Owning pointer for a PulseAudio context.
     */
    using ctx_t = util::safe_ptr<pa_context, &pa_context_unref>;
    /**
     * @brief Owning pointer for a PulseAudio mainloop.
     */
    using loop_t = util::safe_ptr<pa_mainloop, &pa_mainloop_free>;
    /**
     * @brief Owning pointer for a PulseAudio asynchronous operation.
     */
    using op_t = util::safe_ptr<pa_operation, &pa_operation_unref>;
    /**
     * @brief Owning pointer for PulseAudio strings allocated with `pa_xmalloc`.
     */
    using string_t = util::safe_ptr<char, &pa_free<char>>;

    /**
     * @brief Callback wrapper for PulseAudio introspection results without an end marker.
     */
    template<class T>
    using cb_simple_t = std::function<void(ctx_t::pointer, add_const_t<T> i)>;

    /**
     * @brief Handle PulseAudio sink-input introspection results.
     *
     * @param ctx Native context object used by the operation or callback.
     * @param i PulseAudio introspection info supplied to the callback.
     * @param userdata Caller-provided pointer passed through the callback.
     */
    template<class T>
    void cb(ctx_t::pointer ctx, add_const_t<T> i, void *userdata) {  // NOSONAR(cpp:S5008): PulseAudio introspection callbacks require a void pointer for user data.
      auto &f = *(cb_simple_t<T> *) userdata;

      // Cannot similarly filter on eol here. Unless reported otherwise assume
      // we have no need for special filtering like cb?
      f(ctx, i);
    }

    /**
     * @brief Callback wrapper for PulseAudio introspection results with an end marker.
     */
    template<class T>
    using cb_t = std::function<void(ctx_t::pointer, add_const_t<T> i, int eol)>;

    /**
     * @brief Handle PulseAudio source introspection results.
     *
     * @param ctx Native context object used by the operation or callback.
     * @param i PulseAudio introspection info supplied to the callback.
     * @param eol PulseAudio end-of-list marker.
     * @param userdata Caller-provided pointer passed through the callback.
     */
    template<class T>
    void cb(ctx_t::pointer ctx, add_const_t<T> i, int eol, void *userdata) {  // NOSONAR(cpp:S5008): PulseAudio introspection callbacks require a void pointer for user data.
      auto &f = *(cb_t<T> *) userdata;

      // For some reason, pulseaudio calls this callback after disconnecting
      if (i && eol) {
        return;
      }
    };

      f(ctx, i, eol);
    }

    /**
     * @brief Forward a PulseAudio integer callback value into a Sunshine alarm.
     * @details The PulseAudio callback signature supplies a context that is not needed to ring the alarm.
     *
     * @param i Integer value returned by the PulseAudio operation.
     * @param userdata Caller-provided pointer passed through the callback.
     */
    void cb_i(ctx_t::pointer, std::uint32_t i, void *userdata) {
      auto alarm = (safe::alarm_raw_t<int> *) userdata;

      alarm->ring(i);
    }

    /**
     * @brief Translate PulseAudio context state changes into server events.
     *
     * @param ctx Native context object used by the operation or callback.
     * @param userdata Caller-provided pointer passed through the callback.
     */
    void ctx_state_cb(ctx_t::pointer ctx, void *userdata) {
      const auto &f = *(std::function<void(ctx_t::pointer)> *) userdata;

      f(ctx);
    }

    /**
     * @brief Record completion of a PulseAudio asynchronous operation.
     * @details The PulseAudio callback signature supplies a context that is not needed to report completion.
     *
     * @param status Native status code returned by the platform API.
     * @param userdata Caller-provided pointer passed through the callback.
     */
    void success_cb(ctx_t::pointer, int status, void *userdata) {
      assert(userdata != nullptr);

      auto alarm = (safe::alarm_raw_t<int> *) userdata;
      alarm->ring(status ? 0 : 1);
    }

    /**
     * @brief PulseAudio server controller that creates and removes Sunshine sinks.
     */
    class server_t: public audio_control_t {
      enum ctx_event_e : int {
        ready,
        terminated,
        failed
      };

    class server_t: public audio_control_t, public backend_t {
    public:
      loop_t loop;  ///< PulseAudio threaded mainloop instance.
      ctx_t ctx;  ///< PulseAudio threaded mainloop context.
      std::string requested_sink;  ///< Requested sink.

      struct {
        std::uint32_t stereo = PA_INVALID_INDEX;  ///< PulseAudio module index for the stereo null sink.
        std::uint32_t surround51 = PA_INVALID_INDEX;  ///< PulseAudio module index for the 5.1 null sink.
        std::uint32_t surround71 = PA_INVALID_INDEX;  ///< PulseAudio module index for the 7.1 null sink.
      } index;  ///< PulseAudio module indexes for Sunshine-created null sinks.

      std::unique_ptr<safe::event_t<ctx_event_e>> events;  ///< Event queue receiving PulseAudio context state changes.
      std::unique_ptr<std::function<void(ctx_t::pointer)>> events_cb;  ///< Callback that translates PulseAudio context updates into events.

      std::jthread worker;  ///< Thread running the PulseAudio mainloop.

      /**
       * @brief Initialize PulseAudio mainloop, context, and Sunshine null sinks.
       *
       * @return 0 on success; nonzero or negative platform status on failure.
       */
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

        events_cb = std::make_unique<std::function<void(ctx_t::pointer)>>([this](ctx_t::pointer callback_ctx) {
          switch (pa_context_get_state(callback_ctx)) {
            case PA_CONTEXT_READY:
              events->raise(ready);
              break;
            case PA_CONTEXT_TERMINATED:
              BOOST_LOG(debug) << "PulseAudio context terminated"sv;
              events->raise(terminated);
              break;
            case PA_CONTEXT_FAILED:
              BOOST_LOG(debug) << "PulseAudio context failed"sv;
              events->raise(failed);
              break;
            case PA_CONTEXT_CONNECTING:
              BOOST_LOG(debug) << "Connecting to pulseaudio"sv;
            case PA_CONTEXT_UNCONNECTED:
            case PA_CONTEXT_AUTHORIZING:
            case PA_CONTEXT_SETTING_NAME:
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

        worker = std::jthread {
          [](loop_t::pointer loop) {
            int retval;
            platf::set_thread_name("audio::pulseaudio");
            auto status = pa_mainloop_run(loop, &retval);

            if (status < 0) {
              BOOST_LOG(error) << "Couldn't run pulseaudio main loop"sv;
              return;
            }
          },
          loop.get()
        };

        auto event = events->pop();
        if (event == failed) {
          return -1;
        }

        policy.prime();
        return 0;
      }

      /**
       * @brief Create a PulseAudio null sink for one channel layout.
       *
       * @param name Human-readable name to assign.
       * @param channel_mapping Channel mapping.
       * @param channels Number of audio channels in the stream.
       * @return PulseAudio module index for the new sink, or PA_INVALID_INDEX on failure.
       */
      int load_null(const char *name, const std::uint8_t *channel_mapping, int channels) {
        auto alarm = safe::make_alarm<int>();

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

      /**
       * @brief Unload a Sunshine-created PulseAudio null sink.
       *
       * @param i PulseAudio introspection info supplied to the callback.
       * @return 0 when the sink is absent or unloaded; nonzero on PulseAudio failure.
       */
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

      /**
       * @brief Query host and virtual sink names available to Sunshine.
       *
       * @return Host and virtual sink names when the backend can report them.
       */
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

        cb_t<pa_sink_info *> f = [&](ctx_t::pointer callback_ctx, const pa_sink_info *sink_info, int eol) {
          if (!sink_info) {
            if (!eol) {
              BOOST_LOG(error) << "Couldn't get pulseaudio sink info: "sv << pa_strerror(pa_context_errno(callback_ctx));

              alarm->ring(-1);
            }

            alarm->ring(0);
            return;
          }

          // Ensure Sunshine won't create a sink that already exists.
          if (!std::strcmp(sink_info->name, stereo)) {
            index.stereo = sink_info->owner_module;

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
          index.stereo = load_null(stereo, speaker::map_stereo.data(), static_cast<int>(speaker::map_stereo.size()));
          if (index.stereo == PA_INVALID_INDEX) {
            BOOST_LOG(warning) << "Couldn't create virtual sink for stereo: "sv << pa_strerror(context_errno());
          } else {
            ++nullcount;
          }
        }

        if (index.surround51 == PA_INVALID_INDEX) {
          index.surround51 = load_null(surround51, speaker::map_surround51.data(), static_cast<int>(speaker::map_surround51.size()));
          if (index.surround51 == PA_INVALID_INDEX) {
            BOOST_LOG(warning) << "Couldn't create virtual sink for surround-51: "sv << pa_strerror(context_errno());
          } else {
            ++nullcount;
          }
        }

        if (index.surround71 == PA_INVALID_INDEX) {
          index.surround71 = load_null(surround71, speaker::map_surround71.data(), static_cast<int>(speaker::map_surround71.size()));
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

      /**
       * @brief Get default sink name.
       *
       * @return PulseAudio name of the current default sink, or an empty string.
       */
      std::string get_default_sink_name() {
        std::string sink_name;
        auto alarm = safe::make_alarm<int>();

        cb_simple_t<pa_server_info *> server_f = [&](ctx_t::pointer callback_ctx, const pa_server_info *server_info) {
          if (!server_info) {
            BOOST_LOG(error) << "Couldn't get pulseaudio server info: "sv << pa_strerror(pa_context_errno(callback_ctx));
            alarm->ring(-1);
          }

          if (server_info->default_sink_name) {
            sink_name = server_info->default_sink_name;
          }
          alarm->ring(0);
        };

        op_t server_op {pa_context_get_server_info(ctx.get(), cb<pa_server_info *>, &server_f)};
        alarm->wait();
        // No need to check status. If it failed just return default name.
        return sink_name;
      }

      /**
       * @brief Get monitor name.
       *
       * @param sink_name Sink name.
       * @return PulseAudio monitor source name for the supplied sink, or an empty string.
       */
      std::string get_monitor_name(const std::string &sink_name) {
        std::string monitor_name;

        if (sink_name.empty()) {
          return monitor_name;
        }

        cb_t<pa_sink_info *> sink_f = [&](ctx_t::pointer callback_ctx, const pa_sink_info *sink_info, int eol) {
          if (!sink_info) {
            if (!eol) {
              BOOST_LOG(error) << "Couldn't get pulseaudio sink info for ["sv << sink_name
                               << "]: "sv << pa_strerror(pa_context_errno(callback_ctx));
              alarm->ring(-1);
            }

        // No need to check status. If it failed just return default name.
        BOOST_LOG(info) << "Found default monitor by name: "sv << monitor_name;
        return monitor_name;
      }

      /**
       * @brief Create a microphone capture stream for the requested layout.
       * @details The common audio interface supplies an Opus mapping that canonical capture does not need.
       *
       * @param channels Number of audio channels in the stream.
       * @param sample_rate Audio sample rate in hertz.
       * @param frame_size Number of samples captured per audio frame.
       * @param continuous_audio Continuous audio.
       * @param host_audio_enabled Whether host playback should remain enabled during capture.
       * @return Microphone capture object for the requested audio layout.
       */
      std::unique_ptr<mic_t> microphone(const std::uint8_t *, int channels, std::uint32_t sample_rate, std::uint32_t frame_size, bool continuous_audio, [[maybe_unused]] bool host_audio_enabled) override {
        // Sink choice priority:
        // 1. Config sink
        // 2. Last sink swapped to (Usually virtual in this case)
        // 3. Default Sink
        // An attempt was made to always use default to match the switching mechanic,
        // but this happens right after the swap so the default returned by PA was not
        // the new one just set!
        auto sink_name = config::audio.sink;
        if (sink_name.empty()) {
          sink_name = requested_sink;
        }
        if (sink_name.empty()) {
          sink_name = get_default_sink_name();
        }

        return ::platf::microphone(channels, sample_rate, frame_size, get_monitor_name(sink_name));
      }

      bool is_sink_available(const std::string &sink) override {
        return policy.is_sink_available(sink);
      }

      /**
       * @brief Update the sink value on the backend.
       *
       * @param sink Audio sink name to route or capture.
       * @return Status from updating sink.
       */
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

  /**
   * @brief Create the platform audio controller.
   */
  std::unique_ptr<audio_control_t> audio_control() {
    auto audio = std::make_unique<pa::server_t>();

    if (audio->init()) {
      return nullptr;
    }

    return audio;
  }
}  // namespace platf
