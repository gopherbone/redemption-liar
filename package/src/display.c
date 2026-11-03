// SPDX-License-Identifier: GPL-2.0-or-later
// The window and the pad, on their own thread: a small Wayland client speaking
// the wire protocol directly (static binary, no 32-bit libwayland to carry),
// and evdev for the shell's per-app virtual gamepad.
//
// Picture: the game draws 320x240 RGB565. We send a 960x720 XRGB8888 buffer
// (an exact 3x, each game pixel a square block) with a wp_viewporter destination
// of the whole 1024x768 panel: the compositor's bilinear filter then only
// softens the block edges, which is the "sharp bilinear" look, on the GPU, at no
// cost here. Set LIAR_SCALE=integer for an exact 3x with black borders instead.
// Unchanged rows are neither converted nor damaged, so a still screen is free.
#define _GNU_SOURCE
#include "liar.h"
#include <arm_neon.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define GAME_W 320
#define GAME_H 240
#define MAX_PADS 6

volatile int display_visible = 1;
static volatile int quit_flag;
static int wl_fd = -1, wake_fd = -1;

// objects (ours, allocated from 2 up)
enum { ID_DISPLAY = 1, ID_REGISTRY = 2 };
static uint32_t next_id = 3;
static uint32_t id_compositor, id_shm, id_wm, id_viewporter, id_surface, id_xdg_surface, id_toplevel, id_viewport, id_callback, id_sync;
static uint32_t id_buffer[2];
static int buf_busy[2];

static int scale_integer;
static int buf_w, buf_h, org_x, org_y;
static uint8_t *shm;
static uint8_t row_dirty[2][GAME_H];
static int have_configure, cfg_w, cfg_h, cfg_activated = 1, want_ack;
static uint32_t ack_serial;
static int frame_pending;

// from the game thread
static pthread_mutex_t frame_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t visible_cond = PTHREAD_COND_INITIALIZER;
static uint16_t mailbox[GAME_W * GAME_H];
static int frame_ready;
static uint16_t shown[GAME_W * GAME_H];

// ---- wire ----
static uint32_t msg[512];
static void w_flush_fd(int n, int fd) {
    struct iovec iov = {msg, (size_t)n * 4};
    struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1};
    char ctl[CMSG_SPACE(sizeof(int))];
    if (fd >= 0) {
        mh.msg_control = ctl;
        mh.msg_controllen = sizeof ctl;
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof(int));
    }
    while (sendmsg(wl_fd, &mh, MSG_NOSIGNAL) < 0) {
        if (errno == EINTR) continue;
        if (errno == EAGAIN) {
            struct pollfd p = {wl_fd, POLLOUT, 0};
            poll(&p, 1, 100);
            continue;
        }
        quit_flag = 1; // the compositor is gone
        return;
    }
}
static inline void w_start(uint32_t obj, uint32_t op, int words) {
    msg[0] = obj;
    msg[1] = ((uint32_t)(words + 2) * 4) << 16 | op;
}
static void w0(uint32_t obj, uint32_t op) { w_start(obj, op, 0); w_flush_fd(2, -1); }
static void w1(uint32_t obj, uint32_t op, uint32_t a) { w_start(obj, op, 1); msg[2] = a; w_flush_fd(3, -1); }
static void w2(uint32_t obj, uint32_t op, uint32_t a, uint32_t b) { w_start(obj, op, 2); msg[2] = a; msg[3] = b; w_flush_fd(4, -1); }
static void w3(uint32_t obj, uint32_t op, uint32_t a, uint32_t b, uint32_t c) { w_start(obj, op, 3); msg[2] = a; msg[3] = b; msg[4] = c; w_flush_fd(5, -1); }
static void w4(uint32_t obj, uint32_t op, uint32_t a, uint32_t b, uint32_t c, uint32_t d) { w_start(obj, op, 4); msg[2] = a; msg[3] = b; msg[4] = c; msg[5] = d; w_flush_fd(6, -1); }
static int put_str(int at, const char *s) {
    uint32_t n = strlen(s) + 1, words = (n + 3) / 4;
    msg[at] = n;
    msg[at + words] = 0;
    memcpy(&msg[at + 1], s, n);
    return at + 1 + words;
}
static void w_str(uint32_t obj, uint32_t op, const char *s) {
    int end = put_str(2, s);
    w_start(obj, op, end - 2);
    w_flush_fd(end, -1);
}
static void bind_global(uint32_t name, const char *iface, uint32_t ver, uint32_t id) {
    msg[2] = name;
    int at = put_str(3, iface);
    msg[at++] = ver;
    msg[at++] = id;
    w_start(ID_REGISTRY, 0, at - 2);
    w_flush_fd(at, -1);
}

