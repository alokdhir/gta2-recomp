/* IAT patching, synthetic-VA dispatch and logging stubs for unimplemented imports. */
#include "win32/imports.h"

#include <stdlib.h>
#include <string.h>

#define MAX_THUNKS 1024u

typedef struct {
    const char *name;
    NativeImpl fn;
    int stack_bytes;
    unsigned calls;
} Thunk;

static Thunk g_thunks[MAX_THUNKS];
static unsigned g_nthunks;
static RECOMP_TLS unsigned g_current; /* thunk index being dispatched to a stub */

static const NativeExport *const g_modules[] = {
    g_kernel32_exports, g_kernel32_file_exports, g_module_exports, g_user32_exports,
    g_advapi32_exports, g_dinput_exports, g_misc_exports,
};

static NativeImpl find_native(const char *ident)
{
    for (size_t m = 0; m < sizeof g_modules / sizeof *g_modules; m++)
        for (const NativeExport *e = g_modules[m]; e->ident; e++)
            if (!strcmp(e->ident, ident)) return e->fn;
    return NULL;
}

/* Every stub is the same function; it learns which import it is from g_current. */
static void stub_entry(void)
{
    Thunk *t = &g_thunks[g_current];
    if (t->calls++ < 3) {
        rt_log("stub: %s (ret %08X) args %08X %08X %08X %08X", t->name, MEM32(g_esp),
               MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp + 12), MEM32(g_esp + 16));
        /* Print string arguments for the calls that take names or messages */
        for (int a = 0; a < 4; a++) {
            uint32_t v = MEM32(g_esp + 4 + 4 * a);
            if (v >= 0x00400000u && v < 0x00700000u && MEM8(v) >= 0x20 && MEM8(v) < 0x7F)
                rt_log("        arg%d = \"%.80s\"", a, (const char *)GPTR(v));
        }
    }
    if (getenv("GTA2_STRICT")) rt_fatal("unimplemented import %s", t->name);
    g_eax = 0;
    g_esp += 4u + (t->stack_bytes > 0 ? (uint32_t)t->stack_bytes : 0u);
}

uint32_t imports_register_native(const char *name, NativeImpl fn, int stack_bytes)
{
    if (g_nthunks >= MAX_THUNKS) rt_fatal("too many import thunks");
    g_thunks[g_nthunks] = (Thunk){ name, fn, stack_bytes, 0 };
    return IMPORT_THUNK_BASE + 4u * g_nthunks++;
}

void imports_bind(void)
{
    unsigned native = 0;
    for (unsigned i = 0; i < g_import_count; i++) {
        const ImportEntry *e = &g_import_table[i];
        NativeImpl fn = find_native(e->ident);
        native += fn != NULL;
        MEM32(e->iat_va) = imports_register_native(e->full, fn, e->stack_bytes);
    }
    rt_log("imports: %u bound, %u native, %u stubbed", g_import_count, native, g_import_count - native);
}

static void thunk_trampoline_dispatch(void);

recomp_func_t recomp_lookup_kernel(uint32_t va)
{
    if (va < IMPORT_THUNK_BASE || (va & 3u)) return NULL;
    uint32_t i = (va - IMPORT_THUNK_BASE) / 4u;
    if (i >= g_nthunks) return NULL;
    if (g_thunks[i].fn) return g_thunks[i].fn;
    g_current = i;
    return thunk_trampoline_dispatch;
}

static void thunk_trampoline_dispatch(void) { stub_entry(); }
