// SPDX-License-Identifier: GPL-2.0-or-later
// IMG_Load: the BMPs (8, 16, 24 and 32 bit, uncompressed) and RGBA PNGs in the
// game's archive, as surfaces with the masks SDL_image 1.2 gives them.
#include "liar.h"
#include <stdlib.h>
#include <string.h>

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint32_t le16(const uint8_t *p) { return p[0] | (p[1] << 8); }

static SDL_Surface *load_bmp(const uint8_t *d, size_t len) {
    if (len < 54) return NULL;
    uint32_t bits_off = le32(d + 10), hdr = le32(d + 14);
    int w = (int)le32(d + 18), h = (int)le32(d + 22);
    int bpp = le16(d + 28);
    uint32_t comp = le32(d + 30), colors = le32(d + 46);
    if (hdr < 40 || w <= 0 || h == 0 || comp != 0) return NULL;
    int top_down = h < 0;
    if (top_down) h = -h;
    uint32_t R = 0, G = 0, B = 0, A = 0;
    switch (bpp) {
    case 8: break;
    case 16: R = 0x7c00; G = 0x03e0; B = 0x001f; break;
    case 24: R = 0xff0000; G = 0xff00; B = 0xff; break;
    case 32: R = 0xff0000; G = 0xff00; B = 0xff; A = 0xff000000; break;
    default: return NULL;
    }
    size_t stride = (((size_t)w * bpp + 31) / 32) * 4;
    if (bits_off > len || stride * h > len - bits_off) return NULL;
    SDL_Surface *s = liar_new_surface(w, h, bpp, R, G, B, A);
    if (bpp == 8) {
        if (!colors) colors = 256;
        const uint8_t *pal = d + 14 + hdr;
        for (uint32_t i = 0; i < colors && i < 256 && pal + 4 * i + 3 < d + len; i++) {
            SDL_Color *c = &s->format->palette->colors[i];
            c->b = pal[4 * i];
            c->g = pal[4 * i + 1];
            c->r = pal[4 * i + 2];
        }
    }
    size_t row = (size_t)w * s->format->BytesPerPixel;
    for (int y = 0; y < h; y++) {
        const uint8_t *from = d + bits_off + (size_t)(top_down ? y : h - 1 - y) * stride;
        memcpy((uint8_t *)s->pixels + (size_t)y * s->pitch, from, row);
    }
    return s;
}

SDL_Surface *liar_load_image(const uint8_t *d, size_t len) {
    if (len >= 2 && d[0] == 'B' && d[1] == 'M') return load_bmp(d, len);
    int w, h, n;
    stbi_uc *px = stbi_load_from_memory(d, (int)len, &w, &h, &n, 4);
    if (!px) return NULL;
    SDL_Surface *s = liar_new_surface(w, h, 32, 0xff, 0xff00, 0xff0000, 0xff000000);
    for (int y = 0; y < h; y++) memcpy((uint8_t *)s->pixels + y * s->pitch, px + y * w * 4, (size_t)w * 4);
    stbi_image_free(px);
    return s;
}
