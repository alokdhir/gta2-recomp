#pragma once
#include <stdint.h>

/* Scale2x-based pixel-art upscale by 2 or 4 (0xAARRGGBB pixels). Returns a
 * malloc'd w*factor x h*factor image, or NULL for any other factor. */
uint32_t *upscale_pixels(const uint32_t *src, int w, int h, int factor);

/* Configured factor: GTA2_UPSCALE=1|2|4 (default 2). */
int upscale_factor(void);
