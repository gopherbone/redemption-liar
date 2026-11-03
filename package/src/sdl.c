// SPDX-License-Identifier: GPL-2.0-or-later
// The part of SDL 1.2 Redemption: Liar uses, with the structures and behaviour
// the game was built against. Surfaces and blits are done here, in software:
// a 32-bit process has no GPU on the Brick, and the game draws most of the
// screen itself into a 320x240 RGB565 surface anyway.
#define _GNU_SOURCE
#include "liar.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct { const char *name; void *fn; int direct; } Import;

static char last_error[128];
static SDL_Surface *screen;

static void mask_fields(uint32_t mask, uint8_t *shift, uint8_t *loss) {
    *shift = *loss = 0;
    if (!mask) { *loss = 8; return; }
    uint32_t m = mask;
    while (!(m & 1)) { m >>= 1; (*shift)++; }
    uint8_t bits = 0;
    while (m & 1) { m >>= 1; bits++; }
    *loss = bits >= 8 ? 0 : 8 - bits;
}

SDL_Surface *liar_new_surface(int w, int h, int depth, uint32_t R, uint32_t G, uint32_t B, uint32_t A) {
    SDL_Surface *s = calloc(1, sizeof *s);
    SDL_PixelFormat *f = calloc(1, sizeof *f);
    s->format = f;
    s->w = w;
    s->h = h;
    f->BitsPerPixel = depth;
    f->BytesPerPixel = (depth + 7) / 8;
    f->alpha = 255;
    if (depth == 8) {
        f->palette = calloc(1, sizeof *f->palette);
        f->palette->ncolors = 256;
        f->palette->colors = calloc(256, sizeof(SDL_Color));
    } else if (!R && !G && !B && depth > 8) {
        // SDL's default for a mask-less surface: R-G-B from the depth (565 at 16)
        int bpp = depth > 24 ? 24 : depth;
        int blue = bpp / 3, green = bpp / 3 + bpp % 3;
        B = (1u << blue) - 1;
        G = ((1u << green) - 1) << blue;
        R = ((1u << blue) - 1) << (blue + green);
    }
    f->Rmask = R; f->Gmask = G; f->Bmask = B; f->Amask = A;
    mask_fields(R, &f->Rshift, &f->Rloss);
    mask_fields(G, &f->Gshift, &f->Gloss);
    mask_fields(B, &f->Bshift, &f->Bloss);
    mask_fields(A, &f->Ashift, &f->Aloss);
    if (A) s->flags |= SDL_SRCALPHA;
    int pitch = (w * f->BytesPerPixel + 3) & ~3;
    s->pitch = pitch;
    s->pixels = calloc(h ? h : 1, pitch ? pitch : 1);
    s->clip_rect.w = w;
    s->clip_rect.h = h;
    s->refcount = 1;
    return s;
}

static SDL_Surface *i_CreateRGBSurface(uint32_t flags, int w, int h, int depth, uint32_t R, uint32_t G, uint32_t B, uint32_t A) {
    (void)flags;
    return liar_new_surface(w, h, depth, R, G, B, A);
}

static void i_FreeSurface(SDL_Surface *s) {
    if (!s || s == screen) return;
    if (s->format->palette) {
        free(s->format->palette->colors);
        free(s->format->palette);
    }
    free(s->format);
    free(s->pixels);
    free(s);
}

static uint32_t map_rgb(const SDL_PixelFormat *f, uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)(r >> f->Rloss) << f->Rshift) | ((uint32_t)(g >> f->Gloss) << f->Gshift) |
           ((uint32_t)(b >> f->Bloss) << f->Bshift) | f->Amask;
}
static uint32_t i_MapRGB(const SDL_PixelFormat *f, uint8_t r, uint8_t g, uint8_t b) { return map_rgb(f, r, g, b); }

static int i_SetColorKey(SDL_Surface *s, uint32_t flag, uint32_t key) {
    if (flag & SDL_SRCCOLORKEY) {
        s->flags |= SDL_SRCCOLORKEY;
        s->format->colorkey = key;
    } else {
        s->flags &= ~SDL_SRCCOLORKEY;
        s->format->colorkey = 0;
    }
    return 0;
}

