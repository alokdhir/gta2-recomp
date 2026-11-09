/* Metal presentation: owns the device, the window's CAMetalLayer and the
 * frame. The video driver (dmavideo.c) hands it the game's 16-bit
 * framebuffer; the renderer (d3ddll, TREK-5) adds its draws through the
 * scene hook. */
#pragma once
#include <stdint.h>

#ifdef __OBJC__
#import <Metal/Metal.h>
typedef void (*PresenterSceneFn)(id<MTLRenderCommandEncoder> enc, void *ctx);
#else
typedef void (*PresenterSceneFn)(void *enc, void *ctx);
#endif

/* Bind to the window's layer and set the logical game resolution. */
int presenter_init(void *metal_layer, int width, int height);

/* Draw one frame and present it.
 *   fb/pitch: RGB565 pixels of the game's surface (NULL = none this frame)
 *   scene:    optional callback to draw 3D content beneath the 2D surface */
void presenter_frame(const void *fb, uint32_t pitch, PresenterSceneFn scene, void *ctx);

/* NDC rectangle (x0, y0, x1, y1) the game's screen maps to this frame (letterboxed). */
void presenter_fit_rect(float out[4]);
/* Which part of the frame the scene callback is drawing: PHASE_ALL (no
 * bloom), or PHASE_WORLD (offscreen, bloomed) then PHASE_OVERLAY (HUD etc.,
 * straight onto the final image). */
enum { PHASE_ALL = 0, PHASE_WORLD = 1, PHASE_OVERLAY = 2 };
int presenter_phase(void);
#ifdef __OBJC__
/* Called each frame with the frame's command buffer before the main pass
 * (e.g. to render a shadow map that the main pass then samples). */
typedef void (*PresenterPrepassFn)(id<MTLCommandBuffer> cb);
void presenter_set_prepass(PresenterPrepassFn fn);
#endif
#ifdef __OBJC__
id<MTLDevice> presenter_device(void);
#endif

int presenter_width(void);
int presenter_height(void);
void presenter_set_gamma(float r, float g, float b);
void presenter_shutdown(void);

/* Capture ab_original.png and ab_3d.png (into $GTA2_AB_DIR or the game dir) from the next frames. */
void presenter_request_ab(void);
