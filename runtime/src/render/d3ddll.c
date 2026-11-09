/* Native replacement for d3ddll.dll, GTA2's renderer (the gbh_* interface).
 *
 * The game hands the renderer pre-transformed screen-space primitives (8-float
 * vertices: x, y, z, rhw, ARGB diffuse, specular, u, v; u/v in texels), 8-bit
 * indexed textures with a 256-byte row stride, and palettes stored
 * interleaved (palette p, colour i at base + 4*p + 256*i, colour 0 = key).
 * Texture records live in guest memory with the original 0x20-byte layout
 * because the game reads sprite sizes back from them.
 *
 * Interface facts (signatures, layouts, flag bits) from gebdag/gta2-rtx-remix
 * (MIT) and the original DLL's code; the implementation is our own.
 */
#include <stdlib.h>
#include <string.h>

#include "guest_memory.h"
#include "render/render.h"
#include "win32/imports.h"
#include "win32/modules.h"

/* Texture record: +0x0E width, +0x10 height, +0x12 palette (low byte),
 * +0x13 flags, +0x14 pixels, +0x18 palette (full), +0x1A revision */
#define TEX_REC_SIZE 0x20u
#define TEX_W 0x0Eu
#define TEX_H 0x10u
#define TEX_PAL_LOW 0x12u
#define TEX_FLAGS 0x13u
#define TEX_PIXELS 0x14u
#define TEX_PAL 0x18u
#define TEX_REV 0x1Au

/* Draw flags */
#define F_VERTEX_COLOUR 0x2000u
#define F_ADDITIVE 0x200u
#define F_OPAQUE 0x300u
#define F_EXPAND 0x10000u
#define F_POINT 0x20000u

#define MAX_PALETTES 4096
static uint32_t g_palettes[MAX_PALETTES];     /* guest VA of colour 0 */
static uint32_t g_pal_hash[MAX_PALETTES];
static uint32_t g_pal_hash_frame[MAX_PALETTES];
static uint32_t g_frame = 1;

static float g_screen_w = 640, g_screen_h = 480;

/* ---- texture cache: guest record -> GPU texture --------------------------- */

typedef struct {
    uint32_t rec;
    int gpu;
    uint16_t pal, rev;
    uint32_t pal_hash, pixels;
} TexCache;

#define TEXCACHE 8192
static TexCache g_tex[TEXCACHE]; /* open addressing on record VA */

static TexCache *tex_slot(uint32_t rec, int create)
{
    uint32_t h = (rec >> 4) * 2654435761u;
    for (unsigned i = 0; i < TEXCACHE; i++) {
        TexCache *t = &g_tex[(h + i) % TEXCACHE];
        if (t->rec == rec) return t;
        if (!t->rec) {
            if (!create) return NULL;
            t->rec = rec;
            t->gpu = -1;
            return t;
        }
    }
    rt_fatal("renderer texture cache full");
}

static uint32_t palette_hash(unsigned p)
{
    if (p >= MAX_PALETTES || !g_palettes[p]) return 0;
    if (g_pal_hash_frame[p] == g_frame) return g_pal_hash[p];
    uint32_t h = 2166136261u, base = g_palettes[p];
    for (uint32_t i = 0; i < 256; i++) h = (h ^ MEM32(base + 256u * i)) * 16777619u;
    g_pal_hash[p] = h | 1u;
    g_pal_hash_frame[p] = g_frame;
    return g_pal_hash[p];
}

/* Make sure the GPU copy of a texture record is current; returns its id. */
static int texture_for(uint32_t rec)
{
    if (!rec) return -1;
    uint32_t w = MEM16(rec + TEX_W), h = MEM16(rec + TEX_H);
    uint16_t pal = MEM16(rec + TEX_PAL), rev = MEM16(rec + TEX_REV);
    uint32_t pixels = MEM32(rec + TEX_PIXELS);
    if (!w || !h || !pixels) return -1;
    TexCache *t = tex_slot(rec, 1);
    uint32_t ph = palette_hash(pal);
    if (t->gpu >= 0 && t->pal == pal && t->rev == rev && t->pal_hash == ph && t->pixels == pixels)
        return t->gpu;

    static uint32_t rgba[256 * 256];
    if (w > 256 || h > 256) return -1;
    uint32_t base = pal < MAX_PALETTES ? g_palettes[pal] : 0;
    const uint8_t *src = GPTR(pixels);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint8_t idx = src[y * 256u + x];
            uint32_t c = base ? MEM32(base + 256u * idx) : (idx * 0x010101u);
            /* palette colour is 0x00RRGGBB; index 0 is the transparency key */
            rgba[y * w + x] = (c & 0x00FFFFFFu) | (idx ? 0xFF000000u : 0u);
        }
    }
    if (t->gpu < 0) t->gpu = render_texture_create((int)w, (int)h);
    render_texture_upload(t->gpu, (int)w, (int)h, rgba);
    t->pal = pal;
    t->rev = rev;
    t->pal_hash = ph;
    t->pixels = pixels;
    return t->gpu;
}

