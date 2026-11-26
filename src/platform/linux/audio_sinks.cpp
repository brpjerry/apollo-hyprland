/**
 * @file src/platform/linux/audio_sinks.cpp
 * @brief Definitions for the Linux default-sink and stream policy.
 */
// standard includes
#include <algorithm>
#include <thread>

// local includes
#include "audio_sinks.h"
#include "src/logging.h"

using namespace std::literals;

namespace platf::pa {
  sink_manager_t::sink_manager_t(backend_t &backend):
      _backend {backend},
      _generation {std::make_shared<std::atomic<std::uint64_t>>(0)} {
  }

  void sink_manager_t::prime() {
    auto current = _backend.default_sink();
    std::lock_guard lg {_lock};
    if (current) {
      _last_default = std::move(current);
    }
  }

  void sink_manager_t::set_mute_sinks(std::vector<std::string> names) {
    std::lock_guard lg {_lock};
    _mute_sinks = std::move(names);
  }

  bool sink_manager_t::is_mute_sink(const std::string &name) const {
    std::lock_guard lg {_lock};
    return std::ranges::find(_mute_sinks, name) != _mute_sinks.end();
  }

  bool sink_manager_t::muted() const {
    std::lock_guard lg {_lock};
    return !_mute_sink.empty();
  }

  std::shared_ptr<const std::atomic<std::uint64_t>> sink_manager_t::capture_generation() const {
    return _generation;
  }

  std::optional<std::string> sink_manager_t::find_sink_locked(const std::string &name_or_description) {
    auto sinks = _backend.sinks();
    if (!sinks || name_or_description.empty()) {
      return std::nullopt;
    }
    for (const auto &sink : *sinks) {
      if (sink.name == name_or_description) {
        return sink.name;
      }
    }
    std::optional<std::string> match;
    for (const auto &sink : *sinks) {
      if (sink.description == name_or_description) {
        if (match) {
          BOOST_LOG(warning) << "Several audio sinks are called ["sv << name_or_description << "]; use the sink name instead"sv;
          return std::nullopt;
        }
        match = sink.name;
      }
    }
    return match;
  }

  int sink_manager_t::apply_default_locked(const std::string &sink) {
    if (_backend.set_default_sink(sink)) {
      return -1;
    }
    // pipewire-pulse stores the request and WirePlumber applies it a moment later; until then the server still reports
    // the old default. Wait for it, so that the change events in between are not mistaken for someone else's change.
    auto deadline = clock::now() + 2s;
    while (_backend.default_sink() != sink) {
      if (clock::now() > deadline) {
        BOOST_LOG(warning) << "The sound server did not switch the default sink to ["sv << sink << ']';
        break;
      }
      std::this_thread::sleep_for(20ms);
    }
    return 0;
  }

  std::optional<std::string> sink_manager_t::find_sink(const std::string &name_or_description) {
    std::lock_guard lg {_lock};
    return find_sink_locked(name_or_description);
  }

  bool sink_manager_t::is_sink_available(const std::string &name_or_description) {
    return find_sink(name_or_description).has_value();
  }

  int sink_manager_t::set_sink(const std::string &requested) {
    std::lock_guard lg {_lock};

    auto sink = find_sink_locked(requested).value_or(requested);
    BOOST_LOG(info) << "Setting default sink to: ["sv << sink << "]"sv;
    if (apply_default_locked(sink)) {
      BOOST_LOG(error) << "Couldn't set default-sink ["sv << sink << "]"sv;
      return -1;
    }
    _assigned = sink;
    _last_default = sink;

    bool mute = std::ranges::find(_mute_sinks, sink) != _mute_sinks.end();
    if (!mute) {
      unmute_locked();
      return 0;
    }

    if (_mute_sink != sink) {
      unmute_locked();
      _mute_sink = sink;
    }

    // Streams that follow the default are being moved by the session manager right now; the ones that stay on another
    // sink have a stored or explicit target. Move whatever is not on the mute sink yet.
    auto inputs = _backend.sink_inputs();
    auto sinks = _backend.sinks();
    if (inputs && sinks) {
      move_to_mute_sink_locked(*inputs, *sinks);
    } else {
      BOOST_LOG(warning) << "Couldn't list audio streams; streams pinned to other sinks stay audible on the host"sv;
    }
    return 0;
  }

  void sink_manager_t::move_to_mute_sink_locked(const std::vector<sink_input_desc_t> &inputs, const std::vector<sink_desc_t> &sinks) {
    auto sink_name = [&](std::uint32_t index) -> std::optional<std::string> {
      for (const auto &sink : sinks) {
        if (sink.index == index) {
          return sink.name;
        }
      }
      return std::nullopt;
    };

    for (const auto &input : inputs) {
      if (_seen.contains(input.index)) {
        continue;
      }
      if (input.sink == invalid_index) {
        // Not linked yet; look again when it changes.
        continue;
      }
      // An input counts as seen once it is on the mute sink or was moved there; a later move by the user is then
      // left alone. Until then (its sink not listed yet, or the move failed) its next change tries again.
      auto from = sink_name(input.sink);
      if (!from) {
        continue;
      }
      if (*from == _mute_sink) {
        _seen.insert(input.index);
        continue;
      }
      if (_backend.move_sink_input(input.index, _mute_sink)) {
        BOOST_LOG(warning) << "Couldn't move audio stream ["sv << input.name << "] from ["sv << *from << "] to ["sv << _mute_sink << ']';
        continue;
      }
      BOOST_LOG(info) << "Moved audio stream ["sv << input.name << "] from ["sv << *from << "] to ["sv << _mute_sink << ']';
      _seen.insert(input.index);
      _moved.emplace(input.index, *from);
    }
  }

