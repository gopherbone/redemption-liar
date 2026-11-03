// SPDX-License-Identifier: GPL-2.0-or-later
// In place of libinkadrm.so.0 and libdrmcode.so.0 (INKA NetSync DRM). On the
// Wiz these read the card's serial and hidden sectors, a device EEPROM and a
// licence, and refuse to run on anything else. The game only asks whether that
// went well, so the Initialize calls say yes and nothing else is checked.
//
// One thing the real library does matter: of the game's two archives, liar.dat
// is plain but ext.dat (the map scripts) is stored as a "NETSYNCDRM" file. Its
// body starts after a 512-byte header, and in every 512-byte sector
//   - bytes 0-15 are XORed with the first 16 bytes of an RC4 keystream, restarted
//     for each sector (the key comes out of the licence);
//   - bytes 32-47, 64-79, 96-111 and 128-143 are XORed with bytes 0-15 of the
//     decrypted sector.
// We hold no key and need none. The keystream is 16 fixed bytes, and ext.inx,
// which is not encrypted, starts with a copy of the file record that opens the
// body: XOR the two and the keystream falls out of the user's own files. The
// result is checked against the sizes in the index (see check_entry).
#define _GNU_SOURCE
#include "liar.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Index layout, shared with `mortar liar`: count, then parallel tables.
#define IDX_NAMES 0x4
#define IDX_OFFSET 0x60c04
#define IDX_PACKED 0x63c64
#define IDX_SIZE 0x66cc4

typedef struct { uint8_t *data; size_t len, pos; } MemFile;

// The game's own LZSS (window 4096, starting at 0xfee, spaces), as in the index.
int liar_lzss(uint8_t *dst, size_t dstlen, const uint8_t *src, size_t srclen) {
    uint8_t win[4096];
    memset(win, ' ', sizeof win);
    size_t i = 0, o = 0;
    unsigned r = 0xfee, flags = 0;
    for (;;) {
        flags >>= 1;
        if (!(flags & 256)) {
            if (i >= srclen) break;
            flags = src[i++] | 0xff00;
        }
        if (flags & 1) {
            if (i >= srclen) break;
            uint8_t c = src[i++];
            if (o < dstlen) dst[o] = c;
            o++;
            win[r] = c;
            r = (r + 1) & 0xfff;
        } else {
            if (i + 1 >= srclen) break;
            unsigned a = src[i], b = src[i + 1];
            i += 2;
            unsigned off = a | ((b & 0xf0) << 4), n = (b & 15) + 3;
            for (unsigned k = 0; k < n; k++) {
                uint8_t c = win[(off + k) & 0xfff];
                if (o < dstlen) dst[o] = c;
                o++;
                win[r] = c;
                r = (r + 1) & 0xfff;
            }
        }
    }
    return (int)o;
}

