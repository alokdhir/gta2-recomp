/* Native replacements for the DLLs gta2.exe loads with LoadLibraryA. */
#pragma once
#include "winapi/imports.h"

typedef struct {
    const char *name;
    int stack_bytes; /* stdcall argument bytes */
    NativeImpl fn;
} ModuleExport;

/* Install the native implementation table for "dmavideo.dll" / "d3ddll.dll". */
void modules_set_impl(const char *dll, const ModuleExport *impl);
