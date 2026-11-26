// Latency reactor (runs on the Apollo host).
// Covers one Wayland output with a layer-shell surface that flips between black and white, either on every
// key press (react mode takes the keyboard exclusively) or on a jittered timer, and logs Hyprland's presentation
// time for every flip.
//
// Usage: reactor --output NAME [--blink MS] [--animate] [--duration S]
// --animate redraws a 96x96 top-left square every frame (a game-like stream that never goes idle); flips are
// then drawn on the next frame callback, as a game loop would.
// Output lines (CLOCK_MONOTONIC ns):
//   P seq color trigger key_ms recv_ns commit_ns presented_ns refresh_ns flags
//   D seq color trigger key_ms recv_ns commit_ns            (frame discarded)
// trigger is "key" or "timer"; key_ms is the wl_keyboard event time (ms, 0 for timer flips).
// Esc quits.

#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "presentation-time-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

struct output {
  struct wl_output *wl;
  char name[64];
  struct output *next;
};

struct flip_ctx {
  uint64_t seq;
  int color;
  const char *trigger;
  uint32_t key_ms;
  uint64_t recv_ns;
  uint64_t commit_ns;
};

static struct {
  struct wl_display *display;
  struct wl_compositor *compositor;
  struct wl_shm *shm;
  struct wl_seat *seat;
  struct wl_keyboard *keyboard;
  struct zwlr_layer_shell_v1 *layer_shell;
  struct wp_presentation *presentation;
  struct output *outputs;
  struct wl_surface *surface;
  struct zwlr_layer_surface_v1 *layer_surface;
  struct wl_buffer *buffers[4];  // [color * 2 + tick]
  bool animate;
  int tick;
  struct flip_ctx *pending;  // animate mode: drawn on the next frame callback
  uint64_t frames;
  uint64_t first_frame_ns, last_frame_ns;
  int width, height;
  bool configured;
  int color;
  uint64_t seq;
  uint32_t pres_clock;
} st;

static volatile sig_atomic_t running = 1;

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec;
}

static void on_signal(int sig) {
  (void) sig;
  running = 0;
}

// ---- presentation feedback ----
static void fb_sync_output(void *data, struct wp_presentation_feedback *fb, struct wl_output *out) {
  (void) data;
  (void) fb;
  (void) out;
}

static void fb_presented(void *data, struct wp_presentation_feedback *fb, uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec, uint32_t refresh, uint32_t seq_hi, uint32_t seq_lo, uint32_t flags) {
  (void) seq_hi;
  (void) seq_lo;
  struct flip_ctx *c = data;
  uint64_t pres = (((uint64_t) sec_hi << 32) | sec_lo) * 1000000000ull + nsec;
  printf("P %lu %d %s %u %lu %lu %lu %u %u\n", c->seq, c->color, c->trigger, c->key_ms, c->recv_ns, c->commit_ns, pres, refresh, flags);
  free(c);
  wp_presentation_feedback_destroy(fb);
}

static void fb_discarded(void *data, struct wp_presentation_feedback *fb) {
  struct flip_ctx *c = data;
  printf("D %lu %d %s %u %lu %lu\n", c->seq, c->color, c->trigger, c->key_ms, c->recv_ns, c->commit_ns);
  free(c);
  wp_presentation_feedback_destroy(fb);
}

static const struct wp_presentation_feedback_listener fb_listener = {
  .sync_output = fb_sync_output,
  .presented = fb_presented,
  .discarded = fb_discarded,
};