static void handle_event(uint32_t obj, uint32_t op, const uint32_t *a, int words) {
    if (obj == ID_DISPLAY) {
        if (op == 0) {
            fprintf(stderr, "liar: Wayland error %u on object %u: %s\n", a[2], a[0], (const char *)&a[4]);
            quit_flag = 1;
        }
    } else if (obj == ID_REGISTRY) {
        if (op != 0) return;
        const char *iface = (const char *)&a[2];
        if (!strcmp(iface, "wl_compositor")) bind_global(a[0], iface, 4, id_compositor = next_id++);
        else if (!strcmp(iface, "wl_shm")) bind_global(a[0], iface, 1, id_shm = next_id++);
        else if (!strcmp(iface, "xdg_wm_base")) bind_global(a[0], iface, 1, id_wm = next_id++);
        else if (!strcmp(iface, "wp_viewporter")) bind_global(a[0], iface, 1, id_viewporter = next_id++);
    } else if (obj == id_wm) {
        if (op == 0) w1(id_wm, 3, a[0]); // ping -> pong
    } else if (obj == id_xdg_surface) {
        if (op == 0) { have_configure = 1; ack_serial = a[0]; want_ack = 1; }
    } else if (obj == id_toplevel) {
        if (op == 0) { // configure(w, h, states)
            cfg_w = (int)a[0];
            cfg_h = (int)a[1];
            int activated = 0;
            uint32_t bytes = a[2];
            for (uint32_t i = 0; i < bytes / 4 && (int)(3 + i) < words; i++)
                if (a[3 + i] == 4) activated = 1;
            cfg_activated = activated;
        } else if (op == 1) {
            quit_flag = 1;
        }
    } else if (obj == id_buffer[0] || obj == id_buffer[1]) {
        if (op == 0) buf_busy[obj == id_buffer[1]] = 0;
    } else if (obj == id_callback) {
        if (op == 0) { frame_pending = 0; id_callback = 0; }
    } else if (obj == id_sync) {
        if (op == 0) id_sync = 0;
    }
}

static uint8_t rx[65536];
static size_t rx_len;
static int w_read(void) {
    ssize_t n = recv(wl_fd, rx + rx_len, sizeof rx - rx_len, MSG_DONTWAIT);
    if (n == 0) { quit_flag = 1; return -1; }
    if (n < 0) return errno == EAGAIN || errno == EINTR ? 0 : (quit_flag = 1, -1);
    rx_len += n;
    size_t off = 0;
    while (rx_len - off >= 8) {
        uint32_t h[2];
        memcpy(h, rx + off, 8);
        uint32_t size = h[1] >> 16;
        if (size < 8 || rx_len - off < size) break;
        static uint32_t ev[16384];
        memcpy(ev, rx + off + 8, size - 8);
        handle_event(h[0], h[1] & 0xffff, ev, (size - 8) / 4);
        off += size;
    }
    memmove(rx, rx + off, rx_len - off);
    rx_len -= off;
    return 0;
}
static void w_blocking_read(void) {
    struct pollfd p = {wl_fd, POLLIN, 0};
    if (poll(&p, 1, 1000) > 0) w_read();
}
static void w_roundtrip(void) {
    id_sync = next_id++;
    w1(ID_DISPLAY, 0, id_sync);
    for (int i = 0; id_sync && i < 20 && !quit_flag; i++) w_blocking_read();
}

