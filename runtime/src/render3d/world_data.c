/* Live map + style file readers for the 3D renderer. See world_data.h. */
#include "render3d/world_data.h"

#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "winapi/imports.h"

/* Game globals (v9.6): map holder -> map object; DMAP arrays inside it. */
#define MAP_HOLDER_PTR 0x00662C08u
#define MAP_COLUMNS_PTR 0x40008u
#define MAP_BLOCKS_PTR 0x4000Cu

int world_map_live(WorldMap *m)
{
    uint32_t holder = MEM32(MAP_HOLDER_PTR);
    uint32_t obj = holder ? MEM32(holder) : 0;
    if (!obj) return 0;
    uint32_t cols = MEM32(obj + MAP_COLUMNS_PTR), blocks = MEM32(obj + MAP_BLOCKS_PTR);
    if (!cols || !blocks) return 0;
    m->base = GPTR(obj);
    m->columns = GPTR(cols);
    m->blocks = GPTR(blocks);
    return 1;
}

/* Column: u8 height, u8 offset, u16 pad, then (height - offset) u32 block indices
 * for levels offset .. height-1. */
const GBlock *world_map_block(const WorldMap *m, int x, int y, int z)
{
    if (x < 0 || y < 0 || x >= MAP_W || y >= MAP_H || z < 0 || z >= MAP_LEVELS) return NULL;
    const uint8_t *col = m->columns + 4u * m->base[y * MAP_W + x];
    int height = col[0], offset = col[1];
    if (z < offset || z >= height) return NULL;
    uint32_t idx;
    memcpy(&idx, col + 4 + 4 * (z - offset), 4);
    return &m->blocks[idx];
}

/* ---- style (.sty) ------------------------------------------------------------
 * "GBST", u16 version, then chunks: 4-char type, u32 size, data.
 *   TILE  tiles 64x64 8-bit, in 256x256 pages of 4x4 tiles
 *   PPAL  physical palettes, 64 per 64 KB page, interleaved: colour i of
 *         palette p at page + (i * 64 + p) * 4, as 0x00RRGGBB
 *   PALX  16384 u16: virtual palette -> physical palette
 *   PALB  palette bases; tiles use the first virtual palettes (tile n -> n) */

static char g_style_path[1024];

void world_note_style_path(const char *p) { snprintf(g_style_path, sizeof g_style_path, "%s", p); }
/* ---- tile animation ---- */
typedef struct { uint16_t base; uint8_t rate, repeat, len; uint16_t tiles[256]; } TileAnim;
static char g_map_path[1024], g_anim_for[1024];
static TileAnim *g_anims;
static int g_nanims;
static uint32_t g_anim_frame;

void world_note_map_path(const char *p) { snprintf(g_map_path, sizeof g_map_path, "%s", p); }
void world_anim_tick(void) { g_anim_frame++; }

static void anim_load(void)
{
    snprintf(g_anim_for, sizeof g_anim_for, "%s", g_map_path);
    free(g_anims);
    g_anims = NULL;
    g_nanims = 0;
    FILE *f = fopen(g_map_path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)n);
    if (fread(d, 1, (size_t)n, f) != (size_t)n || memcmp(d, "GBMP", 4)) n = 0;
    fclose(f);
    for (long off = 6; off + 8 <= n;) {
        uint32_t len;
        memcpy(&len, d + off + 4, 4);
        if (!memcmp(d + off, "ANIM", 4)) {
            for (long j = off + 8, e = off + 8 + (long)len; j + 6 <= e;) {
                TileAnim a = { 0 };
                memcpy(&a.base, d + j, 2);
                a.rate = d[j + 2], a.repeat = d[j + 3], a.len = d[j + 4];
                for (int k = 0; k < a.len; k++) memcpy(&a.tiles[k], d + j + 6 + 2 * k, 2);
                j += 6 + 2 * (long)a.len;
                if (!a.len) continue;
                g_anims = realloc(g_anims, sizeof *g_anims * (size_t)(g_nanims + 1));
                g_anims[g_nanims++] = a;
            }
        }
        off += 8 + (long)len;
    }
    free(d);
    rt_log("world: map %s: %d tile animations", g_map_path, g_nanims);
}

unsigned world_tile_anim(unsigned tile)
{
    if (!tile || !g_map_path[0]) return tile;
    if (strcmp(g_anim_for, g_map_path)) anim_load();
    for (int i = 0; i < g_nanims; i++) {
        const TileAnim *a = &g_anims[i];
        if (a->base != tile) continue;
        uint32_t step = g_anim_frame / (a->rate ? a->rate : 1u);
        if (a->repeat && step >= (uint32_t)a->repeat * a->len) return a->tiles[a->len - 1];
        return a->tiles[step % a->len];
    }
    return tile;
}

const char *world_style_path(void) { return g_style_path[0] ? g_style_path : NULL; }

