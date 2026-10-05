// See kl_glibc.h.
//
// Binding policy, for the record, because it is NOT glibc's:
//
//   - interposers, then Klepton's (bionic-shaped) shims, then the other guest
//     images, first match wins (kl_image.c sym_value). Linux binds in global
//     scope order — executable first, then its DT_NEEDED breadth-first — so on
//     Linux libmimalloc's malloc/free (hlvr's first DT_NEEDED) interpose every
//     library's. Here the shim's malloc wins instead, which is the safer
//     answer: one allocator for guest and host alike.
//   - symbol versions are ignored: a dynsym name is bare, and `memcpy@GLIBC_2.17`
//     binds to whatever answers `memcpy`. Fine until two versions of one name
//     differ in ABI (none in this guest's import list so far).
//   - a name the shim answers is "resolved" in the gap below even when its
//     bionic implementation does not match glibc's ABI (struct stat, FILE,
//     pthread_mutex_t sizes, errno through __errno_location). The gap is the
//     list of names with NO answer; the ABI audit of the answered ones is the
//     next phase's work.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "klepton.h"
#include "kl_env.h"
#include "kl_glibc.h"
#include "kl_tls.h"

static char      g_err[512];
static char      g_target[64];
static char      g_entry[1024];
static kl_image *g_entry_img;
static unsigned  g_failed;           // roots that did not map/relocate

static int fail(const char *fmt, const char *a) {
    snprintf(g_err, sizeof g_err, fmt, a ? a : "");
    return 1;
}
const char *kl_glibc_error(void) { return g_err; }

// ---- the search path and its soname index ----------------------------------

#define MAX_DIRS 8
static char g_dirs[MAX_DIRS][1024];
static int  g_ndirs, g_nmoddirs;     // the first g_nmoddirs are game dirs

typedef struct { char path[1280]; char file[256]; char soname[128]; int dir; } ent;
static ent     *g_idx;
static unsigned g_nidx, g_capidx;

static void add_dirs(const char *list, int *count) {
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", list);
    for (char *save = NULL, *t = strtok_r(buf, ":", &save); t && g_ndirs < MAX_DIRS;
         t = strtok_r(NULL, ":", &save)) {
        if (!*t) continue;
        snprintf(g_dirs[g_ndirs++], sizeof g_dirs[0], "%s", t);
        if (count) (*count)++;
    }
}

static int is_elf(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    unsigned char m[4] = {0};
    ssize_t n = read(fd, m, 4);
    close(fd);
    return n == 4 && !memcmp(m, "\x7f" "ELF", 4);
}

static int ent_cmp(const void *a, const void *b) {
    const ent *x = a, *y = b;
    if (x->dir != y->dir) return x->dir - y->dir;
    return strcmp(x->file, y->file);
}

static void build_index(void) {
    for (int d = 0; d < g_ndirs; d++) {
        DIR *dir = opendir(g_dirs[d]);
        if (!dir) continue;
        struct dirent *de;
        while ((de = readdir(dir))) {
            if (de->d_name[0] == '.') continue;
            char p[1280];
            snprintf(p, sizeof p, "%s/%s", g_dirs[d], de->d_name);
            struct stat sb;
            if (stat(p, &sb) != 0 || !S_ISREG(sb.st_mode) || !sb.st_size) continue;
            if (!is_elf(p)) continue;
            if (g_nidx == g_capidx) {
                g_capidx = g_capidx ? g_capidx * 2 : 256;
                g_idx = realloc(g_idx, g_capidx * sizeof *g_idx);
            }
            ent *e = &g_idx[g_nidx++];
            snprintf(e->path, sizeof e->path, "%s", p);
            snprintf(e->file, sizeof e->file, "%s", de->d_name);
            if (kl_elf_soname(p, e->soname, sizeof e->soname) != 0) e->soname[0] = 0;
            e->dir = d;
        }
        closedir(dir);
    }
    // Directory order first (the game tree shadows the sysroot), then by name,
    // so which file answers a soname does not depend on readdir's order.
    qsort(g_idx, g_nidx, sizeof *g_idx, ent_cmp);
}

