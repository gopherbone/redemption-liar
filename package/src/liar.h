// SPDX-License-Identifier: GPL-2.0-or-later
// Shared declarations of the Redemption: Liar loader. The SDL types below are
// the SDL 1.2 ABI the game was compiled against (32-bit, GCC 4.0, OABI); the
// game reads these structures directly, so the layouts must not change.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define SDL_SRCCOLORKEY 0x00001000u
#define SDL_SRCALPHA 0x00010000u
#define AUDIO_U8 0x0008
#define AUDIO_S16LSB 0x8010

typedef struct { int16_t x, y; uint16_t w, h; } SDL_Rect;
typedef struct { uint8_t r, g, b, unused; } SDL_Color;
typedef struct { int ncolors; SDL_Color *colors; } SDL_Palette;
typedef struct {
    SDL_Palette *palette;
    uint8_t BitsPerPixel, BytesPerPixel;
    uint8_t Rloss, Gloss, Bloss, Aloss;
    uint8_t Rshift, Gshift, Bshift, Ashift;
    uint32_t Rmask, Gmask, Bmask, Amask;
    uint32_t colorkey;
    uint8_t alpha;
} SDL_PixelFormat;
typedef struct {
    uint32_t flags;
    SDL_PixelFormat *format;
    int w, h;
    uint16_t pitch;
    void *pixels;
    int offset;
    void *hwdata;
    SDL_Rect clip_rect;
    uint32_t unused1, locked;
    void *map;
    unsigned format_version;
    int refcount;
} SDL_Surface;
typedef struct SDL_RWops {
    int (*seek)(struct SDL_RWops *, int offset, int whence);
    int (*read)(struct SDL_RWops *, void *ptr, int size, int maxnum);
    int (*write)(struct SDL_RWops *, const void *ptr, int size, int num);
    int (*close)(struct SDL_RWops *);
    uint32_t type;
    struct { uint8_t *base, *here, *stop; } mem;
} SDL_RWops;
typedef struct {
    int freq;
    uint16_t format;
    uint8_t channels, silence;
    uint16_t samples, padding;
    uint32_t size;
    void (*callback)(void *userdata, uint8_t *stream, int len);
    void *userdata;
} SDL_AudioSpec;
// Only the integer fields are used, by us and by the game, which never does
// double arithmetic. rate_incr and len_ratio are plain 8-byte holes here.
typedef struct {
    int needed;
    uint16_t src_format, dst_format;
    uint32_t rate_incr[2];
    uint8_t *buf;
    int len, len_cvt, len_mult;
    uint32_t len_ratio[2];
    uint32_t filters[10];
    int filter_index;
} SDL_AudioCVT; // 84 bytes, on the game's stack; sdl.c keeps its own state in filters[]

// main.c
void liar_exit(int code) __attribute__((noreturn));

// sdl.c
SDL_Surface *liar_new_surface(int w, int h, int depth, uint32_t R, uint32_t G, uint32_t B, uint32_t A);

// image.c: decode a BMP or PNG from memory the way SDL_image 1.2 lays it out
SDL_Surface *liar_load_image(const uint8_t *data, size_t len);

// display.c: Wayland output and the pad, on a thread of their own
int display_open(const char *title);
void display_present(const uint16_t *pixels, int pitch);
uint32_t display_buttons(void); // bit N = Wiz button N, latched since the last call
int display_should_quit(void);
void display_wait_visible(void);
extern volatile int display_visible;

// audio.c
int audio_open(SDL_AudioSpec *spec);
void audio_pause(int on);
void audio_lock(void);
void audio_unlock(void);
void audio_close(void);

// drm.c
void *NED_fopen(const char *name, const char *mode);
size_t NED_fread(void *ptr, size_t size, size_t n, void *f);
int NED_fseek(void *f, long off, int whence);
int NED_fclose(void *f);
int NED_Initialize(int rand, void *time, int a, int b, void *code, int codelen);
int ND_Initialize(void);
int ND_Terminate(void);
int getcode(void *buf);
int getserial(void *buf);
int getrandnum(void *out);
int com_drm_time(void *out);
int liar_lzss(uint8_t *dst, size_t dstlen, const uint8_t *src, size_t srclen);
