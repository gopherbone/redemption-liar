// SPDX-License-Identifier: GPL-2.0-or-later
// A small loader for liar_e.gpe, the GP2X Wiz build of Redemption: Liar: a
// 32-bit old-ABI (OABI) ARM executable for a system nothing on the Brick can
// provide. It maps the user's own executable at its fixed address and resolves
// its imports to our code: libc through thin wrappers, SDL 1.2 to sdl.c, the
// DRM libraries to drm.c. The CPU runs the game natively in AArch32.
//
// What makes this possible with so little: the game imports only about 100
// functions, no double arithmetic, no libstdc++ beyond new/delete, no exceptions
// actually thrown, and no system calls of its own.
#define _GNU_SOURCE
#include "liar.h"
#include <alloca.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// sdl.c exports the rest of the table
typedef struct { const char *name; void *fn; int direct; } Import;
extern const Import liar_sdl_imports[];

void liar_thunk_common(void);
void liar_enter(uint32_t entry, void *sp) __attribute__((noreturn));

void liar_exit(int code) {
    fflush(NULL);
    sync();
    _exit(code);
}

// --- libgcc: old ABI, soft float, values in integer registers ---
static inline float f_of(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint32_t u_of(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static uint32_t i_addsf3(uint32_t a, uint32_t b) { return u_of(f_of(a) + f_of(b)); }
static uint32_t i_mulsf3(uint32_t a, uint32_t b) { return u_of(f_of(a) * f_of(b)); }
static uint32_t i_divsf3(uint32_t a, uint32_t b) { return u_of(f_of(a) / f_of(b)); }
static int i_fixsfsi(uint32_t a) { return (int)f_of(a); }
static uint32_t i_fixunssfsi(uint32_t a) { float f = f_of(a); return f <= 0 ? 0 : (uint32_t)f; }
static uint32_t i_floatsisf(int a) { return u_of((float)a); }
static int i_divsi3(int a, int b) { return b ? (a == INT32_MIN && b == -1 ? a : a / b) : 0; }
static int i_modsi3(int a, int b) { return b && b != -1 ? a % b : 0; }
static uint32_t i_udivsi3(uint32_t a, uint32_t b) { return b ? a / b : 0; }
static uint32_t i_umodsi3(uint32_t a, uint32_t b) { return b ? a % b : 0; }

// SjLj exceptions: functions register cleanups, nothing is ever thrown
// (operator new aborts rather than throwing).
static void i_unwind_noop(void *ctx) { (void)ctx; }
static void i_unwind_resume(void *ctx) { (void)ctx; fprintf(stderr, "liar: unexpected C++ exception\n"); abort(); }
static int i_personality(void) { abort(); }

static void *i_new(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "liar: out of memory (%zu bytes)\n", n);
        abort();
    }
    return p;
}

// Nothing runs at exit: liar_exit flushes and leaves.
static int i_cxa_atexit(void (*fn)(void *), void *arg, void *dso) { (void)fn; (void)arg; (void)dso; return 0; }

static int i_libc_start_main(int (*gmain)(int, char **, char **), int argc, char **argv,
                             void (*init)(int, char **, char **), void (*fini)(void),
                             void (*rtld_fini)(void), void *stack_end) {
    (void)rtld_fini; (void)stack_end;
    extern char **environ;
    (void)fini;
    if (init) init(argc, argv, environ);
    liar_exit(gmain(argc, argv, environ));
}

// The game leaves through system("sync"-like) and execlp(the Wiz menu). There
// is no menu to go back to: it just means "quit".
static int i_system(const char *cmd) { (void)cmd; return 0; }
static int i_execlp(const char *f, const char *a, ...) { (void)f; (void)a; liar_exit(0); }
static int i_chdir(const char *p) { (void)p; return 0; } // only ever "/usr/gp2x"
// /dev/sound/mixer: the Wiz's hardware volume. The shell owns the volume here.
static int i_open(const char *p, int fl, ...) { (void)p; (void)fl; errno = ENOENT; return -1; }
static int i_close(int fd) { (void)fd; return 0; }
static int i_ioctl(int fd, unsigned long r, ...) { (void)fd; (void)r; errno = ENOTTY; return -1; }
static int i_vsprintf(char *s, const char *fmt, void *ap) {
    // old ABI va_list is a plain pointer, and so is EABI's by value in a register
    union { void *p; va_list v; } u;
    memset(&u, 0, sizeof u);
    u.p = ap;
    return vsprintf(s, fmt, u.v);
}

// The game prints debug text to stdout every frame of a dialogue. Keep it quiet
// (it would all end up in the session log); its errors go to stderr.
static int i_printf(const char *f, ...) { (void)f; return 0; }
static int i_puts(const char *s) { (void)s; return 0; }

