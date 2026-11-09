/* LoadLibraryA / GetProcAddress / FreeLibrary over native module tables.
 *
 * gta2.exe loads its video device (registry "videoname", dmavideo.dll) and
 * renderer ("rendername", d3ddll.dll) at runtime and binds their exports by
 * name. Both are reimplemented natively; GetProcAddress hands the game a
 * synthetic thunk VA for each export. An export with no implementation yet
 * gets a logging stub that pops the right number of argument bytes.
 */
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "winapi/imports.h"
#include "winapi/modules.h"
#include "winapi/win32.h"

/* Exports of the original DLLs; all stdcall. stack_bytes from the `ret N`
 * of each original function (provisional where marked ?). */
#define STUB(name, bytes) { name, bytes, NULL }

static const ModuleExport g_dmavideo_stubs[] = {
    STUB("Vid_CheckMode", 0x10), STUB("Vid_ClearScreen", 0x20), STUB("Vid_CloseScreen", 4),
    STUB("Vid_DisableWrites", 4), STUB("Vid_EnableWrites", 4), STUB("Vid_FindDevice", 8),
    STUB("Vid_FindFirstMode", 8), STUB("Vid_FindMode", 8), STUB("Vid_FindNextMode", 4),
    STUB("Vid_FlipBuffers", 4), STUB("Vid_FreeSurface", 4), STUB("Vid_GetSurface", 4),
    STUB("Vid_GetVersion", 0), STUB("Vid_GrabSurface", 4), STUB("Vid_InitDLL", 8) /*?*/,
    STUB("Vid_Init_SYS", 8), STUB("Vid_ReleaseSurface", 4), STUB("Vid_SetDevice", 8),
    STUB("Vid_SetGamma", 0x10), STUB("Vid_SetMode", 0xC), STUB("Vid_ShutDown_SYS", 4),
    STUB("Vid_WindowProc", 0x10) /*?*/,
    { NULL, 0, NULL },
};

static const ModuleExport g_d3ddll_stubs[] = {
    STUB("ConvertColourBank", 4) /*?*/, STUB("DrawLine", 0x14), STUB("MakeScreenTable", 0xC),
    STUB("SetShadeTableA", 0x14) /*?*/, STUB("gbh_AddLight", 4), STUB("gbh_AssignPalette", 8),
    STUB("gbh_BeginLevel", 0) /*?*/, STUB("gbh_BeginScene", 0), STUB("gbh_BlitBuffer", 0x18) /*?*/,
    STUB("gbh_BlitImage", 0x1C), STUB("gbh_CloseDLL", 0), STUB("gbh_CloseScreen", 4),
    STUB("gbh_Convert16BitGraphic", 8), STUB("gbh_ConvertColour", 0xC), STUB("gbh_DrawFlatRect", 8),
    STUB("gbh_DrawQuad", 0x10), STUB("gbh_DrawQuadClipped", 0x14) /*?*/, STUB("gbh_DrawTile", 0x10),
    STUB("gbh_DrawTilePart", 0x10), STUB("gbh_DrawTriangle", 0x10), STUB("gbh_EndLevel", 0) /*?*/,
    STUB("gbh_EndScene", 0), STUB("gbh_FreeImageTable", 0), STUB("gbh_FreePalette", 4),
    STUB("gbh_FreeTexture", 4) /*?*/, STUB("gbh_GetGlobals", 0) /*?*/, STUB("gbh_GetUsedCache", 4),
    STUB("gbh_Init", 4) /*?*/, STUB("gbh_InitDLL", 4), STUB("gbh_InitImageTable", 4),
    STUB("gbh_LoadImage", 4), STUB("gbh_LockTexture", 4), STUB("gbh_Plot", 0x10) /*?*/,
    STUB("gbh_PrintBitmap", 4), STUB("gbh_RegisterPalette", 8), STUB("gbh_RegisterTexture", 0x14),
    STUB("gbh_ResetLights", 0) /*?*/, STUB("gbh_SetAmbient", 4), STUB("gbh_SetCamera", 0x10) /*?*/,
    STUB("gbh_SetColourDepth", 0), STUB("gbh_SetWindow", 0x10), STUB("gbh_UnlockTexture", 4) /*?*/,
    { NULL, 0, NULL },
};