// ---- pixels ----
static inline void row_3x(const uint16_t *s, uint32_t *d) {
    uint32x4x3_t out;
    for (int x = 0; x < GAME_W; x += 8, s += 8, d += 24) {
        uint16x8_t v = vld1q_u16(s);
        uint16x8_t r = vshrq_n_u16(v, 11), g = vandq_u16(vshrq_n_u16(v, 5), vdupq_n_u16(63)), b = vandq_u16(v, vdupq_n_u16(31));
        r = vorrq_u16(vshlq_n_u16(r, 3), vshrq_n_u16(r, 2));
        g = vorrq_u16(vshlq_n_u16(g, 2), vshrq_n_u16(g, 4));
        b = vorrq_u16(vshlq_n_u16(b, 3), vshrq_n_u16(b, 2));
        uint32x4_t lo = vorrq_u32(vshlq_n_u32(vmovl_u16(vget_low_u16(r)), 16),
                                  vorrq_u32(vshlq_n_u32(vmovl_u16(vget_low_u16(g)), 8), vmovl_u16(vget_low_u16(b))));
        uint32x4_t hi = vorrq_u32(vshlq_n_u32(vmovl_u16(vget_high_u16(r)), 16),
                                  vorrq_u32(vshlq_n_u32(vmovl_u16(vget_high_u16(g)), 8), vmovl_u16(vget_high_u16(b))));
        out.val[0] = out.val[1] = out.val[2] = lo;
        vst3q_u32(d, out);
        out.val[0] = out.val[1] = out.val[2] = hi;
        vst3q_u32(d + 12, out);
    }
}

static void present(void) {
    if (frame_pending || !have_configure) return;
    int b = buf_busy[0] ? 1 : 0;
    if (buf_busy[b]) return;
    pthread_mutex_lock(&frame_lock);
    if (!frame_ready) {
        pthread_mutex_unlock(&frame_lock);
        return;
    }
    frame_ready = 0;
    int first = -1, last = -1;
    for (int y = 0; y < GAME_H; y++) {
        uint16_t *m = mailbox + y * GAME_W, *o = shown + y * GAME_W;
        if (memcmp(m, o, GAME_W * 2)) {
            memcpy(o, m, GAME_W * 2);
            row_dirty[0][y] = row_dirty[1][y] = 1;
            if (first < 0) first = y;
            last = y;
        }
    }
    pthread_mutex_unlock(&frame_lock);
    if (first < 0) {
        for (int y = 0; y < GAME_H; y++)
            if (row_dirty[b][y]) { first = y; break; }
        if (first < 0) return; // nothing changed
        last = GAME_H - 1;
    }
    uint8_t *base = shm + (size_t)b * buf_w * buf_h * 4;
    for (int y = 0; y < GAME_H; y++) {
        if (!row_dirty[b][y]) continue;
        row_dirty[b][y] = 0;
        uint8_t *to = base + ((size_t)org_y + y * 3) * buf_w * 4 + (size_t)org_x * 4;
        row_3x(shown + y * GAME_W, (uint32_t *)to);
        memcpy(to + buf_w * 4, to, GAME_W * 12);
        memcpy(to + buf_w * 8, to, GAME_W * 12);
    }
    w3(id_surface, 1, id_buffer[b], 0, 0);
    w4(id_surface, 9, 0, org_y + first * 3, buf_w, (last - first + 1) * 3);
    id_callback = next_id++;
    w1(id_surface, 3, id_callback);
    w0(id_surface, 6);
    buf_busy[b] = 1;
    frame_pending = 1;
}