static const Import libc_imports[] = {
    // hot and stack-free: straight to the code, no thunk
    {"memcpy", memcpy, 1}, {"memset", memset, 1}, {"strlen", strlen, 1}, {"strcmp", strcmp, 1},
    {"strcpy", strcpy, 1}, {"strcat", strcat, 1},
    {"__divsi3", i_divsi3, 1}, {"__modsi3", i_modsi3, 1}, {"__udivsi3", i_udivsi3, 1}, {"__umodsi3", i_umodsi3, 1},
    {"__addsf3", i_addsf3, 1}, {"__mulsf3", i_mulsf3, 1}, {"__divsf3", i_divsf3, 1}, {"__fixsfsi", i_fixsfsi, 1},
    {"__fixunssfsi", i_fixunssfsi, 1}, {"__floatsisf", i_floatsisf, 1},
    {"_Unwind_SjLj_Register", i_unwind_noop, 1}, {"_Unwind_SjLj_Unregister", i_unwind_noop, 1},
    // everything else through the aligning thunk
    {"_Unwind_SjLj_Resume", i_unwind_resume, 0}, {"__gxx_personality_sj0", i_personality, 0},
    {"__libc_start_main", i_libc_start_main, 0}, {"__cxa_atexit", i_cxa_atexit, 0},
    {"_Znwj", i_new, 0}, {"_Znaj", i_new, 0}, {"_ZdlPv", free, 0}, {"_ZdaPv", free, 0},
    {"abort", abort, 0}, {"atoi", atoi, 0}, {"malloc", malloc, 0}, {"free", free, 0}, {"rand", rand, 0}, {"time", time, 0},
    {"chdir", i_chdir, 0}, {"open", i_open, 0}, {"close", i_close, 0}, {"ioctl", i_ioctl, 0},
    {"system", i_system, 0}, {"execlp", i_execlp, 0}, {"sync", sync, 0},
    {"fopen", fopen, 0}, {"fclose", fclose, 0}, {"fread", fread, 0}, {"fwrite", fwrite, 0}, {"fseek", fseek, 0},
    {"ftell", ftell, 0}, {"rewind", rewind, 0}, {"fputs", fputs, 0}, {"puts", i_puts, 0}, {"printf", i_printf, 0},
    {"fprintf", fprintf, 0}, {"sprintf", sprintf, 0}, {"vsprintf", i_vsprintf, 0}, {"strstr", strstr, 0},
    // the DRM layer: drm.c
    {"NED_Initialize", NED_Initialize, 0}, {"ND_Initialize", ND_Initialize, 0}, {"ND_Terminate", ND_Terminate, 0},
    {"NED_fopen", NED_fopen, 0}, {"NED_fread", NED_fread, 0}, {"NED_fseek", NED_fseek, 0}, {"NED_fclose", NED_fclose, 0},
    {"getcode", getcode, 0}, {"getserial", getserial, 0}, {"getrandnum", getrandnum, 0}, {"com_drm_time", com_drm_time, 0},
    {NULL, NULL, 0},
};

static void die(const char *what) {
    fprintf(stderr, "liar: %s\n", what);
    exit(1);
}

static const Import *find_import(const char *name) {
    for (const Import *t = libc_imports; t->name; t++)
        if (!strcmp(t->name, name)) return t;
    for (const Import *t = liar_sdl_imports; t->name; t++)
        if (!strcmp(t->name, name)) return t;
    return NULL;
}

// A thunk: ip = target; pc = liar_thunk_common.
#define THUNK_WORDS 4
static uint32_t *thunks;
static int nthunks;
static void *thunk_for(void *fn) {
    uint32_t *t = thunks + nthunks++ * THUNK_WORDS;
    t[0] = 0xe59fc000; // ldr ip, [pc, #0]    (t[2])
    t[1] = 0xe59ff000; // ldr pc, [pc, #0]    (t[3])
    t[2] = (uint32_t)fn;
    t[3] = (uint32_t)liar_thunk_common;
    return t;
}

static uint32_t resolve(const Elf32_Sym *sym, const char *strtab) {
    const char *name = strtab + sym->st_name;
    if (sym->st_shndx != SHN_UNDEF) return sym->st_value;
    const Import *im = find_import(name);
    if (im) return im->direct ? (uint32_t)im->fn : (uint32_t)thunk_for(im->fn);
    if (ELF32_ST_BIND(sym->st_info) == STB_WEAK) return 0;
    fprintf(stderr, "liar: the game needs %s, which this loader doesn't provide\n", name);
    exit(1);
}