static int i_SetAlpha(SDL_Surface *s, uint32_t flag, uint8_t alpha) {
    if (flag & SDL_SRCALPHA) {
        s->flags |= SDL_SRCALPHA;
        s->format->alpha = alpha;
    } else {
        s->flags &= ~SDL_SRCALPHA;
        s->format->alpha = 255;
    }
    return 0;
}

// --- pixels ---
static inline uint32_t read_pixel(const uint8_t *p, int bpp) {
    switch (bpp) {
    case 1: return p[0];
    case 2: return *(const uint16_t *)p;
    case 3: return p[0] | (p[1] << 8) | (p[2] << 16);
    default: return *(const uint32_t *)p;
    }
}
static inline void write_pixel(uint8_t *p, int bpp, uint32_t v) {
    switch (bpp) {
    case 1: p[0] = v; break;
    case 2: *(uint16_t *)p = v; break;
    case 3: p[0] = v; p[1] = v >> 8; p[2] = v >> 16; break;
    default: *(uint32_t *)p = v; break;
    }
}
// Expands to 8 bits the way SDL 1.2's RGB_FROM_PIXEL does (low bits zero).
static inline void get_rgba(const SDL_PixelFormat *f, uint32_t v, uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a) {
    if (f->palette) {
        const SDL_Color *c = &f->palette->colors[v & 255];
        *r = c->r; *g = c->g; *b = c->b; *a = 255;
        return;
    }
    *r = ((v & f->Rmask) >> f->Rshift) << f->Rloss;
    *g = ((v & f->Gmask) >> f->Gshift) << f->Gloss;
    *b = ((v & f->Bmask) >> f->Bshift) << f->Bloss;
    *a = f->Amask ? ((v & f->Amask) >> f->Ashift) << f->Aloss : 255;
}

// 565 + 565 at a 5-bit alpha, the way SDL 1.2's Blit565to565SurfaceAlpha does it.
static inline uint16_t blend565(uint32_t s, uint32_t d, uint32_t a5) {
    s = (s | (s << 16)) & 0x07e0f81f;
    d = (d | (d << 16)) & 0x07e0f81f;
    d += ((s - d) * a5) >> 5;
    d &= 0x07e0f81f;
    return (uint16_t)(d | (d >> 16));
}

static int is_565(const SDL_PixelFormat *f) {
    return f->BytesPerPixel == 2 && f->Rmask == 0xf800 && f->Gmask == 0x07e0 && f->Bmask == 0x001f;
}

static void blit(SDL_Surface *src, int sx, int sy, SDL_Surface *dst, int dx, int dy, int w, int h) {
    const SDL_PixelFormat *sf = src->format, *df = dst->format;
    int sbpp = sf->BytesPerPixel, dbpp = df->BytesPerPixel;
    const uint8_t *sp = (const uint8_t *)src->pixels + sy * src->pitch + sx * sbpp;
    uint8_t *dp = (uint8_t *)dst->pixels + dy * dst->pitch + dx * dbpp;
    int spitch = src->pitch, dpitch = dst->pitch;
    int key = (src->flags & SDL_SRCCOLORKEY) != 0;
    uint32_t ck = sf->colorkey;
    int surface_alpha = (src->flags & SDL_SRCALPHA) && !sf->Amask && sf->alpha != 255;
    int pixel_alpha = (src->flags & SDL_SRCALPHA) && sf->Amask;

    if (is_565(sf) && is_565(df)) {
        if (!key && !surface_alpha) { // plain copy (may be the same surface)
            if (sp < dp) {
                sp += (h - 1) * spitch;
                dp += (h - 1) * dpitch;
                spitch = -spitch;
                dpitch = -dpitch;
            }
            for (int y = 0; y < h; y++, sp += spitch, dp += dpitch) memmove(dp, sp, w * 2);
            return;
        }
        uint32_t a5 = sf->alpha >> 3;
        for (int y = 0; y < h; y++, sp += spitch, dp += dpitch) {
            const uint16_t *s = (const uint16_t *)sp;
            uint16_t *d = (uint16_t *)dp;
            if (surface_alpha) {
                for (int x = 0; x < w; x++)
                    if (!key || s[x] != ck) d[x] = blend565(s[x], d[x], a5);
            } else {
                for (int x = 0; x < w; x++)
                    if (s[x] != ck) d[x] = s[x];
            }
        }
        return;
    }
    // everything else (images not converted to the display format)
    for (int y = 0; y < h; y++, sp += spitch, dp += dpitch) {
        const uint8_t *s = sp;
        uint8_t *d = dp;
        for (int x = 0; x < w; x++, s += sbpp, d += dbpp) {
            uint32_t v = read_pixel(s, sbpp);
            if (key && v == ck) continue;
            uint8_t r, g, b, a;
            get_rgba(sf, v, &r, &g, &b, &a);
            if (surface_alpha) a = sf->alpha;
            else if (!pixel_alpha) a = 255;
            if (!a) continue;
            if (a == 255) {
                write_pixel(d, dbpp, map_rgb(df, r, g, b));
            } else if (is_565(df)) {
                *(uint16_t *)d = blend565(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3), *(uint16_t *)d, a >> 3);
            } else {
                uint8_t dr, dg, db, da;
                get_rgba(df, read_pixel(d, dbpp), &dr, &dg, &db, &da);
                write_pixel(d, dbpp, map_rgb(df, dr + ((r - dr) * a >> 8), dg + ((g - dg) * a >> 8), db + ((b - db) * a >> 8)));
            }
        }
    }
}

