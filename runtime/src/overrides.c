/* Hand-written replacements for specific gta2.exe functions.
 *
 * Each `void sub_XXXXXXXX(void)` defined here replaces the translated function
 * at that VA: scripts/translate.sh passes this file to the recompiler with
 * --exclude-manual, which skips generating those bodies so these link instead
 * (for direct calls and the dispatch table alike). Calling convention as for a
 * native import (see win32/imports.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "recomp_types.h"
#include "video/video.h"
#include "win32/imports.h"
#include "debug/watch.h"

/* 0x004031C0  GetDXVersion(DWORD *version, DWORD *platform) -- the DirectX SDK
 * probe that loads DDRAW.DLL and queries COM interfaces. Our renderer is
 * native Metal, so report what the game requires: DirectX 8.1 on NT. */
void sub_004031C0(void)
{
    MEM32(ARG(0)) = 0x801;
    MEM32(ARG(1)) = 2; /* VER_PLATFORM_WIN32_NT */
    RETURN_STDCALL(0, 2);
}



/* 0x0041E7A0  camera: computes the "visible" rectangle that traffic spawning
 * and recycling, pedestrian spawning and the object draw all test against.
 * It scales the zoom by two static 16.14 Fixed constants, 0.5 at 0x5E3F54 and
 * 0.75 (= 4:3) at 0x5E3F34, so at a wider frame things appear and vanish in
 * plain sight at the sides. For the duration of this call only, widen x:
 * half-scale * w and aspect / w, where w = frame aspect / (4/3). Both
 * constants have other readers, so they are restored afterwards.
 * Technique from gebdag/gta2-rtx-remix (MIT), which patches the two PUSHes. */
extern void sub_0041E7A0_gen(void);
void sub_0041E7A0(void)
{
    uint32_t sv = video_state();
    uint32_t vw = sv ? MEM32(sv + 0x48) : 0, vh = sv ? MEM32(sv + 0x4C) : 0;
    if (!vw || !vh || vw * 3u == vh * 4u) {
        sub_0041E7A0_gen();
        return;
    }
    double w = ((double)vw / vh) / (4.0 / 3.0);
    uint32_t half = MEM32(0x5E3F54), aspect = MEM32(0x5E3F34);
    MEM32(0x5E3F54) = (uint32_t)lround((double)(int32_t)half * w);
    MEM32(0x5E3F34) = (uint32_t)lround((double)(int32_t)aspect / w);
    uint32_t cam = g_ecx;
    sub_0041E7A0_gen();
    MEM32(0x5E3F54) = half;
    MEM32(0x5E3F34) = aspect;
    static int logged;
    if (!logged++) { /* view rectangle (16.14 fixed): x at +0x78/+0x7C, y at +0x80/+0x84 */
        double vx = (int32_t)(MEM32(cam + 0x7C) - MEM32(cam + 0x78)) / 16384.0;
        double vy = (int32_t)(MEM32(cam + 0x84) - MEM32(cam + 0x80)) / 16384.0;
        rt_log("widescreen view: %ux%u, w=%.3f, visible %.2f x %.2f tiles (ratio %.3f)", vw, vh, w, vx, vy,
               vy > 0 ? vx / vy : 0);
    }
}