// glibc itself. Never loaded: these names are what Klepton's shims stand in
// for, and loading the real libc.so.6 would mean a second libc, a second
// malloc and a dynamic linker that expects to own the process.
static const char *const k_shim_sonames[] = {
    "libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1",
    "ld-linux-aarch64.so.1", "libresolv.so.2", "libutil.so.1", "libanl.so.1",
    // The Vulkan loader: MoltenVK is the ICD, reached through kl_vulkan's vk*
    // tier in kl_shim_lookup exactly as for an Android guest.
    "libvulkan.so.1",
};

// Never loaded, by policy: the Steam Frame's own GPU driver (freedreno, which
// the game ships) and the window-system libraries only it and the desktop
// paths need. Their imports stay in the gap, under their own heading.
static const char *const k_skip_prefixes[] = {
    "libvulkan_freedreno", "libX11", "libxcb", "libXext", "libXrandr",
    "libXi", "libXcursor", "libXfixes", "libXrender", "libXss", "libxkbcommon",
    "libwayland-", "libdrm", "libxshmfence", "libdisplay-info", "libSPIRV-Tools",
    "libGL.so", "libGLX", "libEGL.so", "libgbm",
};

static int skipped_by_policy(const char *name) {
    for (unsigned i = 0; i < sizeof k_skip_prefixes / sizeof *k_skip_prefixes; i++)
        if (!strncmp(name, k_skip_prefixes[i], strlen(k_skip_prefixes[i]))) return 1;
    const char *extra = getenv("KL_GLIBC_SKIP");
    if (extra && *extra) {
        char buf[1024];
        snprintf(buf, sizeof buf, "%s", extra);
        for (char *save = NULL, *t = strtok_r(buf, ",", &save); t;
             t = strtok_r(NULL, ",", &save))
            if (*t && !strncmp(name, t, strlen(t))) return 1;
    }
    return 0;
}

static int glibc_resolve(const char *name, char *path, size_t cap) {
    const char *b = strrchr(name, '/');
    b = b ? b + 1 : name;
    for (unsigned i = 0; i < sizeof k_shim_sonames / sizeof *k_shim_sonames; i++)
        if (!strcmp(b, k_shim_sonames[i])) return KL_NEED_SHIM;
    if (skipped_by_policy(b)) return KL_NEED_SKIP;
    // Directory by directory, so the game tree shadows the sysroot: within one
    // directory a file of exactly that name (the sysroot's soname symlinks are
    // this case), then any library whose DT_SONAME is that name. Name-first
    // across ALL directories loaded two SDL2s — the game's libSDL2.so (soname
    // libSDL2-2.0.so.0) and SteamRT4's libSDL2-2.0.so.0 — and the sysroot's
    // libmpg123 instead of the game's libmpg123_hlvr.
    for (int d = 0; d < g_ndirs; d++) {
        for (unsigned i = 0; i < g_nidx; i++)
            if (g_idx[i].dir == d && !strcmp(g_idx[i].file, b)) {
                snprintf(path, cap, "%s", g_idx[i].path);
                return KL_NEED_LOAD;
            }
        for (unsigned i = 0; i < g_nidx; i++)
            if (g_idx[i].dir == d && !strcmp(g_idx[i].soname, b)) {
                snprintf(path, cap, "%s", g_idx[i].path);
                return KL_NEED_LOAD;
            }
    }
    return KL_NEED_MISSING;
}

// ---- configure / load --------------------------------------------------------

