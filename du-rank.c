/*
 * du-rank - Tiny disk usage analyzer
 * Copyright (C) 2026 Traliran
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <limits.h>
#include <time.h>

#define PROG      "du-rank"
#define VERSION   "1.0.0"
#define MAX_DEPTH 512
#define BAR_WIDTH 16

#define ANSI_RESET   "\x1b[0m"
#define ANSI_BOLD    "\x1b[1m"
#define ANSI_DIM     "\x1b[2m"
#define ANSI_GREEN   "\x1b[32m"
#define ANSI_YELLOW  "\x1b[33m"
#define ANSI_CYAN    "\x1b[36m"

#define CE(x) (g_color ? (x) : "")

typedef struct {
    char *path;
    long long size;
    int is_dir;
} Entry;

/* a single file recorded for the duplicate scan */
typedef struct {
    char *path;
    char *name;      /* basename */
    long long size;  /* apparent size (st_size), matching key */
    long long bsize; /* allocated bytes (st_blocks*512), for reclaimable */
    long long mtime; /* modification time, seconds */
} DublFile;

/* min-heap that keeps only the N largest entries (constant memory) */
typedef struct {
    Entry *a;
    int n, cap;
} MinHeap;

static int g_color = 1;
static int g_skip_sys = 0;
static int g_top_n = 10;
static int g_dubl_scan = 0;
static const char *g_home = NULL;
static long long g_nfiles = 0, g_ndirs = 0;

static MinHeap g_dirs, g_files;

/* duplicate scan list (all regular files under the scan root) */
static DublFile *g_dubl = NULL;
static long long g_dubl_n = 0, g_dubl_cap = 0;

/* top-level (direct children of the scan root) list */
static Entry *g_tl = NULL;
static int g_tln = 0, g_tlc = 0;

static void die_oom(void) {
    fprintf(stderr, "%s: out of memory\n", PROG);
    exit(1);
}

static void tl_add(const char *path, long long size, int is_dir) {
    if (g_tln >= g_tlc) {
        g_tlc = g_tlc ? g_tlc * 2 : 32;
        g_tl = realloc(g_tl, (size_t)g_tlc * sizeof *g_tl);
        if (!g_tl) die_oom();
    }
    g_tl[g_tln].path = strdup(path);
    if (!g_tl[g_tln].path) die_oom();
    g_tl[g_tln].size = size;
    g_tl[g_tln].is_dir = is_dir;
    g_tln++;
}

static void dubl_add(const char *path, const struct stat *st) {
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    if (g_dubl_n >= g_dubl_cap) {
        g_dubl_cap = g_dubl_cap ? g_dubl_cap * 2 : 1024;
        g_dubl = realloc(g_dubl, (size_t)g_dubl_cap * sizeof *g_dubl);
        if (!g_dubl) die_oom();
    }
    g_dubl[g_dubl_n].path = strdup(path);
    g_dubl[g_dubl_n].name = strdup(name);
    if (!g_dubl[g_dubl_n].path || !g_dubl[g_dubl_n].name) die_oom();
    g_dubl[g_dubl_n].size = st->st_size;
    g_dubl[g_dubl_n].bsize = st->st_blocks > 0 ? (long long)st->st_blocks * 512 : 0;
    g_dubl[g_dubl_n].mtime = (long long)st->st_mtime;
    g_dubl_n++;
}

/* ---------------- min-heap of largest N ---------------- */

static void heap_init(MinHeap *h, int cap) {
    h->a = malloc((size_t)cap * sizeof *h->a);
    if (!h->a) die_oom();
    h->n = 0;
    h->cap = cap;
}

static void heap_free(MinHeap *h) {
    for (int i = 0; i < h->n; i++) free(h->a[i].path);
    free(h->a);
    h->a = NULL;
    h->n = h->cap = 0;
}

static void heap_sift_up(MinHeap *h, int i) {
    while (i > 0) {
        int p = (i - 1) / 2;
        if (h->a[i].size >= h->a[p].size) break;
        Entry t = h->a[i];
        h->a[i] = h->a[p];
        h->a[p] = t;
        i = p;
    }
}

