/* Shared between the video driver (dmavideo.c) and the renderer (d3ddll). */
#pragma once
#include <stdint.h>
#include "video/presenter.h"

void dmavideo_register(void);

uint32_t video_state(void);                 /* guest VA of the driver state struct */
const void *video_surface(uint32_t *pitch); /* host pointer to the RGB565 surface */

/* The renderer installs a callback that draws its 3D scene into each frame. */
void video_set_scene(PresenterSceneFn fn, void *ctx);
PresenterSceneFn video_scene_fn(void);
void *video_scene_ctx(void);