int kl_glibc_configure(const char *target, const char *libdir, const char *entry,
                       FILE *out) {
    if (!libdir || !*libdir) return fail("no library directory%s", NULL);
    if (!entry || !*entry)   return fail("no entry executable%s", NULL);
    snprintf(g_target, sizeof g_target, "%s", target ? target : "glibc");
    g_ndirs = g_nmoddirs = 0;
    g_nidx = 0;

    // Defaults are per TARGET because they are where that game's files sit, not
    // a property of glibc. Alyx: <depot>/game/bin/linuxsteamrtarm64 (the
    // libdir), <depot>/game/hlvr/bin/linuxsteamrtarm64, and the SteamRT4 arm64
    // sysroot downloaded beside the depot.
    char def_mod[2400], def_sys[1200];
    snprintf(def_mod, sizeof def_mod, "%s", libdir);
    def_sys[0] = 0;
    if (!strcmp(g_target, "alyx")) {
        snprintf(def_mod, sizeof def_mod, "%s:%s/../../hlvr/bin/linuxsteamrtarm64",
                 libdir, libdir);
        snprintf(def_sys, sizeof def_sys,
                 "%s/../../../../steamrt4-arm64/files/lib/aarch64-linux-gnu", libdir);
    }
    add_dirs(kl_env_str("KL_GLIBC_MODDIRS", def_mod), &g_nmoddirs);
    add_dirs(kl_env_str("KL_GLIBC_SYSROOT", def_sys), NULL);

    // The entry is an executable, named without a ".so".
    snprintf(g_entry, sizeof g_entry, "%s/%s", libdir, entry);
    if (access(g_entry, R_OK) != 0)
        snprintf(g_entry, sizeof g_entry, "%s/%s.so", libdir, entry);
    if (access(g_entry, R_OK) != 0)
        return fail("entry executable not found: %s", g_entry);

    build_index();
    kl_dl_set_needed_resolver(glibc_resolve);
    // ~90 images, several with tens of thousands of exports: see kl_image.c.
    kl_sym_use_hash(1);
    if (out) {
        fprintf(out, "=== glibc door: %s ===\n", g_target);
        for (int d = 0; d < g_ndirs; d++)
            fprintf(out, "  %s %s\n", d < g_nmoddirs ? "modules:" : "sysroot:", g_dirs[d]);
        fprintf(out, "  %u ELF files indexed\n", g_nidx);
        fflush(out);
    }
    return 0;
}

static int has_so(const char *file) { return strstr(file, ".so") != NULL; }

int kl_glibc_load(FILE *out) {
    if (out) { fprintf(out, "=== %s (entry) ===\n", g_entry); fflush(out); }
    g_failed = 0;
    g_entry_img = kl_load_chain(g_entry, 0);
    if (!g_entry_img) {
        fprintf(stderr, "  [glibc] FAILED to map the entry %s: %s\n", g_entry, kl_error());
        return fail("entry did not map: %s", kl_error());
    }

    // Every library in the game tree is a root: the engine dlopens most of
    // them by name (engine2, the render system, client/server...), so the
    // executable's own closure (mimalloc + libstdc++) is a tiny fraction of
    // what will run. A versioned twin (libavcodec.so beside
    // libavcodec.so.58.91.100) is the same soname and is loaded once.
    if (strcmp(kl_env_str("KL_GLIBC_ROOTS", "all"), "entry") != 0) {
        for (unsigned i = 0; i < g_nidx; i++) {
            const ent *e = &g_idx[i];
            if (e->dir >= g_nmoddirs || !has_so(e->file)) continue;
            if (skipped_by_policy(e->file)) {
                if (out) fprintf(out, "  [glibc] %s: not loaded by policy\n", e->file);
                continue;
            }
            char path[1280];
            const char *key = e->soname[0] ? e->soname : e->file;
            if (glibc_resolve(key, path, sizeof path) != KL_NEED_LOAD)
                snprintf(path, sizeof path, "%s", e->path);
            if (!kl_load_chain(path, 0)) {
                g_failed++;
                fprintf(stderr, "  [glibc] FAILED to map %s: %s\n", path, kl_error());
                if (out) fprintf(out, "  [glibc] FAILED to map %s: %s\n", path, kl_error());
            }
        }
    }

    // Second chance for imports a DT_NEEDED cycle hid (kl_rebind_pending).
    unsigned rebound = 0;
    for (int i = 0; i < kl_dl_image_count(); i++)
        rebound += kl_rebind_pending(kl_dl_image_at(i, NULL));
    if (out && rebound)
        fprintf(out, "  [glibc] %u import slot(s) bound on the second pass "
                     "(exports of a library still being loaded at the time)\n", rebound);

    if (out) {
        int n = kl_dl_image_count();
        double mb = 0;
        unsigned tlsdesc = 0, tprel = 0;
        fprintf(out, "\n=== chain: %d images ===\n", n);
        fprintf(out, "  %-34s %-14s %8s %6s %5s %5s %7s\n", "image", "base", "MiB",
                "unres", "tlsd", "tprel", "x18");
        for (int i = 0; i < n; i++) {
            const char *nm = NULL;
            kl_image *img = kl_dl_image_at(i, &nm);
            const kl_stats *st = kl_get_stats(img);
            unsigned miss = 0;
            kl_missing_imports(img, &miss);
            mb += kl_span(img) / 1048576.0;
            tlsdesc += st->tlsdesc;
            tprel += st->tls_tprel;
            fprintf(out, "  %-34s @%-13p %8.2f %6u %5u %5u %7u\n", nm, kl_base(img),
                    kl_span(img) / 1048576.0, miss, st->tlsdesc, st->tls_tprel,
                    st->x18_patched);
        }
        fprintf(out, "  chain mapped: %d images, %.1f MiB, %u TLSDESC + %u TLS_TPREL64 "
                     "relocations, %u root(s) failed\n", n, mb, tlsdesc, tprel, g_failed);
        kl_tls_report(out);
        fflush(out);
    }
    return g_failed ? fail("%s", "some libraries did not map/relocate (see FAILED above)") : 0;
}

