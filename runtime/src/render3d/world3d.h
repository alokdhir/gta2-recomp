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

#ifdef __OBJC__
#import <Metal/Metal.h>
/* Draw the world into the current frame (depth-tested). */
void world3d_draw(id<MTLRenderCommandEncoder> enc, const float fit[4]);
#endif