static void flip(const char *trigger, uint32_t key_ms, uint64_t recv_ns) {
  if (!st.configured) {
    return;
  }
  st.color ^= 1;
  struct flip_ctx *c = calloc(1, sizeof(*c));
  c->seq = ++st.seq;
  c->color = st.color;
  c->trigger = trigger;
  c->key_ms = key_ms;
  c->recv_ns = recv_ns;
  if (st.animate) {
    // Like a game loop: the change is drawn with the next frame.
    if (st.pending) {
      printf("D %lu %d %s %u %lu 0\n", st.pending->seq, st.pending->color, st.pending->trigger, st.pending->key_ms, st.pending->recv_ns);
      free(st.pending);
    }
    st.pending = c;
    return;
  }
  wl_surface_attach(st.surface, st.buffers[st.color * 2], 0, 0);
  wl_surface_damage_buffer(st.surface, 0, 0, INT32_MAX, INT32_MAX);
  struct wp_presentation_feedback *fb = wp_presentation_feedback(st.presentation, st.surface);
  wp_presentation_feedback_add_listener(fb, &fb_listener, c);
  c->commit_ns = now_ns();
  wl_surface_commit(st.surface);
  wl_display_flush(st.display);
}

// ---- buffers ----
#define CORNER 96

static bool create_buffers(void) {
  size_t frame = (size_t) st.width * st.height * 4;
  int fd = memfd_create("apollo-latency-reactor", MFD_CLOEXEC);
  if (fd < 0 || ftruncate(fd, (off_t) frame * 4) < 0) {
    perror("memfd");
    return false;
  }
  uint32_t *px = mmap(NULL, frame * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (px == MAP_FAILED) {
    perror("mmap");
    return false;
  }
  // Black or white, with a top-left square that alternates between dark and light gray per animation tick.
  size_t n = (size_t) st.width * st.height;
  for (int b = 0; b < 4; ++b) {
    uint32_t base = (b / 2) ? 0xffffffffu : 0xff000000u;
    uint32_t corner = (b % 2) ? 0xffc0c0c0u : 0xff404040u;
    uint32_t *buf = px + n * (size_t) b;
    for (int y = 0; y < st.height; ++y) {
      for (int x = 0; x < st.width; ++x) {
        buf[(size_t) y * st.width + x] = (x < CORNER && y < CORNER) ? corner : base;
      }
    }
  }
  munmap(px, frame * 4);
  struct wl_shm_pool *pool = wl_shm_create_pool(st.shm, fd, (int32_t) (frame * 4));
  for (int i = 0; i < 4; ++i) {
    st.buffers[i] = wl_shm_pool_create_buffer(pool, (int32_t) (frame * i), st.width, st.height, st.width * 4, WL_SHM_FORMAT_XRGB8888);
  }
  wl_shm_pool_destroy(pool);
  close(fd);
  return true;
}

// ---- animate mode: redraw every frame ----
static const struct wl_callback_listener frame_listener;

static void draw_frame(void) {
  st.tick ^= 1;
  wl_surface_attach(st.surface, st.buffers[st.color * 2 + st.tick], 0, 0);
  if (st.pending) {
    wl_surface_damage_buffer(st.surface, 0, 0, INT32_MAX, INT32_MAX);
  } else {
    wl_surface_damage_buffer(st.surface, 0, 0, CORNER, CORNER);
  }
  struct wl_callback *cb = wl_surface_frame(st.surface);
  wl_callback_add_listener(cb, &frame_listener, NULL);
  if (st.pending) {
    struct wp_presentation_feedback *fb = wp_presentation_feedback(st.presentation, st.surface);
    wp_presentation_feedback_add_listener(fb, &fb_listener, st.pending);
    st.pending->commit_ns = now_ns();
    st.pending = NULL;
  }
  wl_surface_commit(st.surface);
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
  (void) data;
  (void) time;
  wl_callback_destroy(cb);
  if (!running) {
    return;  // shutting down: the surface is about to go away
  }
  uint64_t now = now_ns();
  if (!st.first_frame_ns) {
    st.first_frame_ns = now;
  }
  st.last_frame_ns = now;
  ++st.frames;
  draw_frame();
}

static const struct wl_callback_listener frame_listener = {.done = frame_done};

// ---- layer surface ----
static void ls_configure(void *data, struct zwlr_layer_surface_v1 *ls, uint32_t serial, uint32_t w, uint32_t h) {
  (void) data;
  zwlr_layer_surface_v1_ack_configure(ls, serial);
  if (st.configured) {
    wl_surface_commit(st.surface);
    return;
  }
  st.width = (int) w;
  st.height = (int) h;
  if (!create_buffers()) {
    running = 0;
    return;
  }
  st.configured = true;
  st.color = 0;
  printf("# configured %dx%d\n", st.width, st.height);
  if (st.animate) {
    st.tick = 1;  // draw_frame() flips it back to 0
    wl_surface_damage_buffer(st.surface, 0, 0, INT32_MAX, INT32_MAX);
    draw_frame();
    return;
  }
  wl_surface_attach(st.surface, st.buffers[0], 0, 0);
  wl_surface_damage_buffer(st.surface, 0, 0, INT32_MAX, INT32_MAX);
  wl_surface_commit(st.surface);
}

static void ls_closed(void *data, struct zwlr_layer_surface_v1 *ls) {
  (void) data;
  (void) ls;
  running = 0;
}

static const struct zwlr_layer_surface_v1_listener ls_listener = {
  .configure = ls_configure,
  .closed = ls_closed,
};

// ---- keyboard ----
static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t fmt, int32_t fd, uint32_t size) {
  (void) d;
  (void) k;
  (void) fmt;
  (void) size;
  close(fd);
}

