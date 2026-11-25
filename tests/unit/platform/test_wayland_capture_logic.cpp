/**
 * @file tests/unit/platform/test_wayland_capture_logic.cpp
 * @brief Test src/platform/linux/wayland_capture_logic.h.
 */
#include "../../tests_common.h"

#include <src/platform/linux/wayland_capture_logic.h>

using namespace std::literals;

namespace {
  // Modifiers Hyprland 0.57 advertises on an RTX 5090 (NVIDIA block-linear layouts plus the implicit modifier).
  const std::vector<std::uint64_t> nvidia_modifiers {
    0x300000000606010,
    0x300000000606011,
    0x300000000e08010,
    wl::fourcc::MOD_INVALID,
  };

  // What Hyprland 0.57 sends for a 3840x2160 output.
  wl::buffer_constraints_t hyprland_constraints() {
    wl::buffer_constraints_t c;
    c.width = 3840;
    c.height = 2160;
    c.shm_formats = {wl::shm_format::ARGB8888, wl::shm_format::XRGB8888};
    c.dmabuf_device = 0xe280;  // 226:128
    c.dmabuf_formats = {
      {wl::fourcc::ARGB8888, nvidia_modifiers},
      {wl::fourcc::XRGB8888, nvidia_modifiers},
    };
    return c;
  }
}  // namespace

// Bugs caught: 0, False positives: 0
TEST(WaylandCaptureLogic, PrefersOpaqueDmabufAndSplitsOutImplicitModifier) {
  auto spec = wl::choose_buffer_spec(hyprland_constraints(), true, true);
  ASSERT_TRUE(spec);
  EXPECT_FALSE(spec->shm);
  EXPECT_EQ(spec->format, wl::fourcc::XRGB8888);
  EXPECT_EQ(spec->width, 3840u);
  EXPECT_EQ(spec->height, 2160u);
  EXPECT_TRUE(spec->implicit_modifier);
  EXPECT_EQ(spec->modifiers, (std::vector<std::uint64_t> {0x300000000606010, 0x300000000606011, 0x300000000e08010}));
}

// Bugs caught: 0, False positives: 0
TEST(WaylandCaptureLogic, ExplicitModifiersOnlyWhenInvalidIsNotListed) {
  auto c = hyprland_constraints();
  c.dmabuf_formats = {{wl::fourcc::XRGB8888, {0x300000000606010}}};
  auto spec = wl::choose_buffer_spec(c, true, false);
  ASSERT_TRUE(spec);
  EXPECT_FALSE(spec->implicit_modifier);
  EXPECT_EQ(spec->modifiers.size(), 1u);

  // No modifier list at all: only an implicit modifier is possible.
  c.dmabuf_formats = {{wl::fourcc::XRGB8888, {}}};
  spec = wl::choose_buffer_spec(c, true, false);
  ASSERT_TRUE(spec);
  EXPECT_TRUE(spec->implicit_modifier);
  EXPECT_TRUE(spec->modifiers.empty());
}

// Bugs caught: 0, False positives: 0
TEST(WaylandCaptureLogic, FallsBackToShmOnlyWhenAllowed) {
  auto spec = wl::choose_buffer_spec(hyprland_constraints(), false, true);
  ASSERT_TRUE(spec);
  EXPECT_TRUE(spec->shm);
  EXPECT_EQ(spec->format, wl::shm_format::XRGB8888);

  // Video memory capture cannot use shm.
  EXPECT_FALSE(wl::choose_buffer_spec(hyprland_constraints(), false, false));

  // prefer_shm picks shm even when dma-bufs are possible.
  spec = wl::choose_buffer_spec(hyprland_constraints(), true, true, true);
  ASSERT_TRUE(spec);
  EXPECT_TRUE(spec->shm);

  // ...but still uses dma-bufs when shm is not allowed.
  spec = wl::choose_buffer_spec(hyprland_constraints(), true, false, true);
  ASSERT_TRUE(spec);
  EXPECT_FALSE(spec->shm);
}