// SDL_UpperBlit's clipping, which writes the final rectangle back to dstrect.
static int i_UpperBlit(SDL_Surface *src, SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect) {
    SDL_Rect full = {0, 0, 0, 0};
    if (!src || !dst) return -1;
    if (!dstrect) dstrect = &full;
    int srcx = 0, srcy = 0, w = src->w, h = src->h;
    if (srcrect) {
        srcx = srcrect->x;
        w = srcrect->w;
        if (srcx < 0) { w += srcx; dstrect->x -= srcx; srcx = 0; }
        if (src->w - srcx < w) w = src->w - srcx;
        srcy = srcrect->y;
        h = srcrect->h;
        if (srcy < 0) { h += srcy; dstrect->y -= srcy; srcy = 0; }
        if (src->h - srcy < h) h = src->h - srcy;
    }
    const SDL_Rect *c = &dst->clip_rect;
    int d = c->x - dstrect->x;
    if (d > 0) { w -= d; dstrect->x += d; srcx += d; }
    d = dstrect->x + w - c->x - c->w;
    if (d > 0) w -= d;
    d = c->y - dstrect->y;
    if (d > 0) { h -= d; dstrect->y += d; srcy += d; }
    d = dstrect->y + h - c->y - c->h;
    if (d > 0) h -= d;
    if (w > 0 && h > 0) {
        dstrect->w = w;
        dstrect->h = h;
        blit(src, srcx, srcy, dst, dstrect->x, dstrect->y, w, h);
    } else {
        dstrect->w = dstrect->h = 0;
    }
    return 0;
}

static int i_FillRect(SDL_Surface *s, SDL_Rect *r, uint32_t color) {
    int x = s->clip_rect.x, y = s->clip_rect.y, w = s->clip_rect.w, h = s->clip_rect.h;
    if (r) {
        int x2 = x + w, y2 = y + h;
        if (r->x > x) x = r->x;
        if (r->y > y) y = r->y;
        if (r->x + r->w < x2) x2 = r->x + r->w;
        if (r->y + r->h < y2) y2 = r->y + r->h;
        w = x2 - x;
        h = y2 - y;
        if (w <= 0 || h <= 0) { r->w = r->h = 0; return 0; }
        r->x = x; r->y = y; r->w = w; r->h = h;
    }
    int bpp = s->format->BytesPerPixel;
    uint8_t *row = (uint8_t *)s->pixels + y * s->pitch + x * bpp;
    for (int j = 0; j < h; j++, row += s->pitch) {
        if (bpp == 2) {
            uint16_t *p = (uint16_t *)row;
            for (int i = 0; i < w; i++) p[i] = color;
        } else {
            for (int i = 0; i < w; i++) write_pixel(row + i * bpp, bpp, color);
        }
    }
    return 0;
}

