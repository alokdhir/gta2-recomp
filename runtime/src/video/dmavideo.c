/* Native replacement for DMAVideo.dll, the game's video device driver.
 *
 * The game talks to it through stdcall exports and also reads its state
 * struct directly, so that struct, the device list and the display-mode
 * list live in guest memory with the original layout. The 2D surface the
 * game draws its menus into is a guest RGB565 buffer; Vid_FlipBuffers hands
 * it to the Metal presenter.
 *
 * State struct (0x4C4 bytes), fields the game uses:
 *   +0x04 flags            +0x1C mode count       +0x24 mode list head
 *   +0x2C device list      +0x34 active device    +0x3C mode iterator
 *   +0x40 full-screen      +0x48/+0x4C width/height of the active mode
 *   +0x50 surface pixels   +0x54 surface pitch
 *   +0x5C/+0x64/+0x6C red/green/blue bits, +0x60/+0x68/+0x70 their shifts
 *   +0x80 active mode id   +0x4C0 hwnd
 * Device (0x30): id, flags, name*, description*, next*, guid*, guid[16], vidmem
 * Mode (0x40):   id, device, width, height, pitch, bpp, ..., next* @0x38
 */
#include <string.h>

#include "guest_memory.h"
#include "video/presenter.h"
#include "video/video.h"
#include "win32/imports.h"
#include "win32/modules.h"
#include "win32/user32_api.h"

#define SV_SIZE 0x4C4u
#define SV_FLAGS 0x04u
#define SV_FOUND_W 0x08u
#define SV_FOUND_H 0x0Cu
#define SV_FOUND_BPP 0x10u
#define SV_NUM_DEVICES 0x18u
#define SV_NUM_MODES 0x1Cu
#define SV_MODE_HEAD 0x24u
#define SV_MODE_ARRAY 0x28u
#define SV_DEV_HEAD 0x2Cu
#define SV_DEV_LAST 0x30u
#define SV_ACTIVE_DEV 0x34u
#define SV_MODE_ITER 0x3Cu
#define SV_FULLSCREEN 0x40u
#define SV_WIDTH 0x48u
#define SV_HEIGHT 0x4Cu
#define SV_PIXELS 0x50u
#define SV_PITCH 0x54u
#define SV_RBITS 0x5Cu
#define SV_RSHIFT 0x60u
#define SV_GBITS 0x64u
#define SV_GSHIFT 0x68u
#define SV_BBITS 0x6Cu
#define SV_BSHIFT 0x70u
#define SV_ACTIVE_MODE 0x80u
#define SV_HWND 0x4C0u

#define MODE_SIZE 0x40u
#define DEV_SIZE 0x30u

/* Modes offered to the game: its classic 4:3 sizes plus widescreen. */
static const struct { int w, h; } g_mode_sizes[] = {
    { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1280, 960 },
    { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 },
};
#define NMODES (sizeof g_mode_sizes / sizeof *g_mode_sizes)

static uint32_t g_sv;        /* guest VA of the state struct */
static uint32_t g_version;   /* guest VA of the version record */
static int g_surface_dirty;  /* the game wrote to the surface since the last flip */

uint32_t video_state(void) { return g_sv; }

const void *video_surface(uint32_t *pitch)
{
    if (!g_sv || !MEM32(g_sv + SV_PIXELS)) return NULL;
    *pitch = MEM32(g_sv + SV_PITCH);
    return GPTR(MEM32(g_sv + SV_PIXELS));
}

static uint32_t mode_by_id(uint32_t id)
{
    for (uint32_t m = MEM32(g_sv + SV_MODE_HEAD); m; m = MEM32(m + 0x38))
        if (MEM32(m) == id) return m;
    return 0;
}

