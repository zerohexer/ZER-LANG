/* ================================================================
 * zer_trace.c — full function call-graph tracer for `zerc-trace`.
 *
 * Compiled ONLY into the `zerc-trace` Makefile target, which builds every
 * translation unit with -finstrument-functions. GCC then calls
 * __cyg_profile_func_enter/exit around EVERY function; here we print an
 * indented call tree to stderr when `--trace-calls` set g_zer_trace_calls.
 *
 * Name resolution: dladdr only sees dynamic symbols, but almost every
 * function in the compiler is `static`, so we read the binary's own ELF
 * .symtab (/proc/self/exe) instead — it lists the static functions. The
 * binary is linked -no-pie, so a symbol's st_value == its runtime address;
 * no load-bias math is needed.
 *
 * Every function in THIS file is marked no_instrument_function, otherwise
 * the tracer would instrument itself and recurse forever.
 *
 * Normal `zerc` is built WITHOUT -finstrument-functions and never links
 * this file, so it pays zero cost.
 * ================================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <elf.h>

#include "ast.h"   /* g_zer_trace_calls */

#define NI __attribute__((no_instrument_function))

typedef struct { uint64_t addr; const char *name; } Sym;

static Sym  *g_syms       = NULL;
static int   g_sym_count  = 0;
static int   g_loaded     = 0;      /* 0 = not tried, 1 = ok, -1 = failed */
static char *g_strtab     = NULL;   /* owns the ELF string-table blob */

/* printed-ancestor tracking so indentation reflects the PRINTED call tree
 * (not the true depth through libc-ish helpers we may filter out). */
#define ZT_MAXDEPTH 4096
static unsigned char g_printed_stack[ZT_MAXDEPTH];
static int g_stack_top = 0;
static int g_printed_depth = 0;

static int NI sym_cmp(const void *a, const void *b) {
    uint64_t x = ((const Sym *)a)->addr, y = ((const Sym *)b)->addr;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

/* Read our own ELF; collect STT_FUNC symbols from .symtab into g_syms. */
static void NI load_symbols(void) {
    g_loaded = -1;   /* pessimistic; flip to +1 only on full success */
    FILE *f = fopen("/proc/self/exe", "rb");
    if (!f) return;

    Elf64_Ehdr eh;
    if (fread(&eh, sizeof(eh), 1, f) != 1 ||
        memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0) { fclose(f); return; }

    Elf64_Shdr *sh = (Elf64_Shdr *)malloc((size_t)eh.e_shnum * sizeof(Elf64_Shdr));
    if (!sh) { fclose(f); return; }
    if (fseek(f, (long)eh.e_shoff, SEEK_SET) != 0 ||
        fread(sh, sizeof(Elf64_Shdr), eh.e_shnum, f) != (size_t)eh.e_shnum) {
        free(sh); fclose(f); return;
    }

    int symtab_i = -1;
    for (int i = 0; i < eh.e_shnum; i++)
        if (sh[i].sh_type == SHT_SYMTAB) { symtab_i = i; break; }
    if (symtab_i < 0) { free(sh); fclose(f); return; }

    Elf64_Shdr *symh = &sh[symtab_i];
    Elf64_Shdr *strh = &sh[symh->sh_link];

    g_strtab = (char *)malloc(strh->sh_size);
    if (!g_strtab) { free(sh); fclose(f); return; }
    if (fseek(f, (long)strh->sh_offset, SEEK_SET) != 0 ||
        fread(g_strtab, 1, strh->sh_size, f) != strh->sh_size) {
        free(g_strtab); g_strtab = NULL; free(sh); fclose(f); return;
    }

    int nsym = (int)(symh->sh_size / sizeof(Elf64_Sym));
    Elf64_Sym *syms = (Elf64_Sym *)malloc(symh->sh_size);
    if (!syms) { free(sh); fclose(f); return; }
    if (fseek(f, (long)symh->sh_offset, SEEK_SET) != 0 ||
        fread(syms, sizeof(Elf64_Sym), nsym, f) != (size_t)nsym) {
        free(syms); free(sh); fclose(f); return;
    }

    g_syms = (Sym *)malloc((size_t)nsym * sizeof(Sym));
    if (!g_syms) { free(syms); free(sh); fclose(f); return; }
    for (int i = 0; i < nsym; i++) {
        if (ELF64_ST_TYPE(syms[i].st_info) != STT_FUNC) continue;
        if (syms[i].st_value == 0) continue;
        g_syms[g_sym_count].addr = syms[i].st_value;
        g_syms[g_sym_count].name = g_strtab + syms[i].st_name;
        g_sym_count++;
    }
    qsort(g_syms, (size_t)g_sym_count, sizeof(Sym), sym_cmp);

    free(syms);
    free(sh);
    fclose(f);
    g_loaded = 1;
}

/* largest symbol addr <= a  (the function containing address a) */
static const char * NI resolve(uint64_t a) {
    int lo = 0, hi = g_sym_count - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_syms[mid].addr <= a) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return (best >= 0) ? g_syms[best].name : NULL;
}