// Bugs caught: 0, False positives: 0
TEST(WaylandCaptureLogic, RejectsUnsupportedFormatsAndMissingSize) {
  auto c = hyprland_constraints();
  // FP16 only (AB4H) and an RGB565 shm format.
  c.dmabuf_formats = {{wl::fourcc::code('A', 'B', '4', 'H'), nvidia_modifiers}};
  c.shm_formats = {wl::fourcc::code('R', 'G', '1', '6')};
  EXPECT_FALSE(wl::choose_buffer_spec(c, true, true));

  // 10-bit shm is not accepted: the system memory path hands frames on as BGR0.
  c.dmabuf_formats.clear();
  c.shm_formats = {wl::fourcc::XRGB2101010};
  EXPECT_FALSE(wl::choose_buffer_spec(c, true, true));

  c = hyprland_constraints();
  c.width = 0;
  EXPECT_FALSE(wl::choose_buffer_spec(c, true, true));

  // RGBA byte order works for dma-bufs (the EGL import handles it), but never for shm.
  c = hyprland_constraints();
  c.dmabuf_formats = {{wl::fourcc::ABGR8888, {}}};
  c.shm_formats = {wl::fourcc::ABGR8888};
  auto spec = wl::choose_buffer_spec(c, true, true);
  ASSERT_TRUE(spec);
  EXPECT_EQ(spec->format, wl::fourcc::ABGR8888);
  EXPECT_FALSE(wl::choose_buffer_spec(c, false, true));
}

// Bugs caught: 0, False positives: 0
TEST(WaylandCaptureLogic, ConstraintChangesAreDetected) {
  auto a = hyprland_constraints();
  auto b = hyprland_constraints();
  EXPECT_EQ(a, b);
  b.width = 1920;
  EXPECT_NE(a, b);
  b = hyprland_constraints();
  b.dmabuf_formats[0].modifiers.pop_back();
  EXPECT_NE(a, b);
}

// Bugs caught: 0, False positives: 0
TEST(WaylandCaptureLogic, PresentationTimeMapsToSteadyClock) {
  timespec ts {};
  ASSERT_EQ(clock_gettime(CLOCK_MONOTONIC, &ts), 0);
  const auto now = std::chrono::steady_clock::now();

  const std::uint64_t sec = ts.tv_sec;
  auto t = wl::presentation_time_to_steady(sec >> 32, sec & 0xffffffff, (std::uint32_t) ts.tv_nsec, now);
  ASSERT_TRUE(t);
  // steady_clock is CLOCK_MONOTONIC, so the converted time is within a few ms of now().
  EXPECT_LT(std::chrono::abs(*t - now), 10ms);
  EXPECT_EQ(t->time_since_epoch(), std::chrono::seconds(ts.tv_sec) + std::chrono::nanoseconds(ts.tv_nsec));

  // A frame presented 8 ms ago.
  auto earlier = wl::presentation_time_to_steady(sec >> 32, sec & 0xffffffff, (std::uint32_t) ts.tv_nsec, now + 8ms);
  ASSERT_TRUE(earlier);
  EXPECT_EQ(*earlier, *t);
}