static void build_lists(void)
{
    uint32_t dev = guest_alloc(DEV_SIZE, 1);
    MEM32(dev + 0x00) = 1;
    MEM32(dev + 0x04) = 1;
    MEM32(dev + 0x08) = guest_strdup("metal");
    MEM32(dev + 0x0C) = guest_strdup("Apple Metal");
    MEM32(dev + 0x10) = 0;
    MEM32(dev + 0x14) = 0;
    MEM32(dev + 0x28) = 256u << 20;
    MEM32(g_sv + SV_DEV_HEAD) = dev;
    MEM32(g_sv + SV_DEV_LAST) = dev;
    MEM32(g_sv + SV_NUM_DEVICES) = 1;

    uint32_t arr = guest_alloc(MODE_SIZE * NMODES, 1), prev = 0;
    for (uint32_t i = 0; i < NMODES; i++) {
        uint32_t m = arr + MODE_SIZE * i;
        MEM32(m + 0x00) = i + 1;
        MEM32(m + 0x04) = 1;
        MEM32(m + 0x08) = (uint32_t)g_mode_sizes[i].w;
        MEM32(m + 0x0C) = (uint32_t)g_mode_sizes[i].h;
        MEM32(m + 0x10) = (uint32_t)g_mode_sizes[i].w * 2u;
        MEM32(m + 0x14) = 16;
        if (prev) MEM32(prev + 0x38) = m;
        else MEM32(g_sv + SV_MODE_HEAD) = m;
        prev = m;
    }
    MEM32(g_sv + SV_MODE_ARRAY) = arr;
    MEM32(g_sv + SV_NUM_MODES) = NMODES;
}

/* ---- exports (all stdcall) ------------------------------------------------- */

static void Vid_GetVersion(void)
{
    if (!g_version) {
        g_version = guest_alloc(4 + 255, 1);
        MEM32(g_version) = 1;
        strcpy((char *)GPTR(g_version + 4), "gta2-recomp Metal video");
    }
    RETURN_STDCALL(g_version, 0);
}

static void Vid_InitDLL(void) { RETURN_STDCALL(1, 2); }

/* Vid_Init_SYS(param, flags) -> state struct */
static void Vid_Init_SYS(void)
{
    if (!g_sv) {
        g_sv = guest_alloc(SV_SIZE, 1);
        MEM32(g_sv + SV_FLAGS) = ARG(1) & 0xFFFFu;
        MEM32(g_sv + SV_FOUND_BPP) = 16;
        MEM32(g_sv + SV_RBITS) = 5;
        MEM32(g_sv + SV_RSHIFT) = 11;
        MEM32(g_sv + SV_GBITS) = 6;
        MEM32(g_sv + SV_GSHIFT) = 5;
        MEM32(g_sv + SV_BBITS) = 5;
        MEM32(g_sv + SV_BSHIFT) = 0;
        MEM32(g_sv + SV_FULLSCREEN) = 0;
        build_lists();
        rt_log("Vid_Init_SYS -> %08X", g_sv);
    }
    RETURN_STDCALL(g_sv, 2);
}

static void Vid_ShutDown_SYS(void)
{
    presenter_shutdown();
    RETURN_STDCALL(0, 1);
}

/* Vid_CheckMode(sv, w, h, bpp) -> mode id or 0 */
static void Vid_CheckMode(void)
{
    uint32_t w = ARG(1), h = ARG(2), bpp = ARG(3);
    for (uint32_t m = MEM32(g_sv + SV_MODE_HEAD); m; m = MEM32(m + 0x38))
        if (MEM32(m + 8) == w && MEM32(m + 12) == h && (!bpp || MEM32(m + 0x14) == bpp))
            RETURN_STDCALL(MEM32(m), 4);
    RETURN_STDCALL(0, 4);
}

static void Vid_FindDevice(void)
{
    for (uint32_t d = MEM32(g_sv + SV_DEV_HEAD); d; d = MEM32(d + 0x10))
        if (MEM32(d) == ARG(1)) RETURN_STDCALL(d, 2);
    RETURN_STDCALL(0, 2);
}