static void kb_enter(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s, struct wl_array *keys) {
  (void) d;
  (void) k;
  (void) serial;
  (void) s;
  (void) keys;
  printf("# keyboard focus\n");
}

static void kb_leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s) {
  (void) d;
  (void) k;
  (void) serial;
  (void) s;
  printf("# keyboard focus lost\n");
}

static void kb_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
  (void) d;
  (void) k;
  (void) serial;
  uint64_t recv = now_ns();
  if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
    return;
  }
  if (key == 1) {  // KEY_ESC
    running = 0;
    return;
  }
  flip("key", time, recv);
}

static void kb_modifiers(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group) {
  (void) d;
  (void) k;
  (void) serial;
  (void) dep;
  (void) lat;
  (void) lock;
  (void) group;
}

static void kb_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay) {
  (void) d;
  (void) k;
  (void) rate;
  (void) delay;
}

static const struct wl_keyboard_listener kb_listener = {
  .keymap = kb_keymap,
  .enter = kb_enter,
  .leave = kb_leave,
  .key = kb_key,
  .modifiers = kb_modifiers,
  .repeat_info = kb_repeat,
};

static void seat_caps(void *d, struct wl_seat *seat, uint32_t caps) {
  (void) d;
  if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !st.keyboard) {
    st.keyboard = wl_seat_get_keyboard(seat);
    wl_keyboard_add_listener(st.keyboard, &kb_listener, NULL);
  }
}

static void seat_name(void *d, struct wl_seat *s, const char *name) {
  (void) d;
  (void) s;
  (void) name;
}

static const struct wl_seat_listener seat_listener = {.capabilities = seat_caps, .name = seat_name};

// ---- outputs ----
static void out_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph, int32_t sp, const char *mk, const char *md, int32_t tr) {
  (void) d;
  (void) o;
  (void) x;
  (void) y;
  (void) pw;
  (void) ph;
  (void) sp;
  (void) mk;
  (void) md;
  (void) tr;
}

static void out_mode(void *d, struct wl_output *o, uint32_t f, int32_t w, int32_t h, int32_t r) {
  (void) d;
  (void) o;
  (void) f;
  (void) w;
  (void) h;
  (void) r;
}

static void out_done(void *d, struct wl_output *o) {
  (void) d;
  (void) o;
}

static void out_scale(void *d, struct wl_output *o, int32_t s) {
  (void) d;
  (void) o;
  (void) s;
}

