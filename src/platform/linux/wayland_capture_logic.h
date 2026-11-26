/**
 * @file src/platform/linux/wayland_capture_logic.h
 * @brief Pure helpers for Wayland screen capture: buffer constraint and format selection, capture buffer pool
 *        bookkeeping, damage tracking, frame timestamps and pacing.
 * @details No Wayland, GBM or EGL dependency, so everything here can be unit tested without a compositor.
 */
#pragma once

// standard includes
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

namespace wl {
  /// DRM fourcc codes used here (same values as drm_fourcc.h, repeated to keep this header dependency free).
  namespace fourcc {
    constexpr std::uint32_t code(char a, char b, char c, char d) {
      return std::uint32_t(a) | (std::uint32_t(b) << 8) | (std::uint32_t(c) << 16) | (std::uint32_t(d) << 24);
    }

    constexpr std::uint32_t XRGB8888 = code('X', 'R', '2', '4');
    constexpr std::uint32_t ARGB8888 = code('A', 'R', '2', '4');
    constexpr std::uint32_t XBGR8888 = code('X', 'B', '2', '4');
    constexpr std::uint32_t ABGR8888 = code('A', 'B', '2', '4');
    constexpr std::uint32_t XRGB2101010 = code('X', 'R', '3', '0');
    constexpr std::uint32_t ARGB2101010 = code('A', 'R', '3', '0');
    constexpr std::uint32_t XBGR2101010 = code('X', 'B', '3', '0');
    constexpr std::uint32_t ABGR2101010 = code('A', 'B', '3', '0');

    /// DRM_FORMAT_MOD_INVALID: "implicit modifier" when it appears in a list of supported modifiers.
    constexpr std::uint64_t MOD_INVALID = 0x00ffffffffffffffULL;
  }  // namespace fourcc

  /// wl_shm formats: ARGB8888 and XRGB8888 have their own enum values, everything else uses the DRM fourcc.
  namespace shm_format {
    constexpr std::uint32_t ARGB8888 = 0;
    constexpr std::uint32_t XRGB8888 = 1;
  }  // namespace shm_format

  struct dmabuf_format_t {
    std::uint32_t format;
    std::vector<std::uint64_t> modifiers;

    friend bool operator==(const dmabuf_format_t &, const dmabuf_format_t &) = default;
  };

  /**
   * @brief One batch of ext_image_copy_capture_session_v1 buffer constraints (everything up to `done`).
   */
  struct buffer_constraints_t {
    std::uint32_t width {0};
    std::uint32_t height {0};
    std::vector<std::uint32_t> shm_formats;
    std::optional<std::uint64_t> dmabuf_device;  ///< dev_t of the device dma-bufs must be allocated on.
    std::vector<dmabuf_format_t> dmabuf_formats;

    friend bool operator==(const buffer_constraints_t &, const buffer_constraints_t &) = default;
  };

  /**
   * @brief How capture buffers should be allocated.
   */
  struct buffer_spec_t {
    bool shm {false};
    std::uint32_t format {0};  ///< DRM fourcc for dma-bufs, wl_shm format for shm.
    std::uint32_t width {0};
    std::uint32_t height {0};
    /// Explicit modifiers the allocator may choose from. Empty: only the implicit modifier.
    std::vector<std::uint64_t> modifiers;
    /// Row pitch of shm buffers in bytes; 0 means width * 4.
    std::uint32_t stride {0};
    /// Allocating without an explicit modifier is allowed (the compositor listed DRM_FORMAT_MOD_INVALID,
    /// or sent no modifier list at all).
    bool implicit_modifier {true};

    friend bool operator==(const buffer_spec_t &, const buffer_spec_t &) = default;
  };

