/* Sprite texture pack (3D renderer): replacement images for the game's sprite
 * textures, matched by a hash of the final colours the game hands the
 * renderer -- so every colour remap and damage state of a car is its own
 * entry, as in RTX Remix.
 *
 *   textures/<style name>/sprites/<hash>.png   (any size; scaled to fit)
 *
 * GTA2_DUMP_SPRITES=<dir> writes each distinct world sprite the game draws as
 * <dir>/<style name>/<hash>_<w>x<h>_p<palette>.png, the input for scripts/texpack.py
 * sprites (the palette id helps tell people from objects). */
#pragma once
#include <stdint.h>

/* FNV-1a 64 over size and 0xAARRGGBB pixels. */
uint64_t texpack_hash(const uint32_t *rgba, int w, int h);

/* Replacement for this sprite, or NULL. *w, *h receive its size; free() it. */
uint32_t *texpack_sprite(uint64_t hash, int *w, int *h);

/* With GTA2_DUMP_SPRITES set, save this sprite once. */
void texpack_dump(uint64_t hash, const uint32_t *rgba, int w, int h, int pal);