static void Vid_FindMode(void) { RETURN_STDCALL(mode_by_id(ARG(1)), 2); }

/* Vid_FindFirstMode(sv, bpp) / Vid_FindNextMode(sv): iterate mode ids */
static uint32_t next_matching(uint32_t m, uint32_t bpp)
{
    for (; m; m = MEM32(m + 0x38))
        if (!bpp || MEM32(m + 0x14) == bpp) return m;
    return 0;
}

static uint32_t g_iter_bpp;

static void Vid_FindFirstMode(void)
{
    g_iter_bpp = ARG(1);
    uint32_t m = next_matching(MEM32(g_sv + SV_MODE_HEAD), g_iter_bpp);
    MEM32(g_sv + SV_MODE_ITER) = m;
    RETURN_STDCALL(m ? MEM32(m) : 0, 2);
}

static void Vid_FindNextMode(void)
{
    uint32_t cur = MEM32(g_sv + SV_MODE_ITER);
    uint32_t m = cur ? next_matching(MEM32(cur + 0x38), g_iter_bpp) : 0;
    MEM32(g_sv + SV_MODE_ITER) = m;
    RETURN_STDCALL(m ? MEM32(m) : 0, 1);
}

static void Vid_SetDevice(void)
{
    MEM32(g_sv + SV_ACTIVE_DEV) = ARG(1);
    RETURN_STDCALL(0, 2);
}

/* Vid_SetMode(sv, hwnd, mode id) -> 0 on success */
static void Vid_SetMode(void)
{
    uint32_t hwnd = ARG(1), m = mode_by_id(ARG(2));
    if (!m) {
        rt_log("Vid_SetMode: unknown mode %u", ARG(2));
        RETURN_STDCALL(1, 3);
    }
    uint32_t w = MEM32(m + 8), h = MEM32(m + 12), pitch = w * 2u;
    if (MEM32(g_sv + SV_PIXELS)) guest_free(MEM32(g_sv + SV_PIXELS));
    MEM32(g_sv + SV_PIXELS) = guest_alloc(pitch * h, 1);
    MEM32(g_sv + SV_PITCH) = pitch;
    MEM32(g_sv + SV_WIDTH) = w;
    MEM32(g_sv + SV_HEIGHT) = h;
    MEM32(g_sv + SV_FOUND_W) = w;
    MEM32(g_sv + SV_FOUND_H) = h;
    MEM32(g_sv + SV_ACTIVE_MODE) = ARG(2);
    MEM32(g_sv + SV_HWND) = hwnd;
    user32_set_client_size(hwnd, (int)w, (int)h);
    presenter_init(user32_metal_layer(hwnd), (int)w, (int)h);
    rt_log("Vid_SetMode(%u) %ux%u", ARG(2), w, h);
    RETURN_STDCALL(0, 3);
}

static void Vid_CloseScreen(void) { RETURN_STDCALL(0, 1); }

/* The surface is always mapped; grabbing/locking just marks it written. */
static void Vid_GrabSurface(void) { g_surface_dirty = 1; RETURN_STDCALL(0, 1); }
static void Vid_ReleaseSurface(void) { RETURN_STDCALL(0, 1); }
static void Vid_GetSurface(void) { g_surface_dirty = 1; RETURN_STDCALL(0, 1); }
static void Vid_FreeSurface(void) { RETURN_STDCALL(0, 1); }
static void Vid_EnableWrites(void) { g_surface_dirty = 1; RETURN_STDCALL(0, 1); }
static void Vid_DisableWrites(void) { RETURN_STDCALL(0, 1); }

static void Vid_FlipBuffers(void)
{
    uint32_t pitch = 0;
    const void *fb = video_surface(&pitch);
    presenter_frame(g_surface_dirty ? fb : NULL, pitch, video_scene_fn(), video_scene_ctx());
    g_surface_dirty = 0;
    RETURN_STDCALL(0, 1);
}