// ---- pad ----
// Wiz button numbers, as the game reads them through SDL_JoystickGetButton.
enum {
    WIZ_UP, WIZ_UPLEFT, WIZ_LEFT, WIZ_DOWNLEFT, WIZ_DOWN, WIZ_DOWNRIGHT, WIZ_RIGHT, WIZ_UPRIGHT,
    WIZ_MENU, WIZ_SELECT, WIZ_L, WIZ_R, WIZ_A, WIZ_B, WIZ_X, WIZ_Y, WIZ_VOLUP, WIZ_VOLDOWN
};
static const struct { uint16_t code; uint8_t wiz; } keymap[] = {
    {BTN_DPAD_UP, WIZ_UP}, {BTN_DPAD_DOWN, WIZ_DOWN}, {BTN_DPAD_LEFT, WIZ_LEFT}, {BTN_DPAD_RIGHT, WIZ_RIGHT},
    // the Brick's labels are B south, A east, X north, Y west
    {BTN_SOUTH, WIZ_B}, {BTN_EAST, WIZ_A}, {BTN_NORTH, WIZ_X}, {BTN_WEST, WIZ_Y},
    {BTN_TL, WIZ_L}, {BTN_TR, WIZ_R}, {BTN_TL2, WIZ_L}, {BTN_TR2, WIZ_R},
    {BTN_SELECT, WIZ_SELECT}, {BTN_START, WIZ_MENU},
};
static int pad_fd[MAX_PADS], npads, ino_fd = -1;
static uint32_t held;
static uint32_t volatile latched;

static uint32_t with_diagonals(uint32_t b) {
    int up = b >> WIZ_UP & 1, down = b >> WIZ_DOWN & 1, left = b >> WIZ_LEFT & 1, right = b >> WIZ_RIGHT & 1;
    if (up && left) b |= 1u << WIZ_UPLEFT;
    if (up && right) b |= 1u << WIZ_UPRIGHT;
    if (down && left) b |= 1u << WIZ_DOWNLEFT;
    if (down && right) b |= 1u << WIZ_DOWNRIGHT;
    return b;
}

uint32_t display_buttons(void) {
    uint32_t l = __atomic_exchange_n(&latched, 0, __ATOMIC_ACQ_REL);
    return with_diagonals(__atomic_load_n(&held, __ATOMIC_ACQUIRE) | l);
}

// Mortar's clones have vendor 0x6d72, and the shell lists the ones not meant for
// us (and the physical pads) in the same variable it gives SDL.
static int blocked(uint16_t ven, uint16_t prod) {
    char want[32];
    const char *list = getenv("SDL_JOYSTICK_BLACKLIST_DEVICES");
    if (!list) return 0;
    snprintf(want, sizeof want, "0x%04x/0x%04x", ven, prod);
    return strcasestr(list, want) != NULL;
}

static void scan_pads(void) {
    for (int i = 0; i < npads; i++) close(pad_fd[i]);
    npads = 0;
    const char *forced = getenv("LIAR_PAD");
    if (forced) {
        pad_fd[0] = open(forced, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        npads = pad_fd[0] >= 0;
        return;
    }
    DIR *dir = opendir("/dev/input");
    if (!dir) return;
    struct dirent *e;
    while ((e = readdir(dir)) && npads < MAX_PADS) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char path[64];
        snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        struct input_id id;
        if (ioctl(fd, EVIOCGID, &id) || id.vendor != 0x6d72 || blocked(id.vendor, id.product)) {
            close(fd);
            continue;
        }
        pad_fd[npads++] = fd;
    }
    closedir(dir);
}

static void handle_input(int fd) {
    struct input_event ev[32];
    ssize_t n;
    while ((n = read(fd, ev, sizeof ev)) > 0) {
        for (size_t i = 0; i < (size_t)n / sizeof ev[0]; i++) {
            uint32_t bit = 0;
            if (ev[i].type == EV_KEY) {
                for (size_t k = 0; k < sizeof keymap / sizeof keymap[0]; k++)
                    if (keymap[k].code == ev[i].code) bit = 1u << keymap[k].wiz;
                if (!bit) continue;
                if (ev[i].value) {
                    __atomic_or_fetch(&held, bit, __ATOMIC_ACQ_REL);
                    __atomic_or_fetch(&latched, bit, __ATOMIC_ACQ_REL); // a tap shorter than a frame still counts
                } else {
                    __atomic_and_fetch(&held, ~bit, __ATOMIC_ACQ_REL);
                }
            } else if (ev[i].type == EV_ABS && (ev[i].code == ABS_HAT0X || ev[i].code == ABS_HAT0Y)) {
                // a USB or Bluetooth pad's D-pad
                uint32_t neg = ev[i].code == ABS_HAT0X ? 1u << WIZ_LEFT : 1u << WIZ_UP;
                uint32_t pos = ev[i].code == ABS_HAT0X ? 1u << WIZ_RIGHT : 1u << WIZ_DOWN;
                __atomic_and_fetch(&held, ~(neg | pos), __ATOMIC_ACQ_REL);
                if (ev[i].value) {
                    bit = ev[i].value < 0 ? neg : pos;
                    __atomic_or_fetch(&held, bit, __ATOMIC_ACQ_REL);
                    __atomic_or_fetch(&latched, bit, __ATOMIC_ACQ_REL);
                }
            }
        }
    }
    if (n < 0 && errno != EAGAIN && errno != EINTR) { // the shell removed it
        scan_pads();
    }
}

