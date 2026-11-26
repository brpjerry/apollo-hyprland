// Latency probe (runs on the client machine).
// Repeatedly captures a small region of one output with wlr-screencopy and logs its mean brightness with the
// compositor's frame timestamp. Optionally presses a key through its own uinput keyboard at jittered intervals
// (the streaming client must have keyboard focus).
//
// Usage: probe --output NAME --region X,Y,W,H [--duration S] [--press MIN_MS,MAX_MS] [--key CODE] [--inflight N] [--immediate]
// --inflight keeps N capture requests outstanding (each with its own buffer) so one is pending at every repaint;
// --immediate uses copy instead of copy_with_damage. Lines may then come out of timestamp order.
// Output lines (CLOCK_MONOTONIC ns):
//   F ready_ns recv_ns mean      (one captured frame; mean is 0..255)
//   K press_ns                   (key went down)
//   X recv_ns                    (capture failed)

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "wlr-screencopy-unstable-v1-client-protocol.h"

struct output {
  struct wl_output *wl;
  char name[64];
  struct output *next;
};

static struct {
  struct wl_display *display;
  struct wl_shm *shm;
  struct zwlr_screencopy_manager_v1 *screencopy;
  uint32_t screencopy_version;
  struct output *outputs;
  struct output *target;
  int rx, ry, rw, rh;
  int inflight;  // capture requests kept outstanding
  bool immediate;  // use copy instead of copy_with_damage
  uint64_t frames, failures;
} st;

#define MAX_INFLIGHT 8

// One outstanding capture request with its own buffer.
struct slot {
  struct zwlr_screencopy_frame_v1 *frame;
  struct wl_buffer *buffer;
  uint8_t *pixels;
  uint32_t buf_w, buf_h, buf_stride, buf_format;
  uint32_t pending_format, pending_w, pending_h, pending_stride;
};

static struct slot slots[MAX_INFLIGHT];

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

// ---- uinput keyboard ----
static int uinput_open(int key) {
  int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    perror("open /dev/uinput");
    return -1;
  }
  ioctl(fd, UI_SET_EVBIT, EV_KEY);
  ioctl(fd, UI_SET_KEYBIT, key);
  struct uinput_setup us = {0};
  us.id.bustype = BUS_VIRTUAL;
  us.id.vendor = 0x1d6b;
  us.id.product = 0x0104;
  snprintf(us.name, sizeof(us.name), "apollo-latency-probe");
  if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
    perror("uinput setup");
    close(fd);
    return -1;
  }
  return fd;
}

static void uinput_emit(int fd, int type, int code, int value) {
  struct input_event ev = {0};
  ev.type = (uint16_t) type;
  ev.code = (uint16_t) code;
  ev.value = value;
  if (write(fd, &ev, sizeof(ev)) != sizeof(ev)) {
    perror("uinput write");
  }
}

// ---- screencopy ----
static void request_frame(struct slot *sl);

static void fr_buffer(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t format, uint32_t w, uint32_t h, uint32_t stride) {
  (void) f;
  struct slot *sl = d;
  sl->pending_format = format;
  sl->pending_w = w;
  sl->pending_h = h;
  sl->pending_stride = stride;
}

static bool ensure_buffer(struct slot *sl) {
  if (sl->buffer && sl->buf_w == sl->pending_w && sl->buf_h == sl->pending_h && sl->buf_stride == sl->pending_stride && sl->buf_format == sl->pending_format) {
    return true;
  }
  if (sl->buffer) {
    wl_buffer_destroy(sl->buffer);
    munmap(sl->pixels, (size_t) sl->buf_stride * sl->buf_h);
    sl->buffer = NULL;
  }
  size_t size = (size_t) sl->pending_stride * sl->pending_h;
  int fd = memfd_create("apollo-latency-probe", MFD_CLOEXEC);
  if (fd < 0 || ftruncate(fd, (off_t) size) < 0) {
    perror("memfd");
    return false;
  }
  sl->pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (sl->pixels == MAP_FAILED) {
    perror("mmap");
    return false;
  }
  struct wl_shm_pool *pool = wl_shm_create_pool(st.shm, fd, (int32_t) size);
  sl->buffer = wl_shm_pool_create_buffer(pool, 0, (int32_t) sl->pending_w, (int32_t) sl->pending_h, (int32_t) sl->pending_stride, sl->pending_format);
  wl_shm_pool_destroy(pool);
  close(fd);
  sl->buf_w = sl->pending_w;
  sl->buf_h = sl->pending_h;
  sl->buf_stride = sl->pending_stride;
  sl->buf_format = sl->pending_format;
  if (sl == &slots[0]) {
    printf("# capture buffer %ux%u stride %u format 0x%x\n", sl->buf_w, sl->buf_h, sl->buf_stride, sl->buf_format);
  }
  return true;
}