static void heap_sift_down(MinHeap *h, int i) {
    for (;;) {
        int l = 2 * i + 1, r = 2 * i + 2, s = i;
        if (l < h->n && h->a[l].size < h->a[s].size) s = l;
        if (r < h->n && h->a[r].size < h->a[s].size) s = r;
        if (s == i) break;
        Entry t = h->a[i];
        h->a[i] = h->a[s];
        h->a[s] = t;
        i = s;
    }
}

static void heap_push(MinHeap *h, const char *path, long long size, int is_dir) {
    if (size <= 0) return;
    if (h->n < h->cap) {
        int i = h->n++;
        h->a[i].path = strdup(path);
        if (!h->a[i].path) die_oom();
        h->a[i].size = size;
        h->a[i].is_dir = is_dir;
        heap_sift_up(h, i);
    } else if (size > h->a[0].size) {
        free(h->a[0].path);
        h->a[0].path = strdup(path);
        if (!h->a[0].path) die_oom();
        h->a[0].size = size;
        h->a[0].is_dir = is_dir;
        heap_sift_down(h, 0);
    }
}

/* ---------------- filesystem walk ---------------- */

/* virtual / pseudo filesystems that make no sense to scan */
static int is_skipped_sys(const char *name) {
    static const char *skip[] = { "dev", "proc", "run", "sys" };
    for (size_t i = 0; i < sizeof skip / sizeof skip[0]; i++)
        if (strcmp(name, skip[i]) == 0) return 1;
    return 0;
}

/* returns the total size (in bytes) of everything under `path` */
static long long walk(const char *path, int depth) {
    DIR *d = opendir(path);
    if (!d) return 0;
    long long sum = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *name = de->d_name;
        if (name[0] == '.') {
            if (name[1] == '\0') continue;
            if (name[1] == '.' && name[2] == '\0') continue;
        }
        char full[PATH_MAX];
        int n;
        if (path[0] == '/' && path[1] == '\0')
            n = snprintf(full, sizeof full, "/%s", name);
        else
            n = snprintf(full, sizeof full, "%s/%s", path, name);
        if (n < 0 || n >= (int)sizeof full) continue; /* path too long */

        struct stat st;
        if (lstat(full, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            if (depth == 0 && g_skip_sys && is_skipped_sys(name)) continue;
            g_ndirs++;
            long long subsz = 0;
            if (depth < MAX_DEPTH) subsz = walk(full, depth + 1);
            sum += subsz;
            if (!g_dubl_scan) {
                if (depth == 0) tl_add(full, subsz, 1);
                heap_push(&g_dirs, full, subsz, 1);
            }
        } else {
            /* use allocated blocks, not apparent size, to show real disk usage */
            long long sz = st.st_blocks > 0 ? (long long)st.st_blocks * 512 : 0;
            sum += sz;
            g_nfiles++;
            if (g_dubl_scan) {
                if (S_ISREG(st.st_mode)) dubl_add(full, &st);
            } else {
                if (depth == 0) tl_add(full, sz, 0);
                heap_push(&g_files, full, sz, 0);
            }
        }
    }
    closedir(d);

    struct stat s;
    if (stat(path, &s) == 0 && s.st_blocks > 0)
        sum += (long long)s.st_blocks * 512; /* the directory entry itself */

    return sum;
}

/* ---------------- sorting & formatting ---------------- */

static int cmp_desc(const void *x, const void *y) {
    const Entry *a = x, *b = y;
    if (a->size > b->size) return -1;
    if (a->size < b->size) return 1;
    return 0;
}