// ---- thread ----
static void set_visible(int v) {
    pthread_mutex_lock(&frame_lock);
    display_visible = v;
    pthread_cond_broadcast(&visible_cond);
    pthread_mutex_unlock(&frame_lock);
    if (!v) {
        __atomic_store_n(&held, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&latched, 0, __ATOMIC_RELEASE);
    }
}

void display_wait_visible(void) {
    pthread_mutex_lock(&frame_lock);
    while (!display_visible && !quit_flag) pthread_cond_wait(&visible_cond, &frame_lock);
    pthread_mutex_unlock(&frame_lock);
}

static void *thread_main(void *arg) {
    (void)arg;
    sigset_t all;
    sigfillset(&all);
    sigdelset(&all, SIGSEGV);
    sigdelset(&all, SIGBUS);
    sigdelset(&all, SIGILL);
    pthread_sigmask(SIG_BLOCK, &all, NULL);
    while (!quit_flag) {
        struct pollfd p[MAX_PADS + 3];
        int n = 0;
        p[n++] = (struct pollfd){wl_fd, POLLIN, 0};
        p[n++] = (struct pollfd){wake_fd, POLLIN, 0};
        int ino_at = -1;
        if (ino_fd >= 0) { ino_at = n; p[n++] = (struct pollfd){ino_fd, POLLIN, 0}; }
        int pad_at = n;
        for (int i = 0; i < npads; i++) p[n++] = (struct pollfd){pad_fd[i], POLLIN, 0};
        // frame callbacks stop off screen: don't wait on one forever
        int r = poll(p, n, frame_pending ? 200 : 1000);
        if (r < 0 && errno != EINTR) break;
        if (p[0].revents & (POLLERR | POLLHUP)) { quit_flag = 1; break; }
        if (p[0].revents & POLLIN) w_read();
        if (p[1].revents & POLLIN) {
            uint64_t v;
            if (read(wake_fd, &v, sizeof v) < 0) { /* nothing to do */ }
        }
        if (ino_at >= 0 && p[ino_at].revents & POLLIN) {
            char tmp[1024];
            if (read(ino_fd, tmp, sizeof tmp) > 0) {
                for (int i = 0; i < npads; i++) p[pad_at + i].revents = 0;
                scan_pads();
            }
        } else {
            for (int i = 0; i < npads; i++)
                if (p[pad_at + i].revents & (POLLIN | POLLERR | POLLHUP)) handle_input(pad_fd[i]);
        }
        if (want_ack) {
            want_ack = 0;
            w1(id_xdg_surface, 4, ack_serial);
            if (id_viewport && cfg_w > 0 && cfg_h > 0) w2(id_viewport, 2, cfg_w, cfg_h);
            if (cfg_activated != display_visible) set_visible(cfg_activated);
            // show again whatever was there, for a viewport change to take effect
            w0(id_surface, 6);
        }
        if (r == 0) frame_pending = 0;
        present();
    }
    set_visible(1); // release a waiting game thread
    return NULL;
}

// ---- open ----
static void on_term(int sig) { (void)sig; quit_flag = 1; }

int display_should_quit(void) { return quit_flag; }

