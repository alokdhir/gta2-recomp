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

int presenter_width(void);
int presenter_height(void);
void presenter_set_gamma(float r, float g, float b);
void presenter_shutdown(void);