// ---- the gap -------------------------------------------------------------------

typedef struct { const char *name; const char *img; } pair;
static int pair_cmp(const void *a, const void *b) {
    int c = strcmp(((const pair *)a)->name, ((const pair *)b)->name);
    return c ? c : strcmp(((const pair *)a)->img, ((const pair *)b)->img);
}

static int in_list(const char *s, const char *const *list) {
    for (; *list; list++) if (!strcmp(s, *list)) return 1;
    return 0;
}
static int has_prefix(const char *s, const char *const *list) {
    for (; *list; list++) if (!strncmp(s, *list, strlen(*list))) return 1;
    return 0;
}
static int ends_with(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && !strcmp(s + a - b, suf);
}

// What kind of work an unresolved name is. A heuristic on the NAME, ordered
// most-specific first; its job is to make a 900-line list readable as a dozen
// work items, not to be a taxonomy.
enum { C_GLIBC, C_CHK, C_LFS, C_PTHREAD, C_CXX, C_DL, C_LOCALE, C_STDIO, C_MATH,
       C_MEM, C_FS, C_TIME, C_NET, C_PROC, C_SDL, C_WSYS, C_VK, C_OTHER, C_N };
static const char *const k_cat_names[C_N] = {
    "glibc internals (__libc_*, _dl_*, __errno_location, ...)",
    "fortify (__*_chk)",
    "glibc ABI aliases (LFS *64, __isoc99_*, __xstat, ...)",
    "pthread / sched / semaphores",
    "C++ runtime / unwinder (_Z*, __cxa_*, _Unwind_*)",
    "dynamic loading (dl*)",
    "locale / ctype / wchar / iconv",
    "stdio",
    "math (libm)",
    "string / memory / stdlib",
    "files / fds / mmap / poll",
    "time / clocks / sleep",
    "sockets / resolver / net",
    "process / signals / system info",
    "SDL2",
    "window system (X11 / xcb / wayland / drm)",
    "Vulkan",
    "other",
};