static void out_name(void *d, struct wl_output *o, const char *name) {
  (void) o;
  struct output *out = d;
  snprintf(out->name, sizeof(out->name), "%s", name);
}

static void out_desc(void *d, struct wl_output *o, const char *desc) {
  (void) d;
  (void) o;
  (void) desc;
}

static const struct wl_output_listener out_listener = {
  .geometry = out_geometry,
  .mode = out_mode,
  .done = out_done,
  .scale = out_scale,
  .name = out_name,
  .description = out_desc,
};

static void pres_clock_id(void *d, struct wp_presentation *p, uint32_t clk) {
  (void) d;
  (void) p;
  st.pres_clock = clk;
}

static const struct wp_presentation_listener pres_listener = {.clock_id = pres_clock_id};

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version) {
  (void) d;
#define MIN(a, b) ((a) < (b) ? (a) : (b))
  if (!strcmp(iface, wl_compositor_interface.name)) {
    st.compositor = wl_registry_bind(r, name, &wl_compositor_interface, MIN(version, 4));
  } else if (!strcmp(iface, wl_shm_interface.name)) {
    st.shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
  } else if (!strcmp(iface, wl_seat_interface.name) && !st.seat) {
    st.seat = wl_registry_bind(r, name, &wl_seat_interface, MIN(version, 5));
    wl_seat_add_listener(st.seat, &seat_listener, NULL);
  } else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name)) {
    st.layer_shell = wl_registry_bind(r, name, &zwlr_layer_shell_v1_interface, MIN(version, 4));
  } else if (!strcmp(iface, wp_presentation_interface.name)) {
    st.presentation = wl_registry_bind(r, name, &wp_presentation_interface, 1);
    wp_presentation_add_listener(st.presentation, &pres_listener, NULL);
  } else if (!strcmp(iface, wl_output_interface.name) && version >= 4) {
    struct output *o = calloc(1, sizeof(*o));
    o->wl = wl_registry_bind(r, name, &wl_output_interface, 4);
    wl_output_add_listener(o->wl, &out_listener, o);
    o->next = st.outputs;
    st.outputs = o;
  }
}

static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {
  (void) d;
  (void) r;
  (void) name;
}

static const struct wl_registry_listener reg_listener = {.global = reg_global, .global_remove = reg_remove};

static int arm_jittered(int fd, int base_ms) {
  int ms = base_ms + (base_ms > 0 ? rand() % (base_ms / 2 + 1) : 0);
  struct itimerspec its = {.it_value = {.tv_sec = ms / 1000, .tv_nsec = (long) (ms % 1000) * 1000000L}};
  return timerfd_settime(fd, 0, &its, NULL);
}