static void fmt_size(char *out, size_t n, long long bytes) {
    static const char *u[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" };
    double v = (double)bytes;
    int i = 0;
    while (v >= 1024.0 && i < 5) {
        v /= 1024.0;
        i++;
    }
    if (i == 0)
        snprintf(out, n, "%lld B", bytes);
    else
        snprintf(out, n, "%.1f %s", v, u[i]);
}

static void fmt_int(char *out, size_t n, long long v) {
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%lld", v);
    int len = (int)strlen(tmp);
    int o = 0;
    for (int i = 0; i < len; i++) {
        int rem = len - i;
        if (i > 0 && rem % 3 == 0) {
            if (o + 1 >= (int)n) break;
            out[o++] = ',';
        }
        if (o + 1 >= (int)n) break;
        out[o++] = tmp[i];
    }
    out[o] = '\0';
}

static void make_bar(char *out, size_t n, long long size, long long max) {
    int filled = 0;
    if (max > 0) {
        filled = (int)((double)size / (double)max * BAR_WIDTH);
        if (filled > BAR_WIDTH) filled = BAR_WIDTH;
    }
    if (g_color) {
        static const char block[] = "\xe2\x96\x88"; /* UTF-8 full block */
        size_t pos = 0;
        for (int i = 0; i < filled && pos + sizeof block - 1 < n; i++) {
            memcpy(out + pos, block, sizeof block - 1);
            pos += sizeof block - 1;
        }
        for (int i = filled; i < BAR_WIDTH && pos + 1 < n; i++)
            out[pos++] = '.';
        out[pos] = '\0';
    } else {
        for (int i = 0; i < BAR_WIDTH && i < (int)n - 1; i++)
            out[i] = i < filled ? '#' : '.';
        int end = BAR_WIDTH < (int)n ? BAR_WIDTH : (int)n - 1;
        out[end] = '\0';
    }
}

/* show paths under the home directory as ~/... */
static const char *disp(const char *p) {
    static char buf[PATH_MAX + 4];
    if (g_home) {
        size_t hl = strlen(g_home);
        if (strncmp(p, g_home, hl) == 0) {
            const char *r = p + hl;
            if (*r == '\0') return "~";
            if (*r == '/') {
                snprintf(buf, sizeof buf, "~%s", r);
                return buf;
            }
        }
    }
    return p;
}

/* ---------------- output ---------------- */

static void section(const char *title) {
    printf("\n%s%s%s\n", CE(ANSI_BOLD), title, CE(ANSI_RESET));
    printf("%s------------------------------------------%s\n",
           CE(ANSI_DIM), CE(ANSI_RESET));
}

static void print_row(int rank, const Entry *e, long long total, long long max) {
    char s[32], pct[16], bar[256];
    fmt_size(s, sizeof s, e->size);
    double pc = total > 0 ? (double)e->size / (double)total * 100.0 : 0.0;
    snprintf(pct, sizeof pct, "%.1f%%", pc);
    make_bar(bar, sizeof bar, e->size, max);
    printf("  %s%2d%s  %s%10s%s  %s%6s%s  %s%s%s  %s%s%s%s\n",
           CE(ANSI_DIM), rank, CE(ANSI_RESET),
           CE(ANSI_YELLOW), s, CE(ANSI_RESET),
           CE(ANSI_CYAN), pct, CE(ANSI_RESET),
           CE(ANSI_GREEN), bar, CE(ANSI_RESET),
           CE(ANSI_BOLD), disp(e->path), e->is_dir ? "/" : "", CE(ANSI_RESET));
}

static void print_header(const char *root, long long total) {
    char ts[32], fs[32], ds[32];
    fmt_size(ts, sizeof ts, total);
    fmt_int(fs, sizeof fs, g_nfiles);
    fmt_int(ds, sizeof ds, g_ndirs);

    printf("\n%s%s v%s%s\n", CE(ANSI_BOLD), PROG, VERSION, CE(ANSI_RESET));
    printf("%s============================%s\n", CE(ANSI_DIM), CE(ANSI_RESET));
    printf("%sScan path%s     : %s%s%s\n",
           CE(ANSI_BOLD), CE(ANSI_RESET), CE(ANSI_CYAN), disp(root), CE(ANSI_RESET));
    printf("%sTotal size%s    : %s%s%s\n",
           CE(ANSI_BOLD), CE(ANSI_RESET), CE(ANSI_YELLOW), ts, CE(ANSI_RESET));
    printf("%sFiles%s         : %s\n", CE(ANSI_BOLD), CE(ANSI_RESET), fs);
    printf("%sDirectories%s   : %s\n", CE(ANSI_BOLD), CE(ANSI_RESET), ds);
    if (g_dubl_scan)
        printf("%sDup scan%s      : by name + size + mtime\n",
               CE(ANSI_BOLD), CE(ANSI_RESET));
    else
        printf("%sTop N%s         : %d\n", CE(ANSI_BOLD), CE(ANSI_RESET), g_top_n);

    if (strcmp(root, "/") == 0 && geteuid() != 0)
        printf("%sNote%s: running as non-root, some directories may be unreadable.\n",
               CE(ANSI_BOLD), CE(ANSI_RESET));
}

/* ---------------- duplicate scan ---------------- */

static int dubl_cmp(const void *x, const void *y) {
    const DublFile *a = x, *b = y;
    int c = strcmp(a->name, b->name);
    if (c) return c;
    if (a->size != b->size) return a->size < b->size ? -1 : 1;
    if (a->mtime != b->mtime) return a->mtime < b->mtime ? -1 : 1;
    return strcmp(a->path, b->path);
}

static int dubl_same(const DublFile *a, const DublFile *b) {
    return a->size == b->size && a->mtime == b->mtime &&
           strcmp(a->name, b->name) == 0;
}

static void print_dubl(void) {
    qsort(g_dubl, (size_t)g_dubl_n, sizeof *g_dubl, dubl_cmp);

    /* count groups of size >= 2 */
    long long groups = 0, dup_copies = 0, wasted = 0, i = 0;
    while (i < g_dubl_n) {
        long long j = i + 1;
        while (j < g_dubl_n && dubl_same(&g_dubl[i], &g_dubl[j])) j++;
        if (j - i > 1) {
            groups++;
            dup_copies += j - i;
            wasted += (j - i - 1) * g_dubl[i].bsize;
        }
        i = j;
    }

    section("DUPLICATE FILES (same name + size + mtime)");
    if (groups == 0) {
        printf("  %s(no duplicates found)%s\n", CE(ANSI_DIM), CE(ANSI_RESET));
        return;
    }

    char ws[32];
    fmt_size(ws, sizeof ws, wasted);
    printf("  %s%lld%s group%s of duplicates, %s%lld%s duplicate files, "
           "%s%s%s reclaimable %s%s%s\n",
           CE(ANSI_BOLD), groups, CE(ANSI_RESET), groups == 1 ? "" : "s",
           CE(ANSI_BOLD), dup_copies, CE(ANSI_RESET),
           CE(ANSI_YELLOW), ws, CE(ANSI_RESET),
           CE(ANSI_DIM), "(keep one copy per group)", CE(ANSI_RESET));

    int idx = 0;
    i = 0;
    while (i < g_dubl_n) {
        long long j = i + 1;
        while (j < g_dubl_n && dubl_same(&g_dubl[i], &g_dubl[j])) j++;
        if (j - i > 1) {
            idx++;
            char sz[32];
            fmt_size(sz, sizeof sz, g_dubl[i].size);
            printf("\n  %s#%d%s  \"%s%s%s\"  %s%lld copies%s %s(%s each)%s\n",
                   CE(ANSI_DIM), idx, CE(ANSI_RESET),
                   CE(ANSI_BOLD), g_dubl[i].name, CE(ANSI_RESET),
                   CE(ANSI_BOLD), j - i, CE(ANSI_RESET),
                   CE(ANSI_DIM), sz, CE(ANSI_RESET));
            for (long long k = i; k < j; k++)
                printf("      %s%s%s\n", CE(ANSI_CYAN), disp(g_dubl[k].path),
                       CE(ANSI_RESET));
        }
        i = j;
    }
}

static void usage(FILE *out) {
    fprintf(out,
        "Usage: %s [OPTIONS] [PATH]\n\n"
        "A lightweight disk usage analyzer. Shows which directories and files\n"
        "consume the most space, including all nested content. Can also find\n"
        "duplicate files (same name and metadata).\n\n"
        "Modes:\n"
        "  --allsys          Analyze the whole filesystem (from /)\n"
        "  --usr             Analyze the current user's home directory (from ~)\n"
        "  --dubl-scan       Find duplicate files by name + size + mtime\n\n"
        "Options:\n"
        "  --top <N>         Show top N results (default: 10)\n"
        "  --no-color        Disable colored output\n"
        "  -h, --help        Show this help and exit\n\n"
        "If a PATH is given it overrides the mode. With no arguments, --usr is assumed.\n",
        PROG);
}

/* ---------------- main ---------------- */

int main(int argc, char **argv) {
    const char *root = NULL;
    int skip_sys = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--allsys") == 0) {
            root = "/";
            skip_sys = 1;
        } else if (strcmp(argv[i], "--usr") == 0) {
            const char *h = getenv("HOME");
            if (!h || !*h) {
                fprintf(stderr, "%s: $HOME is not set\n", PROG);
                return 1;
            }
            root = h;
        } else if (strcmp(argv[i], "--dubl-scan") == 0) {
            g_dubl_scan = 1;
        } else if (strcmp(argv[i], "--top") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: --top requires a number\n", PROG);
                return 2;
            }
            g_top_n = atoi(argv[++i]);
            if (g_top_n < 1) g_top_n = 1;
        } else if (strcmp(argv[i], "--no-color") == 0) {
            g_color = 0;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "%s: unknown option '%s'\n", PROG, argv[i]);
            usage(stderr);
            return 2;
        } else {
            root = argv[i]; /* positional path overrides the mode */
        }
    }

    if (!root) {
        const char *h = getenv("HOME");
        if (!h || !*h) {
            fprintf(stderr, "%s: no target and $HOME is not set\n", PROG);
            return 1;
        }
        root = h;
    }
    if (strcmp(root, "/") == 0) skip_sys = 1;
    if (!isatty(STDOUT_FILENO)) g_color = 0;

    g_skip_sys = skip_sys;
    g_home = strcmp(root, "/") == 0 ? NULL : getenv("HOME");

    if (!g_dubl_scan) {
        heap_init(&g_dirs, g_top_n);
        heap_init(&g_files, g_top_n);
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    long long total = walk(root, 0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

    print_header(root, total);

    if (g_dubl_scan) {
        print_dubl();
    } else {
        qsort(g_tl, (size_t)g_tln, sizeof *g_tl, cmp_desc);
        qsort(g_dirs.a, (size_t)g_dirs.n, sizeof *g_dirs.a, cmp_desc);
        qsort(g_files.a, (size_t)g_files.n, sizeof *g_files.a, cmp_desc);

        section("TOP-LEVEL ENTRIES (direct children, largest first)");
        int shown = g_tln < g_top_n ? g_tln : g_top_n;
        if (shown == 0) {
            printf("  (none)\n");
        } else {
            for (int i = 0; i < shown; i++)
                print_row(i + 1, &g_tl[i], total, g_tl[0].size);
        }

        section("LARGEST DIRECTORIES (all levels)");
        if (g_dirs.n == 0) {
            printf("  (none)\n");
        } else {
            for (int i = 0; i < g_dirs.n; i++)
                print_row(i + 1, &g_dirs.a[i], total, g_dirs.a[0].size);
        }

        section("LARGEST FILES");
        if (g_files.n == 0) {
            printf("  (none)\n");
        } else {
            for (int i = 0; i < g_files.n; i++)
                print_row(i + 1, &g_files.a[i], total, g_files.a[0].size);
        }
    }

    printf("\n%sDone in %.2f s.%s\n", CE(ANSI_DIM), sec, CE(ANSI_RESET));

    if (g_dubl_scan) {
        for (long long i = 0; i < g_dubl_n; i++) {
            free(g_dubl[i].path);
            free(g_dubl[i].name);
        }
        free(g_dubl);
    } else {
        heap_free(&g_dirs);
        heap_free(&g_files);
        for (int i = 0; i < g_tln; i++) free(g_tl[i].path);
        free(g_tl);
    }
    return 0;
}
