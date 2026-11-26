/**
 * @file src/platform/linux/audio_sinks.h
 * @brief Default-sink and stream policy for Linux audio (PulseAudio API, also served by pipewire-pulse).
 * @details The policy talks to the sound server through @ref platf::pa::backend_t only, so it can be tested
 *          without a server. The PulseAudio implementation of the backend lives in audio.cpp.
 */
#pragma once

// standard includes
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace platf::pa {
  constexpr std::uint32_t invalid_index = UINT32_MAX;

  struct sink_desc_t {
    std::uint32_t index;
    std::string name;
    std::string description;
  };

  struct sink_input_desc_t {
    std::uint32_t index;
    /** Index of the sink the input plays to, invalid_index while it is not linked. */
    std::uint32_t sink;
    std::string name;
  };

  /**
   * @brief The sound server operations the policy needs. Every call blocks until the server answered.
   *        Failures are std::nullopt or a non-zero return.
   */
  class backend_t {
  public:
    virtual ~backend_t() = default;

    virtual std::optional<std::string> default_sink() = 0;
    virtual int set_default_sink(const std::string &name) = 0;
    virtual std::optional<std::vector<sink_desc_t>> sinks() = 0;
    virtual std::optional<std::vector<sink_input_desc_t>> sink_inputs() = 0;
    virtual std::optional<sink_input_desc_t> sink_input(std::uint32_t index) = 0;
    virtual int move_sink_input(std::uint32_t index, const std::string &sink) = 0;
  };

  struct sink_options_t {
    /** config::audio.keep_default: put a virtual sink back as default when something else replaces it. */
    bool keep_default = true;
    /** config::audio.auto_capture: capture the new default sink after the default changed. */
    bool auto_capture = true;
  };

  /**
   * @brief Tracks the sink Apollo assigned as default and keeps the host muted while a virtual sink is in use.
   *
   * Muting: when set_sink() makes a virtual ("mute") sink the default, streams that follow the default move to it on
   * their own, but streams with a stored target (WirePlumber's restore-stream, `pactl move-sink-input`) stay on
   * their hardware sink. Those are moved to the virtual sink explicitly, as are streams that appear while muted, and
   * moved back to the sink they came from when the mute ends. The default is switched before streams are moved and
   * switched back before they are restored, so a stream moved onto the default sink is not pinned to it
   * (pipewire-pulse stores "follow the default" for a move onto the current default).
   *
   * All methods are thread-safe. They call the backend, so they must not run on the sound server's event thread.
   */
  class sink_manager_t {
  public:
    using clock = std::chrono::steady_clock;

    explicit sink_manager_t(backend_t &backend);

    /** Remember the current default sink, so that later changes can be recognised. */
    void prime();

    /** Sinks that mean "host audio off": Apollo's null sinks or the configured virtual sink. */
    void set_mute_sinks(std::vector<std::string> names);
    bool is_mute_sink(const std::string &name) const;

    /**
     * @brief Make @p sink the default sink. A mute sink also moves the remaining streams onto it; any other sink
     *        ends a mute and moves those streams back.
     * @param sink A sink name, or a description that names exactly one sink.
     * @return 0 on success.
     */
    int set_sink(const std::string &sink);

    /** The sink whose monitor should be captured: the last assigned sink, else @p config_sink, else the default. */
    std::string capture_sink(const std::string &config_sink);

    /** The name of the sink called @p name_or_description (name first, then description). */
    std::optional<std::string> find_sink(const std::string &name_or_description);
    bool is_sink_available(const std::string &name_or_description);

    /** Handle a server change event (the default sink may have changed). */
    void on_server_changed(const sink_options_t &options, clock::time_point now = clock::now());

    /** Handle a new or changed sink input. */
    void on_sink_input_changed(std::uint32_t index);
    void on_sink_input_removed(std::uint32_t index);

    /** Move streams that were moved for a mute back to their sinks. */
    void unmute();

    bool muted() const;

    /** Incremented whenever capture should be reinitialised to follow a new default sink. */
    std::shared_ptr<const std::atomic<std::uint64_t>> capture_generation() const;

    /** Restores of the default allowed within keep_default_window before keep_default gives up (another program
     *  keeps changing it). */
    static constexpr int keep_default_limit = 5;
    static constexpr auto keep_default_window = std::chrono::seconds(10);

  private:
    void move_to_mute_sink_locked(const std::vector<sink_input_desc_t> &inputs, const std::vector<sink_desc_t> &sinks);
    void unmute_locked();
    std::optional<std::string> find_sink_locked(const std::string &name_or_description);
    /** Set the default sink and wait until the server reports it. */
    int apply_default_locked(const std::string &sink);

    backend_t &_backend;
    mutable std::mutex _lock;

    std::vector<std::string> _mute_sinks;
    /** The sink set_sink() made the default, or the default followed after a change. */
    std::string _assigned;
    /** The default sink as last seen. */
    std::optional<std::string> _last_default;

    /** The mute sink in use, empty when not muted. */
    std::string _mute_sink;
    /** Sink inputs moved for the mute: index -> name of the sink they played to. */
    std::map<std::uint32_t, std::string> _moved;
    /** Sink inputs already considered during this mute. */
    std::set<std::uint32_t> _seen;

    std::deque<clock::time_point> _restores;
    std::shared_ptr<std::atomic<std::uint64_t>> _generation;
  };
}  // namespace platf::pa