/* ---- primitives ------------------------------------------------------------- */

static uint32_t shade_colour(uint8_t shade)
{
    return 0xFF000000u | (uint32_t)shade << 16 | (uint32_t)shade << 8 | shade;
}

static RBlend blend_for(uint32_t flags)
{
    if ((flags & F_OPAQUE) == F_OPAQUE) return RB_OPAQUE;
    if (flags & F_ADDITIVE) return RB_ADDITIVE;
    return RB_ALPHA;
}

/* Emit a fan of n game vertices (8 floats each) as triangles. */
static void emit(uint32_t flags, uint32_t rec, uint32_t verts, unsigned n, uint8_t shade)
{
    if (!verts) return;
    float v0z;
    uint32_t z = MEM32(verts + 8);
    memcpy(&v0z, &z, 4);
    if (v0z <= 0.0f) return; /* the original rejects primitives behind the eye */

    int tex = texture_for(rec);
    float tw = rec ? (float)MEM16(rec + TEX_W) : 1, th = rec ? (float)MEM16(rec + TEX_H) : 1;
    RVert c[4];
    if ((flags & F_EXPAND) && rec) {
        float x, y;
        uint32_t a = MEM32(verts), b = MEM32(verts + 4);
        memcpy(&x, &a, 4);
        memcpy(&y, &b, 4);
        uint32_t col = (flags & F_VERTEX_COLOUR) ? MEM32(verts + 16) : shade_colour(shade);
        const float uv[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
        for (int i = 0; i < 4; i++)
            c[i] = (RVert){ x + uv[i][0] * tw, y + uv[i][1] * th, uv[i][0], uv[i][1], col };
        n = 4;
    } else {
        for (unsigned i = 0; i < n; i++) {
            uint32_t p = verts + 32u * i;
            float f[8];
            memcpy(f, GPTR(p), 32);
            uint32_t col = (flags & F_VERTEX_COLOUR) ? MEM32(p + 16) : shade_colour(shade);
            c[i] = (RVert){ f[0], f[1], f[6] / tw, f[7] / th, col };
        }
    }
    RVert tri[6];
    int k = 0;
    for (unsigned i = 1; i + 1 < n; i++) {
        tri[k++] = c[0];
        tri[k++] = c[i];
        tri[k++] = c[i + 1];
    }
    render_triangles(tri, k, tex, blend_for(flags), (flags & F_POINT) != 0);
}

static void trace_quad(const char *what)
{
    static int logged;
    if (!getenv("GTA2_TRACE_GBH") || logged++ >= 8) return;
    uint32_t v = ARG(2), r = ARG(1);
    float f[32];
    memcpy(f, GPTR(v), sizeof f);
    rt_log("%s flags=%08X tex=%08X(%ux%u) shade=%u", what, ARG(0), r, r ? MEM16(r + TEX_W) : 0, r ? MEM16(r + TEX_H) : 0, ARG(3) & 0xFF);
    for (int i = 0; i < 4; i++)
        rt_log("   v%d x=%.1f y=%.1f z=%.3f rhw=%.3f col=%08X u=%.2f v=%.2f", i, f[8*i], f[8*i+1], f[8*i+2], f[8*i+3],
               ((uint32_t *)f)[8*i+4], f[8*i+6], f[8*i+7]);
}
static void gbh_DrawQuad(void) { trace_quad("DrawQuad");  emit(ARG(0), ARG(1), ARG(2), 4, (uint8_t)ARG(3)); RETURN_STDCALL(0, 4); }
static void gbh_DrawQuadClipped(void) { emit(ARG(0), ARG(1), ARG(2), 4, (uint8_t)ARG(3)); RETURN_STDCALL(0, 5); }
static void gbh_DrawTile(void) { emit(ARG(0), ARG(1), ARG(2), 4, (uint8_t)ARG(3)); RETURN_STDCALL(0, 4); }
static void gbh_DrawTilePart(void) { emit(ARG(0), ARG(1), ARG(2), 4, (uint8_t)ARG(3)); RETURN_STDCALL(0, 4); }
static void gbh_DrawTriangle(void) { emit(ARG(0), ARG(1), ARG(2), 3, (uint8_t)ARG(3)); RETURN_STDCALL(0, 4); }

static void solid_rect(float x0, float y0, float x1, float y1, uint32_t argb)
{
    RVert t[6] = {
        { x0, y0, 0, 0, argb }, { x1, y0, 0, 0, argb }, { x1, y1, 0, 0, argb },
        { x0, y0, 0, 0, argb }, { x1, y1, 0, 0, argb }, { x0, y1, 0, 0, argb },
    };
    render_triangles(t, 6, -1, (argb >> 24) == 0xFF ? RB_OPAQUE : RB_ALPHA, 1);
}

/* gbh_DrawFlatRect(verts[4], colour) */
static void gbh_DrawFlatRect(void)
{
    uint32_t v = ARG(0), colour = ARG(1);
    if (v) {
        uint32_t argb = (colour >> 24) ? colour : (colour | 0xFF000000u);
        RVert c[4];
        for (int i = 0; i < 4; i++) {
            float f[2];
            memcpy(f, GPTR(v + 32u * (uint32_t)i), 8);
            c[i] = (RVert){ f[0], f[1], 0, 0, argb };
        }
        RVert t[6] = { c[0], c[1], c[2], c[0], c[2], c[3] };
        render_triangles(t, 6, -1, (argb >> 24) == 0xFF ? RB_OPAQUE : RB_ALPHA, 1);
    }
    RETURN_STDCALL(0, 2);
}

static void gbh_Plot(void)
{
    float x = (float)(int32_t)ARG(0), y = (float)(int32_t)ARG(1);
    solid_rect(x, y, x + 1, y + 1, ARG(2) | 0xFF000000u);
    RETURN_STDCALL(0, 4);
}

static void DrawLine(void)
{
    float x0 = (float)(int32_t)ARG(0), y0 = (float)(int32_t)ARG(1);
    float x1 = (float)(int32_t)ARG(2), y1 = (float)(int32_t)ARG(3);
    if (x0 > x1) { float t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { float t = y0; y0 = y1; y1 = t; }
    solid_rect(x0, y0, x1 + 1, y1 + 1, ARG(4) | 0xFF000000u);
    RETURN_STDCALL(0, 5);
}

/* ---- textures & palettes ------------------------------------------------------ */

/* gbh_RegisterTexture(w, h, pixels, palette, flag) -> record */
static void gbh_RegisterTexture(void)
{
    uint32_t r = guest_alloc(TEX_REC_SIZE, 1);
    MEM16(r + TEX_W) = (uint16_t)ARG(0);
    MEM16(r + TEX_H) = (uint16_t)ARG(1);
    MEM32(r + TEX_PIXELS) = ARG(2);
    MEM8(r + TEX_PAL_LOW) = (uint8_t)ARG(3);
    MEM8(r + TEX_FLAGS) = (uint8_t)ARG(4);
    MEM16(r + TEX_PAL) = (uint16_t)ARG(3);
    RETURN_STDCALL(r, 5);
}

static void gbh_FreeTexture(void)
{
    uint32_t r = ARG(0);
    TexCache *t = r ? tex_slot(r, 0) : NULL;
    if (t) {
        if (t->gpu >= 0) render_texture_free(t->gpu);
        t->gpu = -1;
        t->pixels = 0; /* keep the slot: open addressing */
    }
    guest_free(r);
    RETURN_STDCALL(0, 1);
}

static void gbh_LockTexture(void) { RETURN_STDCALL(0, 1); }

static void gbh_UnlockTexture(void)
{
    uint32_t r = ARG(0);
    if (r) MEM16(r + TEX_REV) = (uint16_t)(MEM16(r + TEX_REV) + 1);
    RETURN_STDCALL(0, 1);
}

static void gbh_AssignPalette(void)
{
    uint32_t r = ARG(0);
    if (r) {
        MEM8(r + TEX_PAL_LOW) = (uint8_t)ARG(1);
        MEM16(r + TEX_PAL) = (uint16_t)ARG(1);
    }
    RETURN_STDCALL(0, 2);
}

static void gbh_RegisterPalette(void)
{
    uint32_t i = ARG(0);
    if (i < MAX_PALETTES) {
        g_palettes[i] = ARG(1);
        g_pal_hash_frame[i] = 0;
    }
    RETURN_STDCALL(i, 2);
}

static void gbh_FreePalette(void)
{
    if (ARG(0) < MAX_PALETTES) g_palettes[ARG(0)] = 0;
    RETURN_STDCALL(0, 1);
}

static void gbh_ConvertColour(void)
{
    RETURN_STDCALL(0xFF000000u | (ARG(0) & 0xFFu) << 16 | (ARG(1) & 0xFFu) << 8 | (ARG(2) & 0xFFu), 3);
}

/* ---- images (16-bit TGA, used by the front end) --------------------------------- */

#define MAX_IMAGES 256
static struct { int gpu, w, h; } g_images[MAX_IMAGES];
static int g_nimages;

static void gbh_InitImageTable(void)
{
    g_nimages = 0;
    RETURN_STDCALL(0, 1);
}

static void gbh_FreeImageTable(void)
{
    for (int i = 0; i < g_nimages; i++)
        if (g_images[i].gpu >= 0) render_texture_free(g_images[i].gpu);
    g_nimages = 0;
    RETURN_STDCALL(0, 0);
}

/* gbh_LoadImage(tga) -> image index, or -1 */
static void gbh_LoadImage(void)
{
    const uint8_t *t = GPTR(ARG(0));
    if (!ARG(0) || t[1] != 0 || t[2] != 2 || t[16] != 16 || g_nimages >= MAX_IMAGES) {
        rt_log("gbh_LoadImage: unsupported image");
        RETURN_STDCALL(0xFFFFFFFFu, 1);
    }
    int w = t[12] | t[13] << 8, h = t[14] | t[15] << 8;
    int top_down = (t[17] & 0x20) != 0;
    const uint8_t *px = t + 18 + t[0];
    uint32_t *rgba = malloc((size_t)w * (size_t)h * 4);
    for (int y = 0; y < h; y++) {
        const uint8_t *row = px + (size_t)(top_down ? y : h - 1 - y) * (size_t)w * 2;
        for (int x = 0; x < w; x++) {
            uint16_t c = (uint16_t)(row[2 * x] | row[2 * x + 1] << 8); /* X1R5G5B5 */
            uint32_t r = (c >> 10) & 31u, g = (c >> 5) & 31u, b = c & 31u;
            rgba[y * w + x] = 0xFF000000u | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
        }
    }
    int i = g_nimages++;
    g_images[i].gpu = render_texture_create(w, h);
    g_images[i].w = w;
    g_images[i].h = h;
    render_texture_upload(g_images[i].gpu, w, h, rgba);
    free(rgba);
    RETURN_STDCALL((uint32_t)i, 1);
}

/* gbh_BlitImage(image, srcX1, srcY1, srcX2, srcY2, dstX, dstY) */
static void gbh_BlitImage(void)
{
    static int logged;
    if (getenv("GTA2_TRACE_GBH") && logged++ < 6)
        rt_log("BlitImage(%d, %d,%d, %d,%d, %d,%d) images=%d", (int)ARG(0), (int)ARG(1), (int)ARG(2),
               (int)ARG(3), (int)ARG(4), (int)ARG(5), (int)ARG(6), g_nimages);
    int i = (int)ARG(0);
    if (i >= 0 && i < g_nimages) {
        float sx0 = (float)(int32_t)ARG(1), sy0 = (float)(int32_t)ARG(2);
        float sx1 = (float)(int32_t)ARG(3), sy1 = (float)(int32_t)ARG(4);
        float dx = (float)(int32_t)ARG(5), dy = (float)(int32_t)ARG(6);
        float w = (float)g_images[i].w, h = (float)g_images[i].h;
        float x1 = dx + (sx1 - sx0), y1 = dy + (sy1 - sy0);
        float u0 = sx0 / w, v0 = sy0 / h, u1 = sx1 / w, v1 = sy1 / h;
        uint32_t c = 0xFFFFFFFFu;
        RVert t[6] = {
            { dx, dy, u0, v0, c }, { x1, dy, u1, v0, c }, { x1, y1, u1, v1, c },
            { dx, dy, u0, v0, c }, { x1, y1, u1, v1, c }, { dx, y1, u0, v1, c },
        };
        render_triangles(t, 6, g_images[i].gpu, RB_OPAQUE, 1);
    }
    RETURN_STDCALL(0, 7);
}

/* ---- frame, camera, lights, setup ---------------------------------------------- */

static void gbh_BeginScene(void)
{
    g_frame++;
    render_begin_frame();
    RETURN_STDCALL(0, 0);
}

static void gbh_EndScene(void) { RETURN_STDCALL(0, 0); }

/* gbh_SetWindow(left, top, right, bottom) as floats */
static void gbh_SetWindow(void)
{
    float r, b;
    uint32_t a = ARG(2), c = ARG(3);
    memcpy(&r, &a, 4);
    memcpy(&b, &c, 4);
    g_screen_w = r + 1;
    g_screen_h = b + 1;
    render_set_screen(g_screen_w, g_screen_h);
    RETURN_STDCALL(0, 4);
}

/* MakeScreenTable(base, pitch, height): the original precomputes row pointers
 * into the surface at `base` for its software paths, internally. We draw
 * everything on the GPU, so there is nothing to build -- and `base` is the
 * surface itself, which must not be written. */
static void MakeScreenTable(void) { RETURN_STDCALL(0, 3); }

static uint32_t g_globals;

static void gbh_GetGlobals(void)
{
    if (!g_globals) g_globals = guest_alloc(0x400, 1);
    RETURN_STDCALL(g_globals, 0);
}

static void gbh_InitDLL(void) { RETURN_STDCALL(0, 1); }
static void gbh_Init(void) { render_install(); RETURN_STDCALL(0, 1); }
static void gbh_CloseScreen(void) { RETURN_STDCALL(0, 1); }
static void gbh_CloseDLL(void) { RETURN_STDCALL(0, 0); }
static void gbh_SetColourDepth(void) { RETURN_STDCALL(16, 0); }
static void gbh_GetUsedCache(void) { RETURN_STDCALL(0, 1); }
static void gbh_BeginLevel(void) { RETURN_STDCALL(0, 0); }
static void gbh_EndLevel(void) { RETURN_STDCALL(0, 0); }
static void gbh_SetCamera(void) { RETURN_STDCALL(0, 4); }
static void gbh_ResetLights(void) { RETURN_STDCALL(0, 0); }
static void gbh_AddLight(void) { RETURN_STDCALL(0, 1); }
static void gbh_SetAmbient(void) { RETURN_STDCALL(0, 1); }
static void ConvertColourBank(void) { RETURN_STDCALL(0, 1); }
static void SetShadeTableA(void) { RETURN_STDCALL(0, 5); }
static void gbh_PrintBitmap(void) { RETURN_STDCALL(0, 1); }
static void gbh_Convert16BitGraphic(void) { RETURN_STDCALL(0, 2); }

static void gbh_BlitBuffer(void)
{
    static int logged;
    if (!logged++) rt_log("gbh_BlitBuffer(%08X %08X %08X %08X %08X %08X) not implemented",
                          ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5));
    RETURN_STDCALL(0, 6);
}

#define E(name, bytes) { #name, bytes, name }
static const ModuleExport g_impl[] = {
    E(gbh_InitDLL, 4), E(gbh_Init, 4), E(gbh_CloseScreen, 4), E(gbh_CloseDLL, 0),
    E(gbh_DrawQuad, 16), E(gbh_DrawQuadClipped, 20), E(gbh_DrawTile, 16), E(gbh_DrawTilePart, 16),
    E(gbh_DrawTriangle, 16), E(gbh_DrawFlatRect, 8), E(gbh_Plot, 16), E(DrawLine, 20),
    E(gbh_RegisterTexture, 20), E(gbh_FreeTexture, 4), E(gbh_LockTexture, 4), E(gbh_UnlockTexture, 4),
    E(gbh_AssignPalette, 8), E(gbh_RegisterPalette, 8), E(gbh_FreePalette, 4), E(gbh_ConvertColour, 12),
    E(gbh_InitImageTable, 4), E(gbh_FreeImageTable, 0), E(gbh_LoadImage, 4), E(gbh_BlitImage, 28),
    E(gbh_BeginScene, 0), E(gbh_EndScene, 0), E(gbh_SetWindow, 16), E(MakeScreenTable, 12),
    E(gbh_GetGlobals, 0), E(gbh_SetColourDepth, 0), E(gbh_GetUsedCache, 4), E(gbh_BeginLevel, 0),
    E(gbh_EndLevel, 0), E(gbh_SetCamera, 16), E(gbh_ResetLights, 0), E(gbh_AddLight, 4),
    E(gbh_SetAmbient, 4), E(ConvertColourBank, 4), E(SetShadeTableA, 20), E(gbh_PrintBitmap, 4),
    E(gbh_Convert16BitGraphic, 8), E(gbh_BlitBuffer, 24),
    { NULL, 0, NULL },
};

void d3ddll_register(void) { modules_set_impl("d3ddll.dll", g_impl); }
