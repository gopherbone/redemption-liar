// SPDX-License-Identifier: GPL-2.0-or-later
// SDL_OpenAudio over a pipe into PipeWire. PipeWire's client libraries and
// plugins on the Brick are 64-bit and can't load in this 32-bit process, so the
// game's callback feeds `pw-cat` (64-bit, found through $LIAR_PW_CAT): the same
// mono 22050 Hz blocks the Wiz played. At most one block waits in the pipe, for
// latency, and pw-cat always gets data, even silence while the game is off screen
// or paused: it reads the pipe inside its PipeWire callback, so a pipe left empty
// would stall the audio graph other apps share.
#define _GNU_SOURCE
#include "liar.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static SDL_AudioSpec spec;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t thread;
static volatile int running, paused = 1;
static int out_fd = -1;
static pid_t helper;

void audio_lock(void) { pthread_mutex_lock(&lock); }
void audio_unlock(void) { pthread_mutex_unlock(&lock); }
void audio_pause(int on) { paused = on; }

static int start_helper(void) {
    const char *pw = getenv("LIAR_PW_CAT");
    if (!pw) pw = "pw-cat";
    int p[2];
    if (pipe2(p, O_CLOEXEC)) return -1;
    fcntl(p[1], F_SETPIPE_SZ, 4096);
    char rate[16], ch[16], lat[16];
    snprintf(rate, sizeof rate, "%d", spec.freq);
    snprintf(ch, sizeof ch, "%d", spec.channels);
    snprintf(lat, sizeof lat, "%d", spec.samples / 2);
    char *const argv[] = {(char *)pw, "--playback", "--raw", "--format", "s16", "--rate", rate, "--channels", ch,
                          "--latency", lat, "--media-category", "Playback", "--media-role", "Game",
                          "--properties", "{ node.name = \"Redemption: Liar\" application.name = \"Redemption: Liar\" }", "-", NULL};
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, p[0], 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    int e = posix_spawnp(&helper, pw, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(p[0]);
    if (e) {
        fprintf(stderr, "liar: can't start %s: %s\n", pw, strerror(e));
        close(p[1]);
        return -1;
    }
    out_fd = p[1];
    return 0;
}

static void sleep_block(void) {
    long ns = 1000000000L / spec.freq * spec.samples;
    struct timespec ts = {ns / 1000000000L, ns % 1000000000L};
    nanosleep(&ts, NULL);
}

static void *audio_thread(void *arg) {
    (void)arg;
    uint8_t *buf = malloc(spec.size);
    while (running) {
        // room for this block, keeping one queued at most
        int pending = 0;
        while (running && out_fd >= 0 && !ioctl(out_fd, FIONREAD, &pending) && pending >= (int)spec.size) {
            struct timespec ts = {0, 2000000};
            nanosleep(&ts, NULL);
        }
        memset(buf, spec.silence, spec.size);
        if (!paused && display_visible) {
            audio_lock();
            if (spec.callback) spec.callback(spec.userdata, buf, spec.size);
            audio_unlock();
        }
        size_t off = 0;
        while (off < spec.size && running) {
            ssize_t n = out_fd >= 0 ? write(out_fd, buf + off, spec.size - off) : -1;
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) { // no helper: keep time so the game carries on, silently
                sleep_block();
                break;
            }
            off += n;
        }
    }
    free(buf);
    return NULL;
}

int audio_open(SDL_AudioSpec *want) {
    if (running) return 0;
    spec = *want;
    int bytes = (spec.format & 0xff) / 8;
    spec.size = spec.samples * spec.channels * bytes;
    spec.silence = spec.format == AUDIO_U8 ? 0x80 : 0;
    want->size = spec.size;
    want->silence = spec.silence;
    signal(SIGPIPE, SIG_IGN);
    start_helper(); // no sound is not fatal
    running = 1;
    if (pthread_create(&thread, NULL, audio_thread, NULL)) {
        running = 0;
        return -1;
    }
    return 0;
}

void audio_close(void) {
    if (!running) return;
    running = 0;
    if (out_fd >= 0) {
        close(out_fd); // EOF ends the helper
        out_fd = -1;
    }
    pthread_join(thread, NULL);
}