/* Bare `--trace-calls` shows a readable SKELETON of the meaningful compiler
 * functions (dispatchers + block/inst emitters), NOT the ~17k-line firehose of
 * every token helper. Override with env:
 *   ZER_TRACE_FILTER=all         → EVERYTHING (the full call graph)
 *   ZER_TRACE_FILTER=lower_,ir_   → only names starting with these prefixes
 *   ZER_TRACE_DEPTH=N             → cap printed nesting depth (0 = unlimited) */
static const char *ZT_DEFAULT_FILTER =
    "parse_declaration,parse_statement,parse_func_or_var,parse_struct,parse_block,"
    "lower_stmt,lower_orelse,lower_shortcircuit,ir_lower_func,ir_add_block,"
    "zercheck_ir,ir_check_inst,ir_merge_states,ir_check_ident_uaf,emit_func_decl";

static int         g_cfg_loaded    = 0;
static const char *g_filter        = NULL;  /* NULL => match everything ("all") */
static int         g_maxdepth      = 0;     /* 0 => unlimited */
static int         g_show_converge = 0;     /* ZER_TRACE_CONVERGE => also show the
                                             * fixpoint's throwaway convergence passes */

static void NI load_cfg(void) {
    const char *f = getenv("ZER_TRACE_FILTER");
    if (!f || !*f)                                        g_filter = ZT_DEFAULT_FILTER;
    else if (strcmp(f, "all") == 0 || strcmp(f, "*") == 0) g_filter = NULL;
    else                                                  g_filter = f;
    const char *d = getenv("ZER_TRACE_DEPTH");
    g_maxdepth = d ? atoi(d) : 0;
    g_show_converge = (getenv("ZER_TRACE_CONVERGE") != NULL) ? 1 : 0;
    g_cfg_loaded = 1;
}

static int NI name_passes(const char *name) {
    if (!g_filter) return 1;   /* "all" */
    if (!name) return 0;
    const char *p = g_filter;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (len > 0 && strncmp(name, p, len) == 0) return 1;
        if (!comma) break;
        p = comma + 1;
    }
    return 0;
}

void NI __cyg_profile_func_enter(void *this_fn, void *call_site) {
    (void)call_site;
    if (!g_zer_trace_calls) return;
    if (g_loaded == 0) load_symbols();
    if (!g_cfg_loaded) load_cfg();

    int printed = 0;
    if (g_stack_top < ZT_MAXDEPTH) {
        const char *name = (g_loaded == 1)
            ? resolve((uint64_t)(uintptr_t)this_fn) : NULL;
        if (name && name_passes(name) &&
            (g_maxdepth <= 0 || g_printed_depth < g_maxdepth) &&
            !(g_zer_in_converge && !g_show_converge)) {
            fprintf(stderr, "%*s%s()\n", g_printed_depth * 2, "", name);
            g_printed_depth++;
            printed = 1;
        }
        g_printed_stack[g_stack_top] = (unsigned char)printed;
    }
    g_stack_top++;
}

void NI __cyg_profile_func_exit(void *this_fn, void *call_site) {
    (void)this_fn; (void)call_site;
    if (!g_zer_trace_calls) return;
    if (g_stack_top > 0) {
        g_stack_top--;
        if (g_stack_top < ZT_MAXDEPTH && g_printed_stack[g_stack_top] &&
            g_printed_depth > 0)
            g_printed_depth--;
    }
}