static void do_rel(const Elf32_Rel *rel, size_t n, const Elf32_Sym *symtab, const char *strtab) {
    for (size_t i = 0; i < n; i++) {
        uint32_t *where = (uint32_t *)rel[i].r_offset;
        const Elf32_Sym *s = &symtab[ELF32_R_SYM(rel[i].r_info)];
        switch (ELF32_R_TYPE(rel[i].r_info)) {
        case R_ARM_GLOB_DAT:
        case R_ARM_JUMP_SLOT:
            *where = resolve(s, strtab);
            break;
        case R_ARM_COPY:
            if (!strcmp(strtab + s->st_name, "stderr")) *where = (uint32_t)stderr;
            else memset(where, 0, s->st_size); // __class_type_info's vtable: only RTTI would read it
            break;
        case R_ARM_RELATIVE:
            break; // fixed address
        case R_ARM_ABS32:
            *where += resolve(s, strtab);
            break;
        default:
            fprintf(stderr, "liar: unknown relocation %u\n", ELF32_R_TYPE(rel[i].r_info));
            exit(1);
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) die("usage: liar-run liar_e.gpe (run in the game's folder)");

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { perror(argv[1]); return 1; }
    struct stat st;
    fstat(fd, &st);
    uint8_t *file = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (file == MAP_FAILED) { perror("mmap"); return 1; }
    Elf32_Ehdr *eh = (Elf32_Ehdr *)file;
    if ((size_t)st.st_size < sizeof *eh || memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_ident[EI_CLASS] != ELFCLASS32 ||
        eh->e_machine != EM_ARM || eh->e_type != ET_EXEC)
        die("not a 32-bit ARM executable");

    Elf32_Phdr *ph = (Elf32_Phdr *)(file + eh->e_phoff);
    Elf32_Dyn *dyn = NULL;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) dyn = (Elf32_Dyn *)ph[i].p_vaddr;
        if (ph[i].p_type != PT_LOAD) continue;
        uint32_t start = ph[i].p_vaddr & ~0xfffu, end = (ph[i].p_vaddr + ph[i].p_memsz + 0xfff) & ~0xfffu;
        void *m = mmap((void *)start, end - start, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (m != (void *)start) { perror("mapping the game"); return 1; }
        memcpy((void *)ph[i].p_vaddr, file + ph[i].p_offset, ph[i].p_filesz);
    }
    if (!dyn) die("no dynamic section");

    const Elf32_Sym *symtab = NULL;
    const char *strtab = NULL;
    const Elf32_Rel *rel = NULL, *jmprel = NULL;
    size_t relsz = 0, pltsz = 0;
    for (Elf32_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_SYMTAB: symtab = (void *)d->d_un.d_ptr; break;
        case DT_STRTAB: strtab = (void *)d->d_un.d_ptr; break;
        case DT_REL: rel = (void *)d->d_un.d_ptr; break;
        case DT_RELSZ: relsz = d->d_un.d_val; break;
        case DT_JMPREL: jmprel = (void *)d->d_un.d_ptr; break;
        case DT_PLTRELSZ: pltsz = d->d_un.d_val; break;
        }
    }
    if (!symtab || !strtab) die("no symbols");

    size_t thunk_bytes = 400 * THUNK_WORDS * 4;
    thunks = mmap(NULL, thunk_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (thunks == MAP_FAILED) { perror("mmap"); return 1; }
    do_rel(rel, relsz / sizeof *rel, symtab, strtab);
    do_rel(jmprel, pltsz / sizeof *jmprel, symtab, strtab);
    __builtin___clear_cache((char *)thunks, (char *)thunks + thunk_bytes);
    mprotect(thunks, thunk_bytes, PROT_READ | PROT_EXEC);

    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || !(ph[i].p_flags & PF_X)) continue;
        uint32_t start = ph[i].p_vaddr & ~0xfffu, end = (ph[i].p_vaddr + ph[i].p_memsz + 0xfff) & ~0xfffu;
        __builtin___clear_cache((char *)start, (char *)end);
        if (mprotect((void *)start, end - start, PROT_READ | PROT_EXEC)) { perror("mprotect"); return 1; }
    }
    uint32_t entry = eh->e_entry;
    munmap(file, st.st_size);
    close(fd);

    // _start reads argc, argv and the environment off the stack it's entered on.
    extern char **environ;
    int nenv = 0;
    while (environ[nenv]) nenv++;
    uint32_t *frame = alloca((nenv + 8) * 4 + 8);
    frame = (uint32_t *)(((uintptr_t)frame + 7) & ~7u);
    int n = 0;
    frame[n++] = 1;
    frame[n++] = (uint32_t)argv[1];
    frame[n++] = 0;
    for (int i = 0; i < nenv; i++) frame[n++] = (uint32_t)environ[i];
    frame[n++] = 0;
    liar_enter(entry, frame);
}