static SDL_Surface *i_DisplayFormat(SDL_Surface *src) {
    SDL_Surface *d = liar_new_surface(src->w, src->h, 16, 0xf800, 0x07e0, 0x001f, 0);
    d->flags |= src->flags & SDL_SRCALPHA;
    d->format->alpha = src->format->alpha;
    uint32_t keep = src->flags;
    src->flags &= ~(SDL_SRCALPHA | SDL_SRCCOLORKEY); // convert, don't composite
    blit(src, 0, 0, d, 0, 0, src->w, src->h);
    src->flags = keep;
    return d;
}

static int i_LockSurface(SDL_Surface *s) { s->locked++; return 0; }
static void i_UnlockSurface(SDL_Surface *s) { if (s->locked) s->locked--; }

// --- video and events ---
static uint32_t i_Init(uint32_t flags) { (void)flags; return 0; }
static char caption[64] = "Redemption: Liar";
static void i_WM_SetCaption(const char *t, const char *i) { (void)t; (void)i; }
static int i_ShowCursor(int on) { (void)on; return 0; }

static SDL_Surface *i_SetVideoMode(int w, int h, int bpp, uint32_t flags) {
    (void)flags;
    if (!screen) {
        if (bpp != 16 || display_open(caption)) {
            snprintf(last_error, sizeof last_error, "no display");
            return NULL;
        }
        screen = liar_new_surface(w, h, 16, 0xf800, 0x07e0, 0x001f, 0);
    }
    return screen;
}

// The Wiz's panel timing switch. Nothing to do here.
static int i_SetLcdChange(int a, int b) { (void)a; (void)b; return 0; }

static void i_UpdateRect(SDL_Surface *s, int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
    if (s == screen) display_present(s->pixels, s->pitch);
}

static void i_Quit(void) { audio_close(); }

// Events: the game only ever looks at the joystick. Polling is also its way of
// saying it's between frames, the safe moment to quit, or to wait off-screen.
static int i_PollEvent(void *ev) {
    if (display_should_quit()) liar_exit(0);
    if (!display_visible) display_wait_visible();
    if (ev) memset(ev, 0, 20);
    return 0;
}

static uint32_t buttons;
static void *i_JoystickOpen(int n) { (void)n; return &buttons; }
static void i_JoystickUpdate(void) { buttons = display_buttons(); }
static uint8_t i_JoystickGetButton(void *j, int b) { (void)j; return (buttons >> b) & 1; }

// --- time ---
static uint32_t ticks_now(void) {
    static struct timespec t0;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    if (!t0.tv_sec) t0 = t;
    return (t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000;
}

// The main loop spins on this waiting for the next frame's time. A millisecond
// is far finer than its frames (25 ms and up), so give the time back: one idle
// core is the difference between hot and cool hands on a Brick.
static uint32_t i_GetTicks(void) {
    static uint32_t last;
    uint32_t t = ticks_now();
    if (t == last) {
        struct timespec ts = {0, 500000};
        nanosleep(&ts, NULL);
        t = ticks_now();
    }
    last = t;
    return t;
}

static void i_Delay(uint32_t ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000};
    nanosleep(&ts, NULL);
}

static const char *i_GetError(void) { return last_error; }

// --- memory files ---
static int rw_seek(SDL_RWops *c, int off, int whence) {
    uint8_t *p = whence == 0 ? c->mem.base : whence == 1 ? c->mem.here : c->mem.stop;
    p += off;
    if (p < c->mem.base) p = c->mem.base;
    if (p > c->mem.stop) p = c->mem.stop;
    c->mem.here = p;
    return (int)(p - c->mem.base);
}
static int rw_read(SDL_RWops *c, void *ptr, int size, int maxnum) {
    if (size <= 0 || maxnum <= 0) return 0;
    long total = (long)size * maxnum, left = c->mem.stop - c->mem.here;
    if (total > left) total = left;
    memcpy(ptr, c->mem.here, total);
    c->mem.here += total;
    return (int)(total / size);
}
static int rw_write(SDL_RWops *c, const void *ptr, int size, int num) {
    long total = (long)size * num, left = c->mem.stop - c->mem.here;
    if (total > left) total = left;
    memcpy(c->mem.here, ptr, total);
    c->mem.here += total;
    return size ? (int)(total / size) : 0;
}
static int rw_close(SDL_RWops *c) { free(c); return 0; }