  void sink_manager_t::unmute_locked() {
    if (_mute_sink.empty()) {
      return;
    }
    auto mute_sink = std::exchange(_mute_sink, {});
    auto moved = std::exchange(_moved, {});
    _seen.clear();
    if (moved.empty()) {
      return;
    }

    auto inputs = _backend.sink_inputs();
    auto sinks = _backend.sinks();
    if (!inputs || !sinks) {
      BOOST_LOG(warning) << "Couldn't list audio streams; streams moved to ["sv << mute_sink << "] stay there"sv;
      return;
    }

    // Moved streams were moved onto the default sink, so they now follow the default: once the default is back on a
    // real sink, the session manager may already have taken them there.
    std::set<std::uint32_t> from;
    for (const auto &sink : *sinks) {
      if (sink.name == mute_sink || sink.name == _last_default) {
        from.insert(sink.index);
      }
    }

    for (const auto &input : *inputs) {
      auto it = moved.find(input.index);
      if (it == moved.end()) {
        continue;
      }
      // Leave streams alone that were moved somewhere else in the meantime.
      if (!from.contains(input.sink)) {
        continue;
      }
      const auto &to = it->second;
      if (std::ranges::none_of(*sinks, [&](const auto &sink) { return sink.name == to; })) {
        BOOST_LOG(info) << "Audio stream ["sv << input.name << "] played to ["sv << to << "], which is gone; leaving it on the default sink"sv;
        continue;
      }
      if (_backend.move_sink_input(input.index, to)) {
        BOOST_LOG(warning) << "Couldn't move audio stream ["sv << input.name << "] back to ["sv << to << ']';
        continue;
      }
      BOOST_LOG(info) << "Moved audio stream ["sv << input.name << "] back to ["sv << to << ']';
    }
  }

  void sink_manager_t::unmute() {
    std::lock_guard lg {_lock};
    unmute_locked();
  }

  std::string sink_manager_t::capture_sink(const std::string &config_sink) {
    {
      std::lock_guard lg {_lock};
      if (!_assigned.empty()) {
        return _assigned;
      }
    }
    if (!config_sink.empty()) {
      return find_sink(config_sink).value_or(config_sink);
    }
    return _backend.default_sink().value_or(std::string {});
  }

  void sink_manager_t::on_server_changed(const sink_options_t &options, clock::time_point now) {
    // Query under the lock: a set_sink() in progress must finish first, or this could see the default it replaces.
    std::lock_guard lg {_lock};
    auto current = _backend.default_sink();
    if (!current) {
      return;
    }
    if (_last_default == current) {
      // Something else about the server changed.
      return;
    }
    BOOST_LOG(info) << "Default audio sink changed to ["sv << *current << ']';
    _last_default = current;

    if (_assigned.empty() || *current == _assigned) {
      if (_assigned.empty() && options.auto_capture) {
        // Apollo never set a default (the configured sink, if any, already was it): follow the new one. Recording
        // it matters when a sink is configured, which capture_sink() would otherwise pick again.
        _assigned = *current;
        ++*_generation;
      }
      return;
    }

    // Only a mute Apollo started is defended. A virtual sink Apollo merely followed (host audio on, the user picked
    // it) is the user's choice like any other, and so is the next one.
    if (options.keep_default && !_mute_sink.empty() && _assigned == _mute_sink) {
      while (!_restores.empty() && now - _restores.front() > keep_default_window) {
        _restores.pop_front();
      }
      if (_restores.size() < keep_default_limit) {
        _restores.push_back(now);
        BOOST_LOG(info) << "Resetting sink to ["sv << _assigned << "] after default changed"sv;
        if (!apply_default_locked(_assigned)) {
          _last_default = _assigned;
          return;
        }
        BOOST_LOG(warning) << "Couldn't reset the default sink to ["sv << _assigned << ']';
      } else {
        // Something keeps changing it back; stop fighting and treat the change as the user's choice.
        BOOST_LOG(warning) << "The default sink keeps changing away from ["sv << _assigned << "]; no longer resetting it"sv;
      }
    }

    // The default moved away from Apollo's choice and stays there. A mute is over: the user picked a sink.
    unmute_locked();
    if (options.auto_capture) {
      _assigned = *current;
      ++*_generation;
    }
  }

  void sink_manager_t::on_sink_input_changed(std::uint32_t index) {
    std::lock_guard lg {_lock};
    if (_mute_sink.empty() || _seen.contains(index)) {
      return;
    }
    auto input = _backend.sink_input(index);
    auto sinks = _backend.sinks();
    if (!input || !sinks) {
      return;
    }
    move_to_mute_sink_locked({*input}, *sinks);
  }

  void sink_manager_t::on_sink_input_removed(std::uint32_t index) {
    std::lock_guard lg {_lock};
    _seen.erase(index);
    _moved.erase(index);
  }
}  // namespace platf::pa
