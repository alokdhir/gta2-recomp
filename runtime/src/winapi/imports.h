/* Win32 import binding for the recompiled gta2.exe.
 *
 * At load, every IAT slot is overwritten with a synthetic VA
 * (IMPORT_THUNK_BASE + 4*index). The generated code calls imports as
 * `call [slot]`, which reaches recomp_lookup_kernel() with that VA.
 *
 * Calling convention for a native import implementation (void fn(void)):
 *   on entry  MEM32(esp) = guest return address, args at ARG(0), ARG(1), ...
 *   on return eax = result; the implementation pops the return address plus
 *             its stdcall arguments (use RETURN_STDCALL / RETURN_CDECL).
 */
#pragma once
#include <stdint.h>
#include "recomp_types.h"

#define IMPORT_THUNK_BASE 0xFE000000u

typedef struct {
    uint32_t iat_va;      /* address of the IAT slot in gta2.exe */
    const char *full;     /* "KERNEL32.dll!CreateFileA" */
    const char *ident;    /* "CreateFileA" */
    int stack_bytes;      /* stdcall argument bytes, -1 = cdecl */
} ImportEntry;

extern const ImportEntry g_import_table[];
extern const unsigned g_import_count;

typedef void (*NativeImpl)(void);
typedef struct {
    const char *ident;
    NativeImpl fn;
} NativeExport;

/* Each win32 module provides a NULL-terminated table of implementations. */
extern const NativeExport g_kernel32_exports[];
extern const NativeExport g_kernel32_file_exports[];
extern const NativeExport g_user32_exports[];
extern const NativeExport g_advapi32_exports[];
extern const NativeExport g_module_exports[];
extern const NativeExport g_dinput_exports[];
extern const NativeExport g_miles_exports[];
extern const NativeExport g_misc_exports[];

void imports_bind(void);  /* patch IAT slots and resolve native implementations */

/* Guest synthetic VA for an arbitrary native callback (GetProcAddress results etc). */
uint32_t imports_register_native(const char *name, NativeImpl fn, int stack_bytes);

#define ARG(n) MEM32(g_esp + 4u + 4u * (uint32_t)(n))
#define RETURN_STDCALL(val, nargs) do { g_eax = (uint32_t)(val); g_esp += 4u + 4u * (nargs); return; } while (0)
#define RETURN_CDECL(val) do { g_eax = (uint32_t)(val); g_esp += 4u; return; } while (0)

/* Guest <-> host pointer helpers */
#define GPTR(va) ((void *)XBOX_PTR(va))
#define GSTR(va) ((const char *)XBOX_PTR(va))

void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
_Noreturn void rt_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
