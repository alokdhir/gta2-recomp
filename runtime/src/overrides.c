/* Hand-written replacements for specific gta2.exe functions.
 *
 * Each `void sub_XXXXXXXX(void)` defined here replaces the translated function
 * at that VA: scripts/translate.sh passes this file to the recompiler with
 * --exclude-manual, which skips generating those bodies so these link instead
 * (for direct calls and the dispatch table alike). Calling convention as for a
 * native import (see win32/imports.h).
 */
#include "recomp_types.h"
#include "win32/imports.h"

/* 0x004031C0  GetDXVersion(DWORD *version, DWORD *platform) -- the DirectX SDK
 * probe that loads DDRAW.DLL and queries COM interfaces. Our renderer is
 * native Metal, so report what the game requires: DirectX 8.1 on NT. */
void sub_004031C0(void)
{
    MEM32(ARG(0)) = 0x801;
    MEM32(ARG(1)) = 2; /* VER_PLATFORM_WIN32_NT */
    RETURN_STDCALL(0, 2);
}