static uint8_t *slurp(const char *name, size_t *len) {
    FILE *f = fopen(name, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    uint8_t *p = malloc(n + 1);
    if (p && fread(p, 1, n, f) != (size_t)n) { free(p); p = NULL; }
    fclose(f);
    *len = n;
    return p;
}

// The decoded index for an archive (x.dat -> x.inx).
static uint8_t *load_index(const char *dat, size_t *len) {
    char name[256];
    size_t raw_len;
    snprintf(name, sizeof name, "%s", dat);
    char *dot = strrchr(name, '.');
    if (!dot) return NULL;
    strcpy(dot, ".inx");
    uint8_t *raw = slurp(name, &raw_len);
    if (!raw) return NULL;
    uint32_t packed, size;
    memcpy(&packed, raw, 4);
    memcpy(&size, raw + 4, 4);
    uint8_t *t = (packed + 8 <= raw_len && size <= (1u << 24)) ? malloc(size) : NULL;
    if (t && liar_lzss(t, size, raw + 8, packed) != (int)size) { free(t); t = NULL; }
    free(raw);
    *len = size;
    return t;
}

static int check_entry(const uint8_t *body, size_t blen, const uint8_t *idx, size_t idx_len) {
    if (idx_len < IDX_SIZE + 4) return 0;
    uint32_t off, packed, size;
    memcpy(&off, idx + IDX_OFFSET, 4);
    memcpy(&packed, idx + IDX_PACKED, 4);
    memcpy(&size, idx + IDX_SIZE, 4);
    if ((uint64_t)off + packed > blen || size > (1u << 26)) return 0;
    uint8_t *tmp = malloc(size);
    int ok = tmp && liar_lzss(tmp, size, body + off, packed) == (int)size;
    free(tmp);
    return ok;
}

// Decrypt a NETSYNCDRM file's body in place; returns its offset and length.
static int ned_unlock(MemFile *m, const char *name) {
    size_t idx_len = 0;
    uint8_t *idx = load_index(name, &idx_len);
    if (!idx || idx_len < IDX_NAMES + 16 || m->len < 512 + 512) {
        fprintf(stderr, "liar: %s: its index is missing or unreadable\n", name);
        free(idx);
        return -1;
    }
    uint8_t *body = m->data + 512;
    size_t blen = m->len - 512;
    uint8_t ks[16];
    for (int i = 0; i < 16; i++) ks[i] = body[i] ^ idx[IDX_NAMES + i];
    for (size_t s = 0; s + 512 <= blen; s += 512) {
        uint8_t *p = body + s;
        for (int i = 0; i < 16; i++) p[i] ^= ks[i];
        for (int b = 2; b <= 8; b += 2)
            for (int i = 0; i < 16; i++) p[b * 16 + i] ^= p[i];
    }
    int ok = check_entry(body, blen, idx, idx_len);
    free(idx);
    if (!ok) {
        fprintf(stderr, "liar: %s doesn't decode: is this copy complete?\n", name);
        return -1;
    }
    m->data = body;
    m->len = blen;
    return 0;
}

// One decrypted copy per archive: the game reopens it for every script it loads.
static struct { char name[64]; uint8_t *data; size_t len; } cache[4];

void *NED_fopen(const char *name, const char *mode) {
    (void)mode;
    MemFile *m = calloc(1, sizeof *m);
    for (int i = 0; i < 4; i++)
        if (cache[i].data && !strcmp(cache[i].name, name)) {
            m->data = cache[i].data;
            m->len = cache[i].len;
            return m;
        }
    uint8_t *raw = slurp(name, &m->len);
    if (!raw) { free(m); return NULL; }
    m->data = raw;
    if (m->len >= 10 && !memcmp(raw, "NETSYNCDRM", 10) && ned_unlock(m, name)) {
        free(raw);
        free(m);
        return NULL;
    }
    for (int i = 0; i < 4; i++)
        if (!cache[i].data) {
            snprintf(cache[i].name, sizeof cache[i].name, "%s", name);
            cache[i].data = m->data;
            cache[i].len = m->len;
            break;
        }
    return m;
}

size_t NED_fread(void *ptr, size_t size, size_t n, void *f) {
    MemFile *m = f;
    if (!m || !size) return 0;
    size_t want = size * n, left = m->len > m->pos ? m->len - m->pos : 0;
    if (want > left) want = left;
    memcpy(ptr, m->data + m->pos, want);
    m->pos += want;
    return want / size;
}

int NED_fseek(void *f, long off, int whence) {
    MemFile *m = f;
    if (!m) return -1;
    long base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (long)m->pos : (long)m->len;
    if (base + off < 0) return -1;
    m->pos = base + off;
    return 0;
}

int NED_fclose(void *f) {
    free(f);
    return 0;
}

// main() runs the game only if these two return 0.
int NED_Initialize(int r, void *t, int a, int b, void *code, int codelen) {
    (void)r; (void)t; (void)a; (void)b; (void)code; (void)codelen;
    return 0;
}
int ND_Initialize(void) { return 0; }
int ND_Terminate(void) { return 0; }
int getcode(void *buf) { (void)buf; return 0; }
int getserial(void *buf) { (void)buf; return 0; }
int getrandnum(void *out) { *(int *)out = 0; return 0; }
int com_drm_time(void *out) { (void)out; return 0; }