static int classify(const char *n) {
    static const char *const glibc_p[] = { "__libc_", "_dl_", "__gmon", "_IO_",
        "__res_", "__progname", "program_invocation", "gnu_get_libc", "__nss_",
        "__tls_get_addr", "__register_atfork", "__sched_cpu", "__getdelim", NULL };
    static const char *const glibc_x[] = { "__errno_location", "__h_errno_location",
        "__cxa_thread_atexit_impl", "__assert_fail", "__stack_chk_fail",
        "__stack_chk_guard", "__environ", "environ", "_r_debug", "__sysconf",
        "__fpending", "__overflow", "__uflow", "__libc_single_threaded",
        "__dso_handle", "__xpg_basename", "__xpg_strerror_r", "__sysv_signal",
        "secure_getenv", "__secure_getenv", "getauxval", "__getauxval",
        "__memcpy_chk", "_ITM_registerTMCloneTable", "_ITM_deregisterTMCloneTable",
        "__gmon_start__", "__ctype_get_mb_cur_max", NULL };
    if (!strncmp(n, "__", 2) && ends_with(n, "_chk")) return C_CHK;
    if (has_prefix(n, glibc_p) || in_list(n, glibc_x)) return C_GLIBC;
    static const char *const lfs_p[] = { "__isoc99_", "__isoc23_", "__xstat",
        "__fxstat", "__lxstat", "__xmknod", "__strtol_internal", "__strtoul_internal",
        "__strtod_internal", NULL };
    if (has_prefix(n, lfs_p)) return C_LFS;
    if (strncmp(n, "_Z", 2) && ends_with(n, "64") &&
        strncmp(n, "htobe", 5) && strncmp(n, "be64", 4) && strncmp(n, "le64", 4) &&
        strncmp(n, "vk", 2) && strncmp(n, "SDL_", 4)) return C_LFS;
    static const char *const pt_p[] = { "pthread_", "__pthread", "sem_", "sched_",
        "thrd_", "mtx_", "cnd_", "tss_", NULL };
    if (has_prefix(n, pt_p)) return C_PTHREAD;
    static const char *const cxx_p[] = { "_Z", "__cxa_", "__gxx_", "_Unwind_",
        "__gcc_personality", "__dynamic_cast", "__aarch64_", NULL };
    if (has_prefix(n, cxx_p)) return C_CXX;
    static const char *const dl_x[] = { "dlopen", "dlsym", "dlclose", "dlerror",
        "dladdr", "dladdr1", "dlinfo", "dlvsym", "dlmopen", "dl_iterate_phdr", NULL };
    if (in_list(n, dl_x)) return C_DL;
    static const char *const loc_p[] = { "newlocale", "uselocale", "freelocale",
        "duplocale", "setlocale", "localeconv", "nl_langinfo", "__ctype_", "iconv",
        "wc", "mb", "btowc", "wctob", "wmem", "tow", "isw", "wprintf", "fwprintf",
        "swprintf", "vswprintf", "swscanf", "getwc", "putwc", "ungetwc", "fgetwc",
        "fputwc", "fputws", "fgetws", "catopen", "catgets", "catclose",
        "bindtextdomain", "textdomain", "gettext", "dgettext", "dcgettext",
        "ngettext", "__uselocale", "__newlocale", "__freelocale", "__duplocale",
        "__nl_langinfo", "__wcs", "__strcoll_l", "__strxfrm_l", "__wcscoll_l",
        "__wcsxfrm_l", "__towlower_l", "__towupper_l", "__wctype_l", "__iswctype_l",
        "__strftime_l", "__wcsftime_l", "__strtod_l", "__strtof_l", "__strtold_l",
        NULL };
    if (has_prefix(n, loc_p) || (ends_with(n, "_l") && strncmp(n, "_Z", 2)))
        return C_LOCALE;
    static const char *const stdio_x[] = { "printf", "fprintf", "sprintf", "snprintf",
        "vprintf", "vfprintf", "vsprintf", "vsnprintf", "asprintf", "vasprintf",
        "dprintf", "vdprintf", "scanf", "fscanf", "sscanf", "vscanf", "vfscanf",
        "vsscanf", "fopen", "fdopen", "freopen", "fclose", "fread", "fwrite",
        "fflush", "fputs", "fputc", "fgets", "fgetc", "getc", "putc", "getchar",
        "putchar", "puts", "ungetc", "fseek", "fseeko", "ftell", "ftello", "rewind",
        "fgetpos", "fsetpos", "feof", "ferror", "clearerr", "fileno", "setvbuf",
        "setbuf", "setlinebuf", "perror", "popen", "pclose", "tmpfile", "tmpnam",
        "open_memstream", "fmemopen", "getline", "getdelim", "flockfile",
        "funlockfile", "ftrylockfile", "getc_unlocked", "putc_unlocked",
        "fputs_unlocked", "fwrite_unlocked", "fread_unlocked", "fflush_unlocked",
        "fgets_unlocked", "putchar_unlocked", "getchar_unlocked", "stdin", "stdout",
        "stderr", "fopencookie", "__uflow", "remove", NULL };
    if (in_list(n, stdio_x)) return C_STDIO;
    static const char *const math_b[] = { "sin", "cos", "tan", "asin", "acos",
        "atan", "atan2", "sinh", "cosh", "tanh", "asinh", "acosh", "atanh", "exp",
        "exp2", "exp10", "expm1", "log", "log2", "log10", "log1p", "logb", "pow",
        "sqrt", "cbrt", "hypot", "floor", "ceil", "round", "lround", "llround",
        "trunc", "rint", "lrint", "llrint", "nearbyint", "fmod", "remainder",
        "remquo", "modf", "frexp", "ldexp", "scalbn", "scalbln", "ilogb", "fabs",
        "copysign", "nan", "fmin", "fmax", "fdim", "fma", "erf", "erfc", "tgamma",
        "lgamma", "lgamma_r", "sincos", "nextafter", "nexttoward", "significand",
        "finite", "isnan", "isinf", "j0", "j1", "jn", "y0", "y1", "yn", "gamma",
        "__finite", "__isnan", "__isinf", "__signbit", "__fpclassify", NULL };
    {
        char base[64];
        snprintf(base, sizeof base, "%s", n);
        if (in_list(base, math_b)) return C_MATH;
        size_t l = strlen(base);
        if (l > 1 && (base[l - 1] == 'f' || base[l - 1] == 'l')) {
            base[l - 1] = 0;
            if (in_list(base, math_b)) return C_MATH;
        }
        if (!strncmp(n, "__", 2) && ends_with(n, "_finite")) return C_MATH;
    }
    static const char *const sdl_p[] = { "SDL_", NULL };
    if (has_prefix(n, sdl_p)) return C_SDL;
    static const char *const ws_p[] = { "X", "xcb_", "wl_", "drm", "xkb_", "glX", NULL };
    if (has_prefix(n, ws_p) && n[1] && n[1] != '_') return C_WSYS;
    if (!strncmp(n, "vk", 2) && n[2] >= 'A' && n[2] <= 'Z') return C_VK;
    static const char *const mem_p[] = { "mem", "str", "malloc", "calloc", "realloc",
        "reallocarray", "free", "posix_memalign", "aligned_alloc", "memalign",
        "valloc", "pvalloc", "mallinfo", "mallopt", "malloc_", "qsort", "bsearch",
        "ato", "rand", "srand", "drand48", "erand48", "lrand48", "mrand48",
        "srand48", "random", "srandom", "getenv", "setenv", "unsetenv", "putenv",
        "clearenv", "abs", "labs", "llabs", "div", "ldiv", "lldiv", "bcmp", "bcopy",
        "bzero", "explicit_bzero", "index", "rindex", "ffs", "stp", "basename",
        "dirname", "lfind", "lsearch", "tsearch", "tfind", "tdelete", "twalk",
        "hsearch", "hcreate", "hdestroy", "getsubopt", "getopt", "wordexp",
        "fnmatch", "glob", "regcomp", "regexec", "regfree", "regerror", "strto",
        "a64l", "l64a", "ecvt", "fcvt", "gcvt", "qecvt", "mkstemp", "mkdtemp",
        "mkostemp", "mktemp", "arc4random", "getrandom", "getentropy", NULL };
    if (has_prefix(n, mem_p)) return C_MEM;
    static const char *const fs_p[] = { "open", "close", "read", "write", "lseek",
        "pread", "pwrite", "stat", "fstat", "lstat", "statx", "statfs", "fstatfs",
        "statvfs", "fstatvfs", "access", "faccessat", "mkdir", "rmdir", "unlink",
        "rename", "link", "symlink", "readlink", "realpath", "getcwd", "chdir",
        "fchdir", "chmod", "fchmod", "chown", "fchown", "lchown", "umask", "utime",
        "futimens", "utimensat", "truncate", "ftruncate", "fsync", "fdatasync",
        "sync", "mmap", "munmap", "mprotect", "madvise", "mremap", "msync", "mlock",
        "munlock", "ioctl", "fcntl", "dup", "pipe", "poll", "ppoll", "select",
        "pselect", "epoll_", "inotify_", "eventfd", "timerfd_", "signalfd",
        "opendir", "fdopendir", "readdir", "closedir", "rewinddir", "seekdir",
        "telldir", "dirfd", "scandir", "alphasort", "flock", "lockf", "posix_fadvise",
        "posix_fallocate", "fallocate", "sendfile", "splice", "creat", "mkfifo",
        "mknod", "memfd_create", "shm_open", "shm_unlink", "nftw", "ftw",
        "readv", "writev", "preadv", "pwritev", "copy_file_range", "getdents",
        "name_to_handle_at", "fts_", "isatty", "ttyname", "tcgetattr", "tcsetattr",
        "posix_openpt", "grantpt", "unlockpt", "ptsname", NULL };
    if (has_prefix(n, fs_p)) return C_FS;
    static const char *const time_p[] = { "clock", "time", "gettimeofday",
        "settimeofday", "localtime", "gmtime", "mktime", "timegm", "timelocal",
        "strftime", "strptime", "asctime", "ctime", "difftime", "tzset", "tzname",
        "timezone", "daylight", "nanosleep", "usleep", "sleep", "alarm", "setitimer",
        "getitimer", "timer_", "ftime", "times", "adjtime", NULL };
    if (has_prefix(n, time_p)) return C_TIME;
    static const char *const net_p[] = { "socket", "connect", "bind", "listen",
        "accept", "send", "recv", "shutdown", "getaddrinfo", "freeaddrinfo",
        "gai_strerror", "getnameinfo", "getsockopt", "setsockopt", "getsockname",
        "getpeername", "inet_", "htons", "htonl", "ntohs", "ntohl", "gethostby",
        "gethostname", "getifaddrs", "freeifaddrs", "if_", "getservby",
        "getprotoby", "res_", "herror", "hstrerror", "ether_", "sethostname", NULL };
    if (has_prefix(n, net_p)) return C_NET;
    static const char *const proc_p[] = { "fork", "vfork", "exec", "posix_spawn",
        "wait", "kill", "raise", "signal", "sigaction", "sig", "getpid", "getppid",
        "gettid", "getpgid", "setpgid", "setsid", "getsid", "syscall", "prctl",
        "getuid", "geteuid", "getgid", "getegid", "setuid", "setgid", "setreuid",
        "setregid", "getgroups", "getpw", "getgr", "getlogin", "getresuid",
        "getresgid", "sysconf", "get_nprocs", "get_phys_pages", "get_avphys_pages",
        "getrlimit", "setrlimit", "prlimit", "getrusage", "uname", "sysinfo",
        "getpriority", "setpriority", "nice", "abort", "exit", "_exit", "_Exit",
        "atexit", "at_quick_exit", "quick_exit", "on_exit", "backtrace",
        "system", "daemon", "getpagesize", "getdtablesize", "personality",
        "ptrace", "reboot", "capget", "capset", "setjmp", "longjmp", "_setjmp",
        "_longjmp", "__sigsetjmp", "siglongjmp", "__longjmp_chk", "getcontext",
        "setcontext", "makecontext", "swapcontext", "pause", "getopt_long",
        "openlog", "syslog", "closelog", "vsyslog", "err", "warn", "verr", "vwarn",
        "error", "strsignal", "psignal", "getloadavg", "getdomainname",
        "unshare", "setns", "clone", "mount", "umount", NULL };
    if (has_prefix(n, proc_p)) return C_PROC;
    return C_OTHER;
}