typedef struct {
    const char *dll;
    const ModuleExport *impl;  /* native implementations (may be NULL) */
    const ModuleExport *stubs; /* full export list with stack sizes */
    uint32_t handle;
} Module;

static Module g_modules[] = {
    { "dmavideo.dll", NULL, g_dmavideo_stubs, 0 },
    { "d3ddll.dll", NULL, g_d3ddll_stubs, 0 },
};
#define NMODULES (sizeof g_modules / sizeof *g_modules)

void modules_set_impl(const char *dll, const ModuleExport *impl)
{
    for (unsigned i = 0; i < NMODULES; i++)
        if (!strcasecmp(g_modules[i].dll, dll)) g_modules[i].impl = impl;
}

static const char *basename_of(const char *p)
{
    const char *b = p;
    for (const char *s = p; *s; s++) if (*s == '\\' || *s == '/' || *s == ':') b = s + 1;
    return b;
}

static void LoadLibraryA(void)
{
    const char *name = GSTR(ARG(0));
    const char *base = basename_of(name);
    for (unsigned i = 0; i < NMODULES; i++) {
        if (!strcasecmp(g_modules[i].dll, base)) {
            if (!g_modules[i].handle) g_modules[i].handle = win32_new_handle(WIN32_H_MODULE, (intptr_t)i);
            rt_log("LoadLibraryA(%s) -> native module", name);
            RETURN_STDCALL(g_modules[i].handle, 1);
        }
    }
    rt_log("LoadLibraryA(%s) -> not available", name);
    win32_set_last_error(126); /* ERROR_MOD_NOT_FOUND */
    RETURN_STDCALL(0, 1);
}

static void FreeLibrary(void) { RETURN_STDCALL(1, 1); }

typedef struct {
    const char *name;
    uint32_t va;
} Bound;
static Bound g_bound[128];
static unsigned g_nbound;

static void GetProcAddress(void)
{
    Win32Handle *h = win32_get_handle(ARG(0), WIN32_H_MODULE);
    uint32_t namep = ARG(1);
    if (!h || namep < 0x10000u) RETURN_STDCALL(0, 2);
    const char *name = GSTR(namep);
    Module *m = &g_modules[h->data];
    for (unsigned i = 0; i < g_nbound; i++)
        if (!strcmp(g_bound[i].name, name)) RETURN_STDCALL(g_bound[i].va, 2);
    const ModuleExport *stub = NULL;
    for (const ModuleExport *e = m->stubs; e->name; e++) if (!strcmp(e->name, name)) stub = e;
    if (!stub) {
        rt_log("GetProcAddress(%s, %s) -> not exported", m->dll, name);
        RETURN_STDCALL(0, 2);
    }
    NativeImpl fn = NULL;
    int bytes = stub->stack_bytes;
    if (m->impl)
        for (const ModuleExport *e = m->impl; e->name; e++)
            if (!strcmp(e->name, name)) { fn = e->fn; bytes = e->stack_bytes; }
    char *full = malloc(strlen(m->dll) + strlen(name) + 2);
    sprintf(full, "%s!%s", m->dll, name);
    uint32_t va = imports_register_native(full, fn, bytes);
    if (g_nbound < 128) g_bound[g_nbound++] = (Bound){ stub->name, va };
    RETURN_STDCALL(va, 2);
}

const NativeExport g_module_exports[] = {
    { "LoadLibraryA", LoadLibraryA },
    { "FreeLibrary", FreeLibrary },
    { "GetProcAddress", GetProcAddress },
    { NULL, NULL },
};