static SDL_RWops *i_RWFromMem(void *mem, int size) {
    SDL_RWops *c = calloc(1, sizeof *c);
    c->seek = rw_seek;
    c->read = rw_read;
    c->write = rw_write;
    c->close = rw_close;
    c->mem.base = c->mem.here = mem;
    c->mem.stop = c->mem.base + size;
    return c;
}

static SDL_Surface *i_IMG_Load_RW(SDL_RWops *c, int freesrc) {
    SDL_Surface *s = c ? liar_load_image(c->mem.here, c->mem.stop - c->mem.here) : NULL;
    if (!s) snprintf(last_error, sizeof last_error, "bad image");
    if (c && freesrc) c->close(c);
    return s;
}

// --- sound ---
static SDL_AudioSpec *i_LoadWAV_RW(SDL_RWops *c, int freesrc, SDL_AudioSpec *spec, uint8_t **buf, uint32_t *len) {
    SDL_AudioSpec *ok = NULL;
    const uint8_t *p = c ? c->mem.here : NULL, *end = c ? c->mem.stop : NULL;
    int have_fmt = 0;
    if (c && end - p >= 12 && !memcmp(p, "RIFF", 4) && !memcmp(p + 8, "WAVE", 4)) {
        p += 12;
        while (end - p >= 8) {
            uint32_t n;
            memcpy(&n, p + 4, 4);
            const uint8_t *body = p + 8;
            if (n > (uint32_t)(end - body)) n = end - body;
            if (!memcmp(p, "fmt ", 4) && n >= 16) {
                uint16_t tag = body[0] | (body[1] << 8), bits = body[14] | (body[15] << 8);
                if (tag != 1 || (bits != 8 && bits != 16)) break;
                memset(spec, 0, sizeof *spec);
                spec->channels = body[2];
                memcpy(&spec->freq, body + 4, 4);
                spec->format = bits == 8 ? AUDIO_U8 : AUDIO_S16LSB;
                spec->samples = 4096;
                have_fmt = 1;
            } else if (!memcmp(p, "data", 4) && have_fmt) {
                *buf = malloc(n ? n : 1);
                memcpy(*buf, body, n);
                *len = n;
                ok = spec;
                break;
            }
            p = body + n + (n & 1);
        }
    }
    if (!ok) snprintf(last_error, sizeof last_error, "bad wav");
    if (c && freesrc) c->close(c);
    return ok;
}
static void i_FreeWAV(uint8_t *p) { free(p); }

// Build + Convert: any format to the mixer's. filters[] holds what we need to know.
enum { F_SRC_RATE, F_SRC_CH, F_DST_RATE, F_DST_CH };
static int i_BuildAudioCVT(SDL_AudioCVT *cvt, uint16_t sfmt, uint8_t sch, int srate, uint16_t dfmt, uint8_t dch, int drate) {
    memset(cvt, 0, sizeof *cvt);
    cvt->src_format = sfmt;
    cvt->dst_format = dfmt;
    cvt->filters[F_SRC_RATE] = srate;
    cvt->filters[F_SRC_CH] = sch;
    cvt->filters[F_DST_RATE] = drate;
    cvt->filters[F_DST_CH] = dch;
    int sbytes = (sfmt & 0xff) / 8, dbytes = (dfmt & 0xff) / 8;
    // how many times bigger the game must make the buffer, rounded up
    int64_t num = (int64_t)dbytes * dch * drate, den = (int64_t)sbytes * sch * srate;
    cvt->len_mult = (int)((num + den - 1) / den) + 1;
    cvt->needed = 1;
    return 1;
}

