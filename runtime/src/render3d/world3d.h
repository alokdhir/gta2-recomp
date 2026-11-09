/* 3D world renderer: draws the live map as real geometry in place of the
 * game's screen-space tiles. Toggled with F9 (remembered in the registry). */
#pragma once
#include <stdint.h>

int world3d_enabled(void);
void world3d_toggle(void);
void world3d_set(int on); /* without saving the preference */

/* Camera fit: every map vertex the game projects is reported with its screen
 * position, its 1/z, and the absolute world position the game stored for it.
 * The camera is solved from these each frame, so the 3D view lines up with
 * the game's own sprites and HUD exactly. */
void world3d_observe(float sx, float sy, float zf, float wx, float wy, float wz);
void world3d_begin_frame(float screen_w, float screen_h);

/* A world sprite: four corners (world tiles/levels), uv 0..1, ARGB colour. */
typedef struct {
    float x, y, z, u, v;
    uint32_t argb;
} WSpriteVert;
/* blend: 0 alpha (colour-keyed), 1 additive, 2 opaque (render.h RBlend) */
void world3d_sprite(const WSpriteVert q[4], int tex, int blend);

/* Screen pixel -> world x,y on the plane at height z, with the fitted camera.
 * Returns 0 before the first fit. */
/* Hook the shadow/mesh prepass into the presenter (once at startup). */
void world3d_install_prepass(void);

int world3d_unproject(float sx, float sy, float z, float *wx, float *wy);
/* Screen pixel + the game's 1/depth -> world position (x, y, z). */
int world3d_unproject_zf(float sx, float sy, float zf, float out[3]);

#ifdef __OBJC__
#import <Metal/Metal.h>
/* Draw the world into the current frame (depth-tested). */
void world3d_draw(id<MTLRenderCommandEncoder> enc, const float fit[4]);
#endif