void display_present(const uint16_t *px, int pitch) {
    pthread_mutex_lock(&frame_lock);
    if (pitch == GAME_W * 2) memcpy(mailbox, px, sizeof mailbox);
    else
        for (int y = 0; y < GAME_H; y++) memcpy(mailbox + y * GAME_W, (const uint8_t *)px + y * pitch, GAME_W * 2);
    frame_ready = 1;
    pthread_mutex_unlock(&frame_lock);
    uint64_t one = 1;
    if (write(wake_fd, &one, sizeof one) < 0) { /* the thread is awake anyway */ }
}

int display_open(const char *title) {
    const char *name = getenv("WAYLAND_DISPLAY"), *dir = getenv("XDG_RUNTIME_DIR");
    if (!name) name = "wayland-0";
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    if (name[0] == '/') snprintf(a.sun_path, sizeof a.sun_path, "%s", name);
    else if (dir) snprintf(a.sun_path, sizeof a.sun_path, "%s/%s", dir, name);
    else return -1;
    wl_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (wl_fd < 0 || connect(wl_fd, (struct sockaddr *)&a, sizeof a)) {
        perror(a.sun_path);
        return -1;
    }
    wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

    const char *sc = getenv("LIAR_SCALE");
    scale_integer = sc && !strcmp(sc, "integer");

    w1(ID_DISPLAY, 1, ID_REGISTRY); // get_registry
    w_roundtrip();
    if (!id_compositor || !id_shm || !id_wm) {
        fprintf(stderr, "liar: compositor lacks wl_compositor, wl_shm or xdg_wm_base\n");
        return -1;
    }
    id_surface = next_id++;
    w1(id_compositor, 0, id_surface);
    id_xdg_surface = next_id++;
    w2(id_wm, 2, id_xdg_surface, id_surface);
    id_toplevel = next_id++;
    w1(id_xdg_surface, 1, id_toplevel);
    w_str(id_toplevel, 2, title);
    w_str(id_toplevel, 3, "liar");
    w1(id_toplevel, 11, 0); // set_fullscreen
    if (id_viewporter && !scale_integer) {
        id_viewport = next_id++;
        w2(id_viewporter, 1, id_viewport, id_surface);
    }
    w0(id_surface, 6);
    for (int i = 0; i < 50 && !have_configure && !quit_flag; i++) w_blocking_read();
    if (!have_configure || quit_flag) {
        fprintf(stderr, "liar: no configure from the compositor\n");
        return -1;
    }

    int use_viewport = id_viewport != 0;
    buf_w = use_viewport ? GAME_W * 3 : (cfg_w > 0 ? cfg_w : 1024);
    buf_h = use_viewport ? GAME_H * 3 : (cfg_h > 0 ? cfg_h : 768);
    org_x = (buf_w - GAME_W * 3) / 2;
    org_y = (buf_h - GAME_H * 3) / 2;
    if (org_x < 0) org_x = 0;
    if (org_y < 0) org_y = 0;
    size_t size = (size_t)buf_w * buf_h * 4 * 2;
    int fd = memfd_create("liar-screen", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size)) { perror("memfd"); return -1; }
    shm = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) { perror("mmap"); return -1; }
    memset(row_dirty, 1, sizeof row_dirty);

    uint32_t pool = next_id++;
    w_start(id_shm, 0, 2);
    msg[2] = pool;
    msg[3] = (uint32_t)size;
    w_flush_fd(4, fd);
    close(fd);
    for (int b = 0; b < 2; b++) {
        id_buffer[b] = next_id++;
        w_start(pool, 0, 6);
        msg[2] = id_buffer[b];
        msg[3] = (uint32_t)((size_t)b * size / 2);
        msg[4] = buf_w;
        msg[5] = buf_h;
        msg[6] = buf_w * 4;
        msg[7] = 1; // XRGB8888
        w_flush_fd(8, -1);
    }

    want_ack = 1;
    // Watch first: the shell's clone pad appears around when we start, and one
    // that arrives between a scan and the watch would never be seen.
    ino_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ino_fd >= 0) inotify_add_watch(ino_fd, "/dev/input", IN_CREATE | IN_DELETE | IN_ATTRIB);
    scan_pads();

    struct sigaction sa = {.sa_handler = on_term};
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, thread_main, NULL)) return -1;
    return 0;
}