static void fr_buffer_done(void *d, struct zwlr_screencopy_frame_v1 *f) {
  struct slot *sl = d;
  if (!ensure_buffer(sl)) {
    running = 0;
    return;
  }
  if (st.immediate) {
    zwlr_screencopy_frame_v1_copy(f, sl->buffer);
  } else {
    zwlr_screencopy_frame_v1_copy_with_damage(f, sl->buffer);
  }
}

static void fr_flags(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t flags) {
  (void) d;
  (void) f;
  (void) flags;
}

static void fr_ready(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec) {
  struct slot *sl = d;
  uint64_t recv = now_ns();
  uint64_t ready = (((uint64_t) sec_hi << 32) | sec_lo) * 1000000000ull + nsec;
  // 32-bit formats: average the green byte (byte 1 in both XRGB and XBGR little-endian layouts).
  uint64_t sum = 0;
  uint64_t n = 0;
  for (uint32_t y = 0; y < sl->buf_h; ++y) {
    const uint8_t *row = sl->pixels + (size_t) y * sl->buf_stride;
    for (uint32_t x = 0; x < sl->buf_w; ++x) {
      sum += row[x * 4 + 1];
      ++n;
    }
  }
  printf("F %lu %lu %lu\n", ready, recv, n ? sum / n : 0);
  ++st.frames;
  zwlr_screencopy_frame_v1_destroy(f);
  sl->frame = NULL;
  request_frame(sl);
}

static void fr_failed(void *d, struct zwlr_screencopy_frame_v1 *f) {
  struct slot *sl = d;
  printf("X %lu\n", now_ns());
  ++st.failures;
  zwlr_screencopy_frame_v1_destroy(f);
  sl->frame = NULL;
  if (st.failures > 100) {
    fprintf(stderr, "too many capture failures\n");
    running = 0;
    return;
  }
  request_frame(sl);
}

static void fr_damage(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
  (void) d;
  (void) f;
  (void) x;
  (void) y;
  (void) w;
  (void) h;
}

static void fr_dmabuf(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t format, uint32_t w, uint32_t h) {
  (void) d;
  (void) f;
  (void) format;
  (void) w;
  (void) h;
}

static const struct zwlr_screencopy_frame_v1_listener fr_listener = {
  .buffer = fr_buffer,
  .flags = fr_flags,
  .ready = fr_ready,
  .failed = fr_failed,
  .damage = fr_damage,
  .linux_dmabuf = fr_dmabuf,
  .buffer_done = fr_buffer_done,
};

static void request_frame(struct slot *sl) {
  if (!running) {
    return;
  }
  sl->frame = zwlr_screencopy_manager_v1_capture_output_region(st.screencopy, 0, st.target->wl, st.rx, st.ry, st.rw, st.rh);
  zwlr_screencopy_frame_v1_add_listener(sl->frame, &fr_listener, sl);
}