int main(int argc, char **argv) {
  const char *output_name = NULL;
  int blink_ms = 0;
  int duration_s = 0;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--output") && i + 1 < argc) {
      output_name = argv[++i];
    } else if (!strcmp(argv[i], "--blink") && i + 1 < argc) {
      blink_ms = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--animate")) {
      st.animate = true;
    } else if (!strcmp(argv[i], "--duration") && i + 1 < argc) {
      duration_s = atoi(argv[++i]);
    } else {
      fprintf(stderr, "usage: %s --output NAME [--blink MS] [--animate] [--duration S]\n", argv[0]);
      return 2;
    }
  }
  if (!output_name) {
    fprintf(stderr, "--output is required\n");
    return 2;
  }
  setvbuf(stdout, NULL, _IOLBF, 0);
  srand((unsigned) now_ns());
  struct sigaction sa = {.sa_handler = on_signal};
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);

  st.display = wl_display_connect(NULL);
  if (!st.display) {
    fprintf(stderr, "cannot connect to Wayland\n");
    return 1;
  }
  struct wl_registry *reg = wl_display_get_registry(st.display);
  wl_registry_add_listener(reg, &reg_listener, NULL);
  wl_display_roundtrip(st.display);
  wl_display_roundtrip(st.display);
  if (!st.compositor || !st.shm || !st.layer_shell || !st.presentation || !st.seat) {
    fprintf(stderr, "missing globals (compositor %p shm %p layer_shell %p presentation %p seat %p)\n", (void *) st.compositor, (void *) st.shm, (void *) st.layer_shell, (void *) st.presentation, (void *) st.seat);
    return 1;
  }
  // analyze.py compares presentation times with CLOCK_MONOTONIC input times and clock offsets.
  if (st.pres_clock != CLOCK_MONOTONIC) {
    fprintf(stderr, "presentation clock %u is not CLOCK_MONOTONIC; timings would not be comparable\n", st.pres_clock);
    return 1;
  }
  struct output *target = NULL;
  for (struct output *o = st.outputs; o; o = o->next) {
    if (!strcmp(o->name, output_name)) {
      target = o;
    }
  }
  if (!target) {
    fprintf(stderr, "output %s not found\n", output_name);
    return 1;
  }
  printf("# reactor output=%s presentation_clock=%u blink_ms=%d animate=%d\n", output_name, st.pres_clock, blink_ms, st.animate);

  st.surface = wl_compositor_create_surface(st.compositor);
  st.layer_surface = zwlr_layer_shell_v1_get_layer_surface(st.layer_shell, st.surface, target->wl, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "apollo-latency");
  zwlr_layer_surface_v1_add_listener(st.layer_surface, &ls_listener, NULL);
  zwlr_layer_surface_v1_set_anchor(st.layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
  zwlr_layer_surface_v1_set_size(st.layer_surface, 0, 0);
  zwlr_layer_surface_v1_set_exclusive_zone(st.layer_surface, -1);
  // Only react mode needs keys; blink mode leaves the keyboard alone.
  zwlr_layer_surface_v1_set_keyboard_interactivity(st.layer_surface, blink_ms > 0 ? ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE : ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
  wl_surface_commit(st.surface);

  int blink_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
  int end_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
  if (blink_ms > 0) {
    arm_jittered(blink_fd, blink_ms);
  }
  if (duration_s > 0) {
    struct itimerspec its = {.it_value = {.tv_sec = duration_s}};
    timerfd_settime(end_fd, 0, &its, NULL);
  }

  while (running) {
    while (wl_display_prepare_read(st.display) != 0) {
      wl_display_dispatch_pending(st.display);
    }
    wl_display_flush(st.display);
    struct pollfd fds[3] = {{wl_display_get_fd(st.display), POLLIN, 0}, {blink_fd, POLLIN, 0}, {end_fd, POLLIN, 0}};
    if (poll(fds, 3, -1) < 0) {
      wl_display_cancel_read(st.display);
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (fds[0].revents & POLLIN) {
      if (wl_display_read_events(st.display) < 0) {
        break;
      }
    } else {
      wl_display_cancel_read(st.display);
    }
    if (wl_display_dispatch_pending(st.display) < 0) {
      break;
    }
    if (fds[1].revents & POLLIN) {
      uint64_t n;
      if (read(blink_fd, &n, sizeof(n)) == sizeof(n)) {
        flip("timer", 0, now_ns());
      }
      arm_jittered(blink_fd, blink_ms);
    }
    if (fds[2].revents & POLLIN) {
      running = 0;
    }
  }
  // Collect feedback for the last flips.
  wl_display_roundtrip(st.display);
  printf("# done flips=%lu\n", st.seq);
  if (st.frames > 1) {
    printf("# animate frames=%lu rate=%.2f/s\n", st.frames, (double) (st.frames - 1) * 1e9 / (double) (st.last_frame_ns - st.first_frame_ns));
  }
  zwlr_layer_surface_v1_destroy(st.layer_surface);
  wl_surface_destroy(st.surface);
  wl_display_roundtrip(st.display);
  wl_display_disconnect(st.display);
  return 0;
}
