/* zer_tmp.h — a PER-PROCESS scratch directory for the matrix grids.
 *
 * The grids used to write their probe program and read their diagnostics through
 * FIXED paths (`/tmp/_zer_hw.zer`, `/tmp/_zer_hw.err`, ...), so two runs at once —
 * two worktrees' `make check`, or a hand-run grid beside one — read each other's
 * files and reported phantom failures (tests/test_zer.sh had the same defect,
 * fixed with mktemp). Every path/command literal that names `/tmp/_zer_` is
 * wrapped in ZT(), which rewrites that prefix to "<this process's mkdtemp dir>/".
 * The literals keep their old spelling so a grep for a grid's file still finds it.
 */
#ifndef ZER_TMP_H
#define ZER_TMP_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* -std=c99 hides mkdtemp (POSIX.1-2008) and this header is included after the
 * system headers, so a feature macro here would be too late. The prototype is
 * the POSIX one. */
char *mkdtemp(char *tmpl);

static char zer_tmp_dir_buf[64];

static void zer_tmp_cleanup(void) {
    if (!zer_tmp_dir_buf[0]) return;
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", zer_tmp_dir_buf);
    if (system(cmd) != 0) { /* best effort */ }
}

static const char *zer_tmp_dir(void) {
    if (!zer_tmp_dir_buf[0]) {
        snprintf(zer_tmp_dir_buf, sizeof(zer_tmp_dir_buf), "/tmp/_zer_run_XXXXXX");
        if (!mkdtemp(zer_tmp_dir_buf)) {
            perror("zer_tmp: mkdtemp");
            exit(2);
        }
        atexit(zer_tmp_cleanup);
    }
    return zer_tmp_dir_buf;
}

/* Rewrite every "/tmp/_zer_" in `s` to "<dir>/". Eight rotating buffers, so a
 * call's result survives the next seven — each call site uses one at a time. */
static const char *ZT(const char *s) {
    static char bufs[8][4096];
    static int which;
    char *out = bufs[which++ & 7];
    const char *dir = zer_tmp_dir();
    const char *pat = "/tmp/_zer_";
    size_t plen = strlen(pat), dlen = strlen(dir), o = 0;
    while (*s && o + dlen + 2 < sizeof(bufs[0])) {
        if (strncmp(s, pat, plen) == 0) {
            memcpy(out + o, dir, dlen); o += dlen;
            out[o++] = '/';
            s += plen;
        } else {
            out[o++] = *s++;
        }
    }
    if (*s) { fprintf(stderr, "zer_tmp: ZT() input too long\n"); exit(2); }
    out[o] = '\0';
    return out;
}
#endif
