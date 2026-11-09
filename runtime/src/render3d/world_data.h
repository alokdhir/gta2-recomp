/* The live GTA2 world for the 3D renderer: the map as the running game holds
 * it (same layout as the .gmp DMAP chunk) and the tile graphics from the
 * level's style (.sty) file. */
#pragma once
#include <stdint.h>

#define MAP_W 256
#define MAP_H 256
#define MAP_LEVELS 8
#define TILE_PX 64

/* One block (12 bytes, as in the file). Face words: tile number in bits 0-9,
 * 0x0400 wall (collide), 0x0800 bullet wall, 0x1000 flat (transparent),
 * 0x2000 flip, rotation (quarter turns) in bits 14-15. For the lid, bits
 * 10-11 are the lighting level instead. slope: ground type in bits 0-1,
 * slope type in bits 2-7. */
typedef struct __attribute__((packed)) {
    uint16_t left, right, top, bottom, lid;
    uint8_t arrows, slope;
} GBlock;

#define FACE_TILE(f) ((f) & 0x3FFu)
#define FACE_FLAT(f) (((f) & 0x1000u) != 0)
#define FACE_FLIP(f) (((f) & 0x2000u) != 0)
#define FACE_ROT(f) (((f) >> 14) & 3u)

typedef struct {
    const uint32_t *base;     /* MAP_W * MAP_H column-word indices */
    const uint8_t *columns;   /* column pool (u32 words) */
    const GBlock *blocks;     /* block pool */
} WorldMap;

/* Read the map the game has loaded; 0 when no world is loaded (menus). */
int world_map_live(WorldMap *m);

/* Block at (x, y, level) or NULL if that cell is empty. */
const GBlock *world_map_block(const WorldMap *m, int x, int y, int z);

/* ---- style --------------------------------------------------------------- */

typedef struct {
    int ntiles;
    uint32_t *rgba; /* ntiles * 64 * 64 pixels, 0xAARRGGBB; colour 0 has alpha 0 */
} WorldStyle;

/* Called by CreateFileA when the game opens a .sty; remembered for loading. */
void world_note_style_path(const char *host_path);
const char *world_style_path(void);

/* Parse the style file; returns 0 on failure. */
int world_style_load(const char *path, WorldStyle *s);
void world_style_free(WorldStyle *s);

/* Debug: GTA2_DUMP_MAP=out.png writes a top-down image of the live map once. */
void world_debug_dump(void);
