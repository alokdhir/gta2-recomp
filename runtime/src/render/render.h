/* Draw list and GPU textures for the native d3ddll replacement.
 * d3ddll.c (guest-facing exports) fills it; render_metal.m draws it. */
#pragma once
#include <stdint.h>

typedef struct {
    float x, y;    /* game screen pixels */
    float u, v;    /* normalised texture coordinates */
    uint32_t argb; /* 0xAARRGGBB */
} RVert;

typedef enum { RB_ALPHA = 0, RB_ADDITIVE = 1, RB_OPAQUE = 2 } RBlend;

/* GPU textures, addressed by small integer ids (-1 = untextured). */
int render_texture_create(int width, int height);
void render_texture_upload(int id, int width, int height, const uint32_t *rgba);
void render_texture_free(int id);

/* Frame */
void render_begin_frame(void);
void render_set_screen(float width, float height);
void render_triangles(const RVert *v, int count, int tex, RBlend blend, int point_sample);
void render_install(void); /* hook into the presenter's frame */