unsigned kl_glibc_gap(FILE *out) {
    int nimg = kl_dl_image_count();
    size_t np = 0, cap = 0, nw = 0, capw = 0;
    pair *pairs = NULL, *weak = NULL;
    for (int i = 0; i < nimg; i++) {
        const char *so = NULL;
        kl_image *img = kl_dl_image_at(i, &so);
        unsigned n = 0, w = 0;
        const char *const *m = kl_missing_imports(img, &n);
        const char *const *wk = kl_weak_imports(img, &w);
        for (unsigned k = 0; k < n; k++) {
            if (np == cap) { cap = cap ? cap * 2 : 1024; pairs = realloc(pairs, cap * sizeof *pairs); }
            pairs[np++] = (pair){ m[k], so };
        }
        for (unsigned k = 0; k < w; k++) {
            if (nw == capw) { capw = capw ? capw * 2 : 256; weak = realloc(weak, capw * sizeof *weak); }
            weak[nw++] = (pair){ wk[k], so };
        }
    }
    qsort(pairs, np, sizeof *pairs, pair_cmp);
    qsort(weak, nw, sizeof *weak, pair_cmp);

    // One row per unique name: how many images import it, and which.
    typedef struct { const char *name; unsigned count; size_t first; int cat; } row;
    row *rows = calloc(np ? np : 1, sizeof *rows);
    size_t nr = 0;
    unsigned per_cat[C_N] = {0};
    for (size_t i = 0; i < np; ) {
        size_t j = i;
        while (j < np && !strcmp(pairs[j].name, pairs[i].name)) j++;
        int c = classify(pairs[i].name);
        rows[nr++] = (row){ pairs[i].name, (unsigned)(j - i), i, c };
        per_cat[c]++;
        i = j;
    }
    unsigned nweak_unique = 0;
    for (size_t i = 0; i < nw; i++)
        if (!i || strcmp(weak[i].name, weak[i - 1].name)) nweak_unique++;

    char path[1024];
    snprintf(path, sizeof path, "%s",
             kl_env_str("KL_GAP_OUT", NULL) ? kl_env_str("KL_GAP_OUT", NULL) : "");
    if (!*path) snprintf(path, sizeof path, "build/%s_gap.txt", g_target);
    FILE *f = fopen(path, "w");

    if (f) {
        fprintf(f, "# %s: unresolved imports after mapping %d images "
                   "(name  x<importers>  importers...)\n", g_target, nimg);
        fprintf(f, "# %zu unique unresolved, %u weak left NULL\n", nr, nweak_unique);
    }
    if (out) fprintf(out, "\n=== shim gap (%s) — by category, most-needed first ===\n",
                     g_target);
    for (int c = 0; c < C_N; c++) {
        if (!per_cat[c]) continue;
        if (out) fprintf(out, "  [%u] %s\n", per_cat[c], k_cat_names[c]);
        if (f)   fprintf(f, "\n## %s (%u)\n", k_cat_names[c], per_cat[c]);
        // Within a category, the names the most images need first: the next
        // shim worth writing is the one that unblocks the most libraries.
        for (unsigned want = (unsigned)nimg; want >= 1; want--) {
            for (size_t r = 0; r < nr; r++) {
                if (rows[r].cat != c || rows[r].count != want) continue;
                if (f) {
                    fprintf(f, "  %-44s x%-3u", rows[r].name, rows[r].count);
                    for (unsigned k = 0; k < rows[r].count && k < 8; k++)
                        fprintf(f, " %s", pairs[rows[r].first + k].img);
                    if (rows[r].count > 8) fprintf(f, " ...");
                    fputc('\n', f);
                }
            }
        }
        if (out) {
            // The console gets every name too, packed, in the same order.
            int col = 0;
            for (unsigned want = (unsigned)nimg; want >= 1; want--)
                for (size_t r = 0; r < nr; r++) {
                    if (rows[r].cat != c || rows[r].count != want) continue;
                    char cell[160];
                    int len = rows[r].count > 1
                            ? snprintf(cell, sizeof cell, "%s(x%u)", rows[r].name, rows[r].count)
                            : snprintf(cell, sizeof cell, "%s", rows[r].name);
                    if (col && col + len + 1 > 110) { fputc('\n', out); col = 0; }
                    fprintf(out, "%s%s", col ? " " : "      ", cell);
                    col += len + (col ? 1 : 6);
                }
            if (col) fputc('\n', out);
        }
    }
    if (f && nw) {
        fprintf(f, "\n## weak undefined, left NULL (%u)\n", nweak_unique);
        for (size_t i = 0; i < nw; i++)
            if (!i || strcmp(weak[i].name, weak[i - 1].name))
                fprintf(f, "  %-44s %s\n", weak[i].name, weak[i].img);
    }
    if (out) {
        fprintf(out, "  %zu unique unresolved imports across %d images "
                     "(%u weak left NULL)%s%s\n",
                nr, nimg, nweak_unique, f ? " — full list: " : "", f ? path : "");
        fflush(out);
    }
    if (f) fclose(f);
    free(rows); free(pairs); free(weak);
    return (unsigned)nr;
}

int kl_glibc_begin(FILE *out) {
    (void)out;
    // Phase B: the init arrays (libstdc++ and tier0 first), then the entry —
    // hlvr's e_entry is _start, which hands main to __libc_start_main, so the
    // shim for that one function is the whole front door.
    return fail("%s", "the glibc door stops at the gap: running init arrays and "
                      "_start is not implemented yet (phase B)");
}

double kl_glibc_pump(double seconds, const volatile int *quit) {
    (void)seconds; (void)quit;
    return 0.0;
}

void kl_glibc_report(FILE *out) {
    if (!out) return;
    fprintf(out, "  entry %s %s\n", g_entry, g_entry_img ? "mapped" : "NOT mapped");
    kl_tls_report(out);
    fflush(out);
}