static int i_ConvertAudio(SDL_AudioCVT *cvt) {
    int srate = cvt->filters[F_SRC_RATE], sch = cvt->filters[F_SRC_CH];
    int drate = cvt->filters[F_DST_RATE], dch = cvt->filters[F_DST_CH];
    int sbytes = (cvt->src_format & 0xff) / 8;
    int frames = cvt->len / (sbytes * sch);
    int16_t *mono = malloc((frames ? frames : 1) * sizeof *mono);
    const uint8_t *in = cvt->buf;
    for (int i = 0; i < frames; i++) {
        int sum = 0;
        for (int c = 0; c < sch; c++, in += sbytes)
            sum += sbytes == 1 ? (in[0] - 128) << 8 : (int16_t)(in[0] | (in[1] << 8));
        mono[i] = sum / sch;
    }
    int64_t out_frames = frames > 0 ? (int64_t)frames * drate / srate : 0;
    int16_t *out = (int16_t *)cvt->buf;
    // Linear interpolation; forwards in place only works when not shrinking,
    // so go through a copy.
    int16_t *tmp = malloc((out_frames ? out_frames : 1) * sizeof *tmp);
    for (int64_t i = 0; i < out_frames; i++) {
        int64_t pos = i * srate;
        int idx = (int)(pos / drate), frac = (int)(pos % drate);
        int a = mono[idx], b = idx + 1 < frames ? mono[idx + 1] : a;
        tmp[i] = a + (int)((int64_t)(b - a) * frac / drate);
    }
    for (int64_t i = 0; i < out_frames; i++)
        for (int c = 0; c < dch; c++) *out++ = tmp[i];
    free(tmp);
    free(mono);
    cvt->len_cvt = (int)(out_frames * 2 * dch);
    return 0;
}

// SDL_MixAudio for the signed 16-bit stream the game opens; volume 0-128.
static void i_MixAudio(uint8_t *dst, const uint8_t *src, uint32_t len, int vol) {
    if (!vol) return;
    int16_t *d = (int16_t *)dst;
    const int16_t *s = (const int16_t *)src;
    for (uint32_t i = 0; i < len / 2; i++) {
        int v = d[i] + (s[i] * vol) / 128;
        d[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
    }
}

static int i_OpenAudio(SDL_AudioSpec *want, SDL_AudioSpec *have) {
    if (audio_open(want)) {
        snprintf(last_error, sizeof last_error, "no audio");
        return -1;
    }
    if (have) *have = *want;
    return 0;
}

const Import liar_sdl_imports[] = {
    {"SDL_Init", i_Init, 0}, {"SDL_Quit", i_Quit, 0}, {"SDL_GetError", i_GetError, 0},
    {"SDL_WM_SetCaption", i_WM_SetCaption, 0}, {"SDL_ShowCursor", i_ShowCursor, 0},
    {"SDL_SetVideoMode", i_SetVideoMode, 0}, {"SDL_SetLcdChange", i_SetLcdChange, 0}, {"SDL_UpdateRect", i_UpdateRect, 0},
    {"SDL_CreateRGBSurface", i_CreateRGBSurface, 0}, {"SDL_FreeSurface", i_FreeSurface, 0},
    {"SDL_DisplayFormat", i_DisplayFormat, 0}, {"SDL_MapRGB", i_MapRGB, 0},
    {"SDL_SetColorKey", i_SetColorKey, 0}, {"SDL_SetAlpha", i_SetAlpha, 0},
    {"SDL_UpperBlit", i_UpperBlit, 0}, {"SDL_FillRect", i_FillRect, 0},
    {"SDL_LockSurface", i_LockSurface, 0}, {"SDL_UnlockSurface", i_UnlockSurface, 0},
    {"SDL_PollEvent", i_PollEvent, 0}, {"SDL_JoystickOpen", i_JoystickOpen, 0},
    {"SDL_JoystickUpdate", i_JoystickUpdate, 0}, {"SDL_JoystickGetButton", i_JoystickGetButton, 0},
    {"SDL_GetTicks", i_GetTicks, 0}, {"SDL_Delay", i_Delay, 0},
    {"SDL_RWFromMem", i_RWFromMem, 0}, {"IMG_Load_RW", i_IMG_Load_RW, 0},
    {"SDL_LoadWAV_RW", i_LoadWAV_RW, 0}, {"SDL_FreeWAV", i_FreeWAV, 0},
    {"SDL_BuildAudioCVT", i_BuildAudioCVT, 0}, {"SDL_ConvertAudio", i_ConvertAudio, 0},
    {"SDL_MixAudio", i_MixAudio, 0}, {"SDL_OpenAudio", i_OpenAudio, 0}, {"SDL_PauseAudio", audio_pause, 0},
    {"SDL_LockAudio", audio_lock, 0}, {"SDL_UnlockAudio", audio_unlock, 0}, {"SDL_CloseAudio", audio_close, 0},
    {NULL, NULL, 0},
};