int world_style_load(const char *path, WorldStyle *s)
{
    memset(s, 0, sizeof *s);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)n);
    if (fread(d, 1, (size_t)n, f) != (size_t)n || memcmp(d, "GBST", 4)) {
        fclose(f);
        free(d);
        return 0;
    }
    fclose(f);
    const uint8_t *tile = NULL, *ppal = NULL, *palx = NULL;
    uint32_t tile_len = 0, ppal_len = 0;
    for (long off = 6; off + 8 <= n;) {
        uint32_t len;
        memcpy(&len, d + off + 4, 4);
        const uint8_t *body = d + off + 8;
        if (!memcmp(d + off, "TILE", 4)) { tile = body; tile_len = len; }
        if (!memcmp(d + off, "PPAL", 4)) { ppal = body; ppal_len = len; }
        if (!memcmp(d + off, "PALX", 4)) palx = body;
        off += 8 + (long)len;
    }
    if (!tile || !ppal || !palx) {
        free(d);
        return 0;
    }
    s->ntiles = (int)(tile_len / (TILE_PX * TILE_PX));
    s->rgba = malloc((size_t)s->ntiles * TILE_PX * TILE_PX * 4);
    for (int t = 0; t < s->ntiles; t++) {
        uint16_t phys;
        memcpy(&phys, palx + 2 * t, 2);
        uint32_t page = phys / 64u, slot = phys % 64u;
        const uint8_t *pal = ppal + (size_t)page * 65536u;
        const uint8_t *src = tile + (size_t)(t / 16) * 65536u;
        int col = t % 4, row = (t / 4) % 4;
        uint32_t *dst = s->rgba + (size_t)t * TILE_PX * TILE_PX;
        for (int y = 0; y < TILE_PX; y++) {
            for (int x = 0; x < TILE_PX; x++) {
                uint8_t idx = src[(row * TILE_PX + y) * 256 + col * TILE_PX + x];
                uint32_t c = 0;
                size_t po = ((size_t)idx * 64u + slot) * 4u;
                if ((size_t)page * 65536u + po + 4 <= ppal_len) memcpy(&c, pal + po, 4);
                dst[y * TILE_PX + x] = (c & 0x00FFFFFFu) | (idx ? 0xFF000000u : 0u);
            }
        }
    }
    free(d);
    rt_log("world: style %s: %d tiles", path, s->ntiles);
    return 1;
}

void world_style_free(WorldStyle *s)
{
    free(s->rgba);
    memset(s, 0, sizeof *s);
}

/* ---- debug dump ----------------------------------------------------------------- */

static void write_png(const char *path, const uint32_t *argb, int w, int h)
{
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate((void *)argb, (size_t)w, (size_t)h, 8, (size_t)w * 4, cs,
                                             kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
    CGImageDestinationAddImage(dst, img, NULL);
    CGImageDestinationFinalize(dst);
    CFRelease(dst);
    CFRelease(url);
    CGImageRelease(img);
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
}

/* Top-down view: for every cell, the lid of the highest block that has one,
 * shaded darker the lower it is; 8 pixels per block. */
void world_debug_dump(void)
{
    static int done;
    const char *out = getenv("GTA2_DUMP_MAP");
    if (done || !out) return;
    WorldMap m;
    if (!world_map_live(&m) || !world_style_path()) return;
    done = 1;
    WorldStyle st;
    if (!world_style_load(world_style_path(), &st)) {
        rt_log("world: cannot load style %s", world_style_path());
        return;
    }
    const int S = 8, W = MAP_W * S;
    uint32_t *img = calloc((size_t)W * W, 4);
    int blocks = 0;
    for (int y = 0; y < MAP_H; y++) {
        for (int x = 0; x < MAP_W; x++) {
            for (int z = MAP_LEVELS - 1; z >= 0; z--) {
                const GBlock *b = world_map_block(&m, x, y, z);
                if (!b) continue;
                blocks++;
                unsigned t = FACE_TILE(b->lid);
                if (!t || (int)t >= st.ntiles) continue;
                const uint32_t *tp = st.rgba + (size_t)t * TILE_PX * TILE_PX;
                unsigned shade = 128 + (unsigned)z * 16;
                for (int py = 0; py < S; py++)
                    for (int px = 0; px < S; px++) {
                        uint32_t c = tp[(py * TILE_PX / S) * TILE_PX + px * TILE_PX / S];
                        uint32_t r = ((c >> 16) & 255) * shade / 255, g = ((c >> 8) & 255) * shade / 255,
                                 bl = (c & 255) * shade / 255;
                        img[(size_t)(y * S + py) * W + x * S + px] = 0xFF000000u | r << 16 | g << 8 | bl;
                    }
                break;
            }
        }
    }
    write_png(out, img, W, W);
    rt_log("world: dumped %s (%d blocks seen)", out, blocks);
    free(img);
    world_style_free(&st);
}