// Bugs caught: 0, False positives: 0
TEST(WaylandCaptureLogic, InvalidPresentationTimesAreRejected) {
  const auto now = std::chrono::steady_clock::now();
  const std::uint64_t sec = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

  // tv_nsec out of range.
  EXPECT_FALSE(wl::presentation_time_to_steady(sec >> 32, sec & 0xffffffff, 1'000'000'000u, now));
  // All zero (no timestamp).
  EXPECT_FALSE(wl::presentation_time_to_steady(0, 0, 0, now));
  // A different clock (e.g. CLOCK_REALTIME, decades away).
  const std::uint64_t realtime = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  EXPECT_FALSE(wl::presentation_time_to_steady(realtime >> 32, realtime & 0xffffffff, 0, now));
  // Huge seconds must not overflow.
  EXPECT_FALSE(wl::presentation_time_to_steady(0xffffffff, 0xffffffff, 0, now));
}

// Bugs caught: 0, False positives: 0
TEST(WaylandCaptureLogic, DamageRectangles) {
  EXPECT_EQ(wl::bounding_union({}, {10, 20, 30, 40}), (wl::rect_t {10, 20, 30, 40}));
  EXPECT_EQ(wl::bounding_union({0, 0, 10, 10}, {20, 20, 10, 10}), (wl::rect_t {0, 0, 30, 30}));
  EXPECT_EQ(wl::clip({-10, -10, 30, 30}, 100, 100), (wl::rect_t {0, 0, 20, 20}));
  EXPECT_TRUE(wl::clip({200, 200, 30, 30}, 100, 100).empty());
}

// Bugs caught: 0, False positives: 0
TEST(WaylandCaptureLogic, SystemMemoryPoolReusesTwoBuffers) {
  wl::buffer_slots_t slots {2};

  // Frame 1 into slot 0 (allocated), frame 2 into slot 1 (allocated) while 0 waits for the pacer.
  auto a = slots.acquire();
  ASSERT_TRUE(a);
  EXPECT_TRUE(a->allocate);
  EXPECT_FALSE(slots.completed(a->index, {0, 0, 100, 100}));

  auto b = slots.acquire();
  ASSERT_TRUE(b);
  EXPECT_TRUE(b->allocate);
  EXPECT_NE(a->index, b->index);
  EXPECT_FALSE(slots.acquire()) << "both slots are in use";

  // Frame 2 replaces frame 1, which is dropped and becomes free again.
  EXPECT_EQ(slots.completed(b->index, {0, 0, 10, 10}), a->index);
  EXPECT_EQ(slots.ready_slot(), b->index);

  // Many more frames: copied out right away, never another allocation.
  for (int x = 0; x < 100; ++x) {
    auto next = slots.acquire();
    ASSERT_TRUE(next);
    EXPECT_FALSE(next->allocate);
    slots.completed(next->index, {});
    slots.consumed(*slots.ready_slot());
  }
  EXPECT_EQ(slots.allocations, 2u);
  EXPECT_EQ(slots.size(), 2u);
}

// Bugs caught: 0, False positives: 0
TEST(WaylandCaptureLogic, HeldSlotsAreNotReusedWhileTheEncoderHoldsTheImage) {
  wl::buffer_slots_t slots {3};

  // The capture image pool keeps one reference to every image.
  auto img1 = std::make_shared<int>(1);
  auto img2 = std::make_shared<int>(2);

  auto a = slots.acquire();
  slots.completed(a->index, {});
  slots.hand_over(a->index, img1);
  // The encoder holds img1.
  auto encoder_ref = img1;

  auto b = slots.acquire();
  ASSERT_TRUE(b);
  EXPECT_NE(b->index, a->index);
  slots.completed(b->index, {});
  slots.hand_over(b->index, img2);
  auto encoder_ref2 = img2;

  auto c = slots.acquire();
  ASSERT_TRUE(c);
  EXPECT_TRUE(c->allocate);
  slots.completed(c->index, {});

  // The encoder is done with img2 (only the image pool has it), so its slot is free; img1 is still being encoded.
  EXPECT_FALSE(slots.is_free(b->index));
  encoder_ref2.reset();
  EXPECT_TRUE(slots.is_free(b->index));
  EXPECT_FALSE(slots.is_free(a->index));
  auto d = slots.acquire();
  ASSERT_TRUE(d);
  EXPECT_EQ(d->index, b->index);
  EXPECT_FALSE(d->allocate);

  // All three are now in use (held, ready, capturing).
  EXPECT_FALSE(slots.acquire());

  // The encoder lets go of img1.
  encoder_ref.reset();
  auto e = slots.acquire();
  ASSERT_TRUE(e);
  EXPECT_EQ(e->index, a->index);
  EXPECT_EQ(slots.allocations, 3u);
}

// Bugs caught: 0, False positives: 0
TEST(WaylandCaptureLogic, ReusedImageReleasesItsPreviousSlot) {
  wl::buffer_slots_t slots {2};
  auto img = std::make_shared<int>(0);
  auto encoder_ref = img;

  auto a = slots.acquire();
  slots.completed(a->index, {});
  slots.hand_over(a->index, img);

  auto b = slots.acquire();
  slots.completed(b->index, {});
  // The same image now shows slot b, so slot a is free even though the image is busy.
  slots.hand_over(b->index, img);
  EXPECT_TRUE(slots.is_free(a->index));
  EXPECT_FALSE(slots.is_free(b->index));
}

// Bugs caught: 0, False positives: 0
TEST(WaylandCaptureLogic, RepeatedSlotStaysHeldUntilEveryImageIsIdle) {
  wl::buffer_slots_t slots {2};
  auto first = std::make_shared<int>(0);
  auto repeat = std::make_shared<int>(1);
  auto first_ref = first;
  auto repeat_ref = repeat;

  auto a = slots.acquire();
  slots.completed(a->index, {});
  slots.hand_over(a->index, first);
  slots.hand_over(a->index, repeat);

  first_ref.reset();
  EXPECT_FALSE(slots.is_free(a->index)) << "the repeat is still being encoded";
  repeat_ref.reset();
  EXPECT_TRUE(slots.is_free(a->index));
}

// Bugs caught: 0, False positives: 0
TEST(WaylandCaptureLogic, DamageAccumulatesPerSlot) {
  wl::buffer_slots_t slots {2};

  // Frame a waits for the pacer while frame b is captured into a second buffer.
  auto a = slots.acquire();
  EXPECT_FALSE(slots.damage_for(a->index)) << "a new buffer needs full damage";
  slots.completed(a->index, {0, 0, 100, 100});

  auto b = slots.acquire();
  EXPECT_NE(b->index, a->index);
  EXPECT_FALSE(slots.damage_for(b->index));
  // Frame b replaces frame a, which becomes free.
  EXPECT_EQ(slots.completed(b->index, {10, 10, 5, 5}), a->index);

  // Slot a missed frame b's damage.
  auto c = slots.acquire();
  ASSERT_EQ(c->index, a->index);
  ASSERT_TRUE(slots.damage_for(c->index));
  EXPECT_EQ(*slots.damage_for(c->index), (wl::rect_t {10, 10, 5, 5}));
  EXPECT_EQ(slots.completed(c->index, {50, 50, 10, 10}), b->index);

  // Slot b missed frame c's damage only.
  auto d = slots.acquire();
  ASSERT_EQ(d->index, b->index);
  EXPECT_EQ(*slots.damage_for(d->index), (wl::rect_t {50, 50, 10, 10}));

  // An abandoned capture leaves the buffer content unknown.
  slots.abandoned(d->index);
  EXPECT_FALSE(slots.damage_for(d->index));
}

// Bugs caught: 0, False positives: 0
TEST(WaylandCaptureLogic, PacerDeliversAtMostAtTheStreamRate) {
  using clock = std::chrono::steady_clock;
  const auto interval = std::chrono::nanoseconds {1s} / 120;
  wl::frame_pacer_t pacer {interval};

  auto t0 = clock::now();
  EXPECT_TRUE(pacer.may_deliver(t0));
  pacer.delivered(t0);

  // A frame from a 144 Hz output arrives early: held back until one interval after the last delivery.
  EXPECT_FALSE(pacer.may_deliver(t0 + 6940us));
  EXPECT_EQ(pacer.deadline(), t0 + interval);

  // A frame that arrives slightly early (jitter) at the stream rate goes out right away.
  EXPECT_TRUE(pacer.may_deliver(t0 + interval - 500us));

  // Simulate a 144 Hz output streamed at 120 fps for one second.
  pacer.reset();
  int delivered = 0;
  std::optional<clock::time_point> held;
  const auto frame = std::chrono::nanoseconds {1s} / 144;
  auto next_arrival = t0;
  for (auto now = t0; now < t0 + 1s; now += 100us) {
    if (held && now >= pacer.deadline()) {
      pacer.delivered(now);
      held.reset();
      ++delivered;
    }
    if (now >= next_arrival) {
      next_arrival += frame;
      if (pacer.may_deliver(now)) {
        pacer.delivered(now);
        held.reset();
        ++delivered;
      } else {
        held = now;
      }
    }
  }
  EXPECT_GE(delivered, 118);
  EXPECT_LE(delivered, 121);
}

// Bugs caught: 0, False positives: 0
TEST(WaylandCaptureLogic, CopyRowsFlipsVertically) {
  // 2x3 image, 1 byte per pixel, source pitch 4.
  const std::uint8_t src[] = {1, 2, 0, 0, 3, 4, 0, 0, 5, 6, 0, 0};
  std::uint8_t dst[6] {};
  wl::copy_rows(src, 4, dst, 2, 2, 3, false);
  EXPECT_EQ(std::vector<std::uint8_t>(dst, dst + 6), (std::vector<std::uint8_t> {1, 2, 3, 4, 5, 6}));
  wl::copy_rows(src, 4, dst, 2, 2, 3, true);
  EXPECT_EQ(std::vector<std::uint8_t>(dst, dst + 6), (std::vector<std::uint8_t> {5, 6, 3, 4, 1, 2}));
}

// Bugs caught: 0, False positives: 0
TEST(WaylandCaptureLogic, PrefersTenBitDmabufsOnlyWhenAsked) {
  // A 10-bit output with misc.screencopy_force_8b = false: Hyprland offers its 10-bit format.
  auto c = hyprland_constraints();
  c.dmabuf_formats.insert(c.dmabuf_formats.begin(), {wl::fourcc::XBGR2101010, nvidia_modifiers});

  auto spec = wl::choose_buffer_spec(c, true, false, false, true);
  ASSERT_TRUE(spec);
  EXPECT_EQ(spec->format, wl::fourcc::XBGR2101010);

  spec = wl::choose_buffer_spec(c, true, false, false, false);
  ASSERT_TRUE(spec);
  EXPECT_EQ(spec->format, wl::fourcc::XRGB8888) << "SDR keeps 8-bit buffers";

  // Only 10-bit offered: used either way.
  c.dmabuf_formats = {{wl::fourcc::XRGB2101010, nvidia_modifiers}};
  spec = wl::choose_buffer_spec(c, true, false);
  ASSERT_TRUE(spec);
  EXPECT_EQ(spec->format, wl::fourcc::XRGB2101010);

  // Only 8-bit offered (Hyprland's default misc.screencopy_force_8b = true): 8-bit it is.
  spec = wl::choose_buffer_spec(hyprland_constraints(), true, false, false, true);
  ASSERT_TRUE(spec);
  EXPECT_EQ(spec->format, wl::fourcc::XRGB8888);
}