  /**
   * @brief Picks the buffer type and format to allocate from a set of constraints.
   * @param constraints The constraints announced by the compositor.
   * @param allow_dmabuf Whether dma-bufs can be allocated (a GBM device and linux-dmabuf are available).
   * @param allow_shm Whether shared memory buffers are acceptable (only the system memory path can use them).
   * @param prefer_shm Pick shm over dma-buf when both are possible.
   * @param prefer_10bit Pick a 10-bit dma-buf format when one is offered (HDR streams: less banding once the frames
   *        are converted to PQ). Hyprland only offers them for 10-bit outputs with `misc.screencopy_force_8b = false`.
   * @return The spec, or std::nullopt if no supported RGB format is offered.
   * @details dma-bufs may be 8-bit or 10-bit RGB (they are imported into GL, which reads either); shm is 8-bit only:
   *          XRGB8888/ARGB8888, since the system memory path hands frames to the encoder as BGR0. Opaque formats are
   *          preferred over formats with alpha, and BGRA byte order (XRGB) over RGBA (XBGR).
   */
  inline std::optional<buffer_spec_t> choose_buffer_spec(const buffer_constraints_t &constraints, bool allow_dmabuf, bool allow_shm, bool prefer_shm = false, bool prefer_10bit = false) {
    if (!constraints.width || !constraints.height) {
      return std::nullopt;
    }

    auto pick_dmabuf = [&]() -> std::optional<buffer_spec_t> {
      if (!allow_dmabuf) {
        return std::nullopt;
      }
      const std::vector<std::uint32_t> formats_8bit {fourcc::XRGB8888, fourcc::ARGB8888, fourcc::XBGR8888, fourcc::ABGR8888};
      const std::vector<std::uint32_t> formats_10bit {fourcc::XRGB2101010, fourcc::ARGB2101010, fourcc::XBGR2101010, fourcc::ABGR2101010};
      auto order = prefer_10bit ? formats_10bit : formats_8bit;
      const auto &fallback = prefer_10bit ? formats_8bit : formats_10bit;
      order.insert(order.end(), fallback.begin(), fallback.end());
      for (auto preferred : order) {
        for (const auto &format : constraints.dmabuf_formats) {
          if (format.format != preferred) {
            continue;
          }

          buffer_spec_t spec;
          spec.format = format.format;
          spec.width = constraints.width;
          spec.height = constraints.height;
          spec.implicit_modifier = format.modifiers.empty();
          for (auto modifier : format.modifiers) {
            if (modifier == fourcc::MOD_INVALID) {
              spec.implicit_modifier = true;
            } else if (std::find(spec.modifiers.begin(), spec.modifiers.end(), modifier) == spec.modifiers.end()) {
              spec.modifiers.push_back(modifier);
            }
          }
          return spec;
        }
      }
      return std::nullopt;
    };

    auto pick_shm = [&]() -> std::optional<buffer_spec_t> {
      if (!allow_shm) {
        return std::nullopt;
      }
      for (auto preferred : {shm_format::XRGB8888, shm_format::ARGB8888}) {
        if (std::find(constraints.shm_formats.begin(), constraints.shm_formats.end(), preferred) != constraints.shm_formats.end()) {
          buffer_spec_t spec;
          spec.shm = true;
          spec.format = preferred;
          spec.width = constraints.width;
          spec.height = constraints.height;
          return spec;
        }
      }
      return std::nullopt;
    };

    if (prefer_shm) {
      if (auto spec = pick_shm()) {
        return spec;
      }
      return pick_dmabuf();
    }

    if (auto spec = pick_dmabuf()) {
      return spec;
    }
    return pick_shm();
  }