/* Vid_ClearScreen(sv, r, g, b, left, top, right, bottom) */
static void Vid_ClearScreen(void)
{
    uint32_t pitch = 0;
    uint8_t *fb = (uint8_t *)video_surface(&pitch);
    if (fb) {
        uint16_t c = (uint16_t)((ARG(1) & 0xF8u) << 8 | (ARG(2) & 0xFCu) << 3 | (ARG(3) & 0xF8u) >> 3);
        int w = (int)MEM32(g_sv + SV_WIDTH), h = (int)MEM32(g_sv + SV_HEIGHT);
        int l = (int)ARG(4), t = (int)ARG(5), r = (int)ARG(6), b = (int)ARG(7);
        if (l < 0) l = 0;
        if (t < 0) t = 0;
        if (r <= l || r > w) r = w;
        if (b <= t || b > h) b = h;
        for (int y = t; y < b; y++) {
            uint16_t *row = (uint16_t *)(fb + (size_t)y * pitch);
            for (int x = l; x < r; x++) row[x] = c;
        }
        g_surface_dirty = 1;
    }
    RETURN_STDCALL(0, 8);
}

/* Vid_SetGamma(sv, r, g, b) with float arguments */
static void Vid_SetGamma(void)
{
    float r, g, b;
    uint32_t a = ARG(1), c = ARG(2), d = ARG(3);
    memcpy(&r, &a, 4);
    memcpy(&g, &c, 4);
    memcpy(&b, &d, 4);
    presenter_set_gamma(r, g, b);
    RETURN_STDCALL(0, 4);
}

/* Vid_WindowProc(sv, hwnd, msg, wparam, lparam): nothing to track natively */
static void Vid_WindowProc(void) { RETURN_STDCALL(0, 5); }

static const ModuleExport g_impl[] = {
    { "Vid_GetVersion", 0, Vid_GetVersion },
    { "Vid_InitDLL", 8, Vid_InitDLL },
    { "Vid_Init_SYS", 8, Vid_Init_SYS },
    { "Vid_ShutDown_SYS", 4, Vid_ShutDown_SYS },
    { "Vid_CheckMode", 16, Vid_CheckMode },
    { "Vid_FindDevice", 8, Vid_FindDevice },
    { "Vid_FindMode", 8, Vid_FindMode },
    { "Vid_FindFirstMode", 8, Vid_FindFirstMode },
    { "Vid_FindNextMode", 4, Vid_FindNextMode },
    { "Vid_SetDevice", 8, Vid_SetDevice },
    { "Vid_SetMode", 12, Vid_SetMode },
    { "Vid_CloseScreen", 4, Vid_CloseScreen },
    { "Vid_GrabSurface", 4, Vid_GrabSurface },
    { "Vid_ReleaseSurface", 4, Vid_ReleaseSurface },
    { "Vid_GetSurface", 4, Vid_GetSurface },
    { "Vid_FreeSurface", 4, Vid_FreeSurface },
    { "Vid_EnableWrites", 4, Vid_EnableWrites },
    { "Vid_DisableWrites", 4, Vid_DisableWrites },
    { "Vid_FlipBuffers", 4, Vid_FlipBuffers },
    { "Vid_ClearScreen", 32, Vid_ClearScreen },
    { "Vid_SetGamma", 16, Vid_SetGamma },
    { "Vid_WindowProc", 20, Vid_WindowProc },
    { NULL, 0, NULL },
};

void dmavideo_register(void) { modules_set_impl("dmavideo.dll", g_impl); }

static PresenterSceneFn g_scene_fn;
static void *g_scene_ctx;
void video_set_scene(PresenterSceneFn fn, void *ctx) { g_scene_fn = fn; g_scene_ctx = ctx; }
PresenterSceneFn video_scene_fn(void) { return g_scene_fn; }
void *video_scene_ctx(void) { return g_scene_ctx; }