// ---- outputs / registry ----
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

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version) {
  (void) d;
  if (!strcmp(iface, wl_shm_interface.name)) {
    st.shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
  } else if (!strcmp(iface, zwlr_screencopy_manager_v1_interface.name)) {
    st.screencopy_version = version < 3 ? version : 3;
    st.screencopy = wl_registry_bind(r, name, &zwlr_screencopy_manager_v1_interface, st.screencopy_version);
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

static void arm_ms(int fd, int ms) {
  struct itimerspec its = {.it_value = {.tv_sec = ms / 1000, .tv_nsec = (long) (ms % 1000) * 1000000L}};
  timerfd_settime(fd, 0, &its, NULL);
}

int main(int argc, char **argv) {
  const char *output_name = NULL;
  int duration_s = 30;
  int press_min = 0, press_max = 0;
  int key = KEY_LEFTSHIFT;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--output") && i + 1 < argc) {
      output_name = argv[++i];
    } else if (!strcmp(argv[i], "--region") && i + 1 < argc) {
      if (sscanf(argv[++i], "%d,%d,%d,%d", &st.rx, &st.ry, &st.rw, &st.rh) != 4) {
        fprintf(stderr, "bad --region\n");
        return 2;
      }
    } else if (!strcmp(argv[i], "--duration") && i + 1 < argc) {
      duration_s = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--press") && i + 1 < argc) {
      if (sscanf(argv[++i], "%d,%d", &press_min, &press_max) != 2 || press_max < press_min) {
        fprintf(stderr, "bad --press\n");
        return 2;
      }
    } else if (!strcmp(argv[i], "--inflight") && i + 1 < argc) {
      st.inflight = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--immediate")) {
      st.immediate = true;
    } else if (!strcmp(argv[i], "--key") && i + 1 < argc) {
      key = atoi(argv[++i]);
    } else {
      fprintf(stderr, "usage: %s --output NAME --region X,Y,W,H [--duration S] [--press MIN_MS,MAX_MS] [--key CODE] [--inflight N] [--immediate]\n", argv[0]);
      return 2;
    }
  }
  if (st.inflight < 1 || st.inflight > MAX_INFLIGHT) {
    st.inflight = st.inflight < 1 ? 1 : MAX_INFLIGHT;
  }
  if (!output_name || st.rw <= 0 || st.rh <= 0) {
    fprintf(stderr, "--output and --region are required\n");
    return 2;
  }
  setvbuf(stdout, NULL, _IOLBF, 0);
  srand((unsigned) now_ns());
  struct sigaction sa = {.sa_handler = on_signal};
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);

  int ufd = -1;
  if (press_max > 0) {
    ufd = uinput_open(key);
    if (ufd < 0) {
      return 1;
    }
  }

  st.display = wl_display_connect(NULL);
  if (!st.display) {
    fprintf(stderr, "cannot connect to Wayland\n");
    return 1;
  }
  struct wl_registry *reg = wl_display_get_registry(st.display);
  wl_registry_add_listener(reg, &reg_listener, NULL);
  wl_display_roundtrip(st.display);
  wl_display_roundtrip(st.display);
  if (!st.shm || !st.screencopy || st.screencopy_version < 3) {
    fprintf(stderr, "need wl_shm and zwlr_screencopy_manager_v1 v3\n");
    return 1;
  }
  for (struct output *o = st.outputs; o; o = o->next) {
    if (!strcmp(o->name, output_name)) {
      st.target = o;
    }
  }
  if (!st.target) {
    fprintf(stderr, "output %s not found\n", output_name);
    return 1;
  }
  printf("# probe output=%s region=%d,%d,%dx%d press=%d..%d key=%d inflight=%d immediate=%d\n", output_name, st.rx, st.ry, st.rw, st.rh, press_min, press_max, key, st.inflight, st.immediate);

  int press_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
  int end_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
  if (ufd >= 0) {
    arm_ms(press_fd, 1500);  // give the compositor time to pick up the new keyboard
  }
  struct itimerspec end = {.it_value = {.tv_sec = duration_s}};
  timerfd_settime(end_fd, 0, &end, NULL);

  for (int i = 0; i < st.inflight; ++i) {
    request_frame(&slots[i]);
  }
  while (running) {
    while (wl_display_prepare_read(st.display) != 0) {
      wl_display_dispatch_pending(st.display);
    }
    wl_display_flush(st.display);
    struct pollfd fds[3] = {{wl_display_get_fd(st.display), POLLIN, 0}, {press_fd, POLLIN, 0}, {end_fd, POLLIN, 0}};
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
      if (read(press_fd, &n, sizeof(n)) == sizeof(n) && ufd >= 0) {
        uinput_emit(ufd, EV_KEY, key, 1);
        uinput_emit(ufd, EV_SYN, SYN_REPORT, 0);
        printf("K %lu\n", now_ns());
        uinput_emit(ufd, EV_KEY, key, 0);
        uinput_emit(ufd, EV_SYN, SYN_REPORT, 0);
        arm_ms(press_fd, press_min + (press_max > press_min ? rand() % (press_max - press_min + 1) : 0));
      }
    }
    if (fds[2].revents & POLLIN) {
      running = 0;
    }
  }
  printf("# done frames=%lu failures=%lu\n", st.frames, st.failures);
  if (ufd >= 0) {
    ioctl(ufd, UI_DEV_DESTROY);
    close(ufd);
  }
  wl_display_disconnect(st.display);
  return 0;
}
