/* Guest CPU state and the small runtime hooks the generated code calls. */
#include <mach/mach_time.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "recomp_types.h"
#include "winapi/imports.h"

RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
RECOMP_TLS uint32_t g_ebx, g_esi, g_edi, g_ebp;
RECOMP_TLS double g_fp_stack[8];
RECOMP_TLS int g_fp_top;
RECOMP_TLS uint16_t g_fp_control_word = 0x027F; /* x87 default: 53-bit precision, round-nearest, all masked */
RECOMP_TLS int g_fp_cmp;
RECOMP_TLS uint16_t g_fp_cc;
RECOMP_TLS int g_df;
RECOMP_TLS uint32_t g_fs_base;
RECOMP_TLS uint32_t g_seh_ebp;
RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;

volatile uint32_t g_icall_trace[ICALL_TRACE_SIZE];
volatile uint32_t g_icall_trace_idx;
volatile uint64_t g_icall_count;

static FILE *g_log;

void rt_log(const char *fmt, ...)
{
    if (!g_log) {
        const char *p = getenv("GTA2_LOG");
        g_log = p ? fopen(p, "w") : stderr;
        if (!g_log) g_log = stderr;
        setvbuf(g_log, NULL, _IOLBF, 0);
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
}

_Noreturn void rt_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("gta2: fatal: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    fprintf(stderr, "  recent indirect calls:");
    for (unsigned i = 0; i < ICALL_TRACE_SIZE; i++)
        fprintf(stderr, " %08X", g_icall_trace[(g_icall_trace_idx + i) & (ICALL_TRACE_SIZE - 1)]);
    fprintf(stderr, "\n  eax=%08X ecx=%08X edx=%08X ebx=%08X esi=%08X edi=%08X ebp=%08X esp=%08X\n",
            g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_ebp, g_esp);
    abort();
}

/* Indirect-call targets with no translated function. Saved on exit to
 * $GTA2_SEEDS (default ../generated/icall_seeds.json, relative to the game
 * dir) in the --seed-functions format, so the next translation finds them. */
#define MAX_MISSES 4096
static uint32_t g_misses[MAX_MISSES];
static unsigned g_nmisses;

static void save_misses(void)
{
    if (!g_nmisses) return;
    const char *path = getenv("GTA2_SEEDS");
    if (!path) path = "../generated/icall_seeds.json";
    /* merge with what is already there */
    FILE *f = fopen(path, "r");
    if (f) {
        char line[64];
        unsigned v;
        while (fgets(line, sizeof line, f))
            if (sscanf(line, " {\"start\": \"0x%x\"", &v) == 1 && g_nmisses < MAX_MISSES) {
                unsigned i;
                for (i = 0; i < g_nmisses && g_misses[i] != v; i++) {}
                if (i == g_nmisses) g_misses[g_nmisses++] = v;
            }
        fclose(f);
    }
    f = fopen(path, "w");
    if (!f) return;
    fputs("[\n", f);
    for (unsigned i = 0; i < g_nmisses; i++)
        fprintf(f, " {\"start\": \"0x%08X\", \"observed\": true}%s\n", g_misses[i],
                i + 1 < g_nmisses ? "," : "");
    fputs("]\n", f);
    fclose(f);
    rt_log("icall: %u missed targets saved to %s (re-run scripts/translate.sh)", g_nmisses, path);
}

void recomp_icall_fail_log(uint32_t va)
{
    for (unsigned i = 0; i < g_nmisses; i++)
        if (g_misses[i] == va) return;
    rt_log("icall: no function at %08X", va);
    if (g_nmisses < MAX_MISSES) {
        g_misses[g_nmisses++] = va;
        save_misses(); /* now: runs often end by signal, which skips atexit */
    }
}

void recomp_icall_not_code_log(uint32_t va)
{
    rt_log("icall: target %08X is not code", va);
}

void recomp_unimpl(const char *text, uint32_t va)
{
    rt_log("unimplemented instruction at %08X: %s", va, text);
}

recomp_func_t recomp_lookup_manual(uint32_t va)
{
    (void)va;
    return NULL;
}

uint64_t xbox_ReadTimeStampCounter(void)
{
    return mach_absolute_time();
}

/* A direct call reached a target the disassembler never translated. */
void recomp_unresolved_stub_hit(uint32_t va)
{
    static uint32_t seen[64];
    static unsigned n;
    for (unsigned i = 0; i < n; i++) if (seen[i] == va) return;
    if (n < 64) seen[n++] = va;
    rt_log("UNRESOLVED STUB hit: %08X (recorded as an observed seed; re-run scripts/translate.sh)", va);
    recomp_icall_fail_log(va); /* same seed file: execution really goes here */
}