  /**
   * @brief Converts a Wayland presentation timestamp (CLOCK_MONOTONIC, split in three 32-bit words) into a
   *        steady_clock time point.
   * @param now The current time, used as a sanity check.
   * @return The time point, or std::nullopt if the timestamp is invalid or more than a second away from `now`
   *         (e.g. a compositor that does not use CLOCK_MONOTONIC).
   * @note steady_clock is CLOCK_MONOTONIC on Linux (libstdc++ and libc++).
   */
  inline std::optional<std::chrono::steady_clock::time_point> presentation_time_to_steady(std::uint32_t tv_sec_hi, std::uint32_t tv_sec_lo, std::uint32_t tv_nsec, std::chrono::steady_clock::time_point now) {
    if (tv_nsec >= 1'000'000'000u) {
      return std::nullopt;
    }

    const std::uint64_t seconds = (std::uint64_t(tv_sec_hi) << 32) | tv_sec_lo;
    if (!seconds && !tv_nsec) {
      return std::nullopt;
    }
    // Keep the arithmetic below in range for a signed 64-bit nanosecond count.
    if (seconds > std::uint64_t(std::numeric_limits<std::int64_t>::max() / 1'000'000'000 - 1)) {
      return std::nullopt;
    }

    const auto since_epoch = std::chrono::seconds(seconds) + std::chrono::nanoseconds(tv_nsec);
    const std::chrono::steady_clock::time_point timestamp {std::chrono::duration_cast<std::chrono::steady_clock::duration>(since_epoch)};

    const auto distance = timestamp > now ? timestamp - now : now - timestamp;
    if (distance > std::chrono::seconds(1)) {
      return std::nullopt;
    }

    return timestamp;
  }

  struct rect_t {
    int x {0};
    int y {0};
    int width {0};
    int height {0};

    bool empty() const {
      return width <= 0 || height <= 0;
    }

    friend bool operator==(const rect_t &, const rect_t &) = default;
  };

  /**
   * @brief Bounding box of two rectangles (an empty rectangle is the identity).
   */
  inline rect_t bounding_union(const rect_t &a, const rect_t &b) {
    if (a.empty()) {
      return b;
    }
    if (b.empty()) {
      return a;
    }
    const int x0 = std::min(a.x, b.x);
    const int y0 = std::min(a.y, b.y);
    const int x1 = std::max(a.x + a.width, b.x + b.width);
    const int y1 = std::max(a.y + a.height, b.y + b.height);
    return {x0, y0, x1 - x0, y1 - y0};
  }

  /**
   * @brief Clips a rectangle to [0, width) x [0, height).
   */
  inline rect_t clip(const rect_t &r, int width, int height) {
    const int x0 = std::clamp(r.x, 0, width);
    const int y0 = std::clamp(r.y, 0, height);
    const int x1 = std::clamp(r.x + r.width, 0, width);
    const int y1 = std::clamp(r.y + r.height, 0, height);
    return {x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
  }

  /**
   * @brief Bookkeeping for a small pool of capture buffers that are reused from frame to frame.
   * @details A slot is in one of four states:
   *          - free: may be captured into;
   *          - capturing: attached to the frame the compositor is copying into;
   *          - ready: holds the newest completed frame, not yet handed to the encoder;
   *          - held: handed to the encoder through one or more images; it stays held while anyone but the
   *            capture image pool references one of those images (its use count is above 1), so it is never
   *            overwritten while the encoder may still read it.
   *
   *          It also tracks, per slot, the area that changed on screen since the slot was last captured
   *          (the damage to send with ext_image_copy_capture_frame_v1.damage_buffer).
   *          The owner of the pool allocates the actual buffers; this class only hands out indices.
   */
  class buffer_slots_t {
  public:
    enum class state_e {
      free,
      capturing,
      ready,
      held,
    };

    explicit buffer_slots_t(std::size_t max_slots = 3):
        max_slots {max_slots} {
    }

    struct acquired_t {
      std::size_t index;
      bool allocate;  ///< The slot is new: the caller must allocate its buffer.
    };

    /**
     * @brief Picks a slot to capture into and marks it as capturing.
     * @return A free slot (preferring the least recently captured one), a new slot if the pool may still
     *         grow, or std::nullopt if every slot is in use.
     */
    std::optional<acquired_t> acquire() {
      std::optional<std::size_t> best;
      for (std::size_t x = 0; x < slots.size(); ++x) {
        if (!is_free(x)) {
          continue;
        }
        if (!best || slots[x].last_capture < slots[*best].last_capture) {
          best = x;
        }
      }

      if (best) {
        slots[*best].state = state_e::capturing;
        slots[*best].holders.clear();
        return acquired_t {*best, false};
      }

      if (slots.size() < max_slots) {
        slots.emplace_back();
        slots.back().state = state_e::capturing;
        slots.back().damage_full = true;
        ++allocations;
        return acquired_t {slots.size() - 1, true};
      }

      return std::nullopt;
    }

    /**
     * @brief The capture into a slot completed. Its content is now current; every other slot is stale in
     *        the area `damage` covers.
     * @return The slot that was ready before (now free again), if any.
     */
    std::optional<std::size_t> completed(std::size_t index, const rect_t &damage) {
      for (std::size_t x = 0; x < slots.size(); ++x) {
        if (x != index) {
          slots[x].damage = bounding_union(slots[x].damage, damage);
        }
      }

      slots[index].damage = {};
      slots[index].damage_full = false;
      slots[index].last_capture = ++capture_counter;

      std::optional<std::size_t> previous = ready_slot();
      if (previous) {
        slots[*previous].state = state_e::free;
      }
      slots[index].state = state_e::ready;

      return previous;
    }

    /**
     * @brief The capture into a slot failed or was abandoned; its content is unknown.
     */
    void abandoned(std::size_t index) {
      slots[index].state = state_e::free;
      slots[index].damage_full = true;
      slots[index].holders.clear();
    }

    /**
     * @brief A ready or held slot was handed to the encoder through the image `holder`.
     * @details The image no longer holds any other slot, since it now shows this one. A held slot can be
     *          handed over again (to repeat a frame); it then stays held until all of its images are idle.
     */
    void hand_over(std::size_t index, const std::weak_ptr<void> &holder) {
      for (auto &slot : slots) {
        if (slot.state != state_e::held) {
          continue;
        }
        std::erase_if(slot.holders, [&](const auto &other) {
          return same_owner(other, holder);
        });
        if (slot.holders.empty()) {
          slot.state = state_e::free;
        }
      }
      slots[index].state = state_e::held;
      slots[index].holders.push_back(holder);
    }

    /**
     * @brief The ready slot was consumed right away (copied to system memory); it may be reused.
     */
    void consumed(std::size_t index) {
      slots[index].state = state_e::free;
      slots[index].holders.clear();
    }

    std::optional<std::size_t> ready_slot() const {
      for (std::size_t x = 0; x < slots.size(); ++x) {
        if (slots[x].state == state_e::ready) {
          return x;
        }
      }
      return std::nullopt;
    }

    /**
     * @brief The damage to send when capturing into a slot.
     * @return The accumulated damage, or std::nullopt if the whole buffer must be damaged.
     */
    std::optional<rect_t> damage_for(std::size_t index) const {
      if (slots[index].damage_full) {
        return std::nullopt;
      }
      return slots[index].damage;
    }

    bool is_free(std::size_t index) const {
      const auto &slot = slots[index];
      if (slot.state == state_e::free) {
        return true;
      }
      if (slot.state != state_e::held) {
        return false;
      }
      // The capture image pool keeps one reference to every image; anything above that is the encoder
      // (or the capture backend keeping the image to repeat it).
      return std::all_of(slot.holders.begin(), slot.holders.end(), [](const auto &holder) {
        return holder.use_count() <= 1;
      });
    }

    state_e state(std::size_t index) const {
      return slots[index].state;
    }

    std::size_t size() const {
      return slots.size();
    }

    /// Total number of slots ever created (i.e. buffer allocations requested).
    std::size_t allocations {0};

  private:
    static bool same_owner(const std::weak_ptr<void> &a, const std::weak_ptr<void> &b) {
      return !a.owner_before(b) && !b.owner_before(a);
    }

    struct slot_t {
      state_e state {state_e::free};
      std::vector<std::weak_ptr<void>> holders;
      rect_t damage;
      bool damage_full {true};
      std::uint64_t last_capture {0};
    };

    std::size_t max_slots;
    std::uint64_t capture_counter {0};
    std::vector<slot_t> slots;
  };

  /**
   * @brief Decides when a completed frame is handed to the encoder.
   * @details Frames are delivered as soon as they arrive, unless the previous delivery was less than
   *          90% of a frame interval ago (the output refreshes faster than the stream). Such a frame is held
   *          back and delivered one interval after the previous one, unless a newer frame replaces it first.
   */
  class frame_pacer_t {
  public:
    using clock = std::chrono::steady_clock;

    explicit frame_pacer_t(std::chrono::nanoseconds interval):
        interval {interval},
        min_interval {interval * 9 / 10} {
    }

    /// Whether a frame that is available at `now` may be delivered right away.
    bool may_deliver(clock::time_point now) const {
      return !last || now - *last >= min_interval;
    }

    /// When a held back frame must be delivered at the latest.
    clock::time_point deadline() const {
      return last ? *last + interval : clock::time_point::min();
    }

    void delivered(clock::time_point now) {
      last = now;
    }

    void reset() {
      last.reset();
    }

  private:
    std::chrono::nanoseconds interval;
    std::chrono::nanoseconds min_interval;
    std::optional<clock::time_point> last;
  };

  /**
   * @brief Copies an image row by row, optionally flipping it vertically.
   */
  inline void copy_rows(const std::uint8_t *src, std::size_t src_pitch, std::uint8_t *dst, std::size_t dst_pitch, std::size_t row_bytes, std::size_t rows, bool flip) {
    for (std::size_t y = 0; y < rows; ++y) {
      const auto src_row = flip ? rows - 1 - y : y;
      std::copy_n(src + src_row * src_pitch, row_bytes, dst + y * dst_pitch);
    }
  }
}  // namespace wl
