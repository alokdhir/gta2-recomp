/* Pixel-art upscaling for textures (tiles and sprites).
 *
 * Scale2x / EPX: each source pixel P becomes a 2x2 block. A corner takes the
 * colour of the two neighbours it touches when those agree with each other
 * and the other pair disagrees -- which rounds off diagonal staircases while
 * keeping hard edges and flat areas exactly as drawn. Applied twice for 4x.
 * Colours compare including alpha, so transparent edges stay clean. */
#include "render3d/upscale.h"

#include <stdlib.h>
#include <string.h>

static void scale2x(const uint32_t *src, int w, int h, uint32_t *dst)
{
    int W = w * 2;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t P = src[y * w + x];
            uint32_t A = y > 0 ? src[(y - 1) * w + x] : P;     /* up */
            uint32_t B = x < w - 1 ? src[y * w + x + 1] : P;   /* right */
            uint32_t C = x > 0 ? src[y * w + x - 1] : P;       /* left */
            uint32_t D = y < h - 1 ? src[(y + 1) * w + x] : P; /* down */
            uint32_t e0 = P, e1 = P, e2 = P, e3 = P;
            if (C == A && C != D && A != B) e0 = A;
            if (A == B && A != C && B != D) e1 = B;
            if (D == C && D != B && C != A) e2 = C;
            if (B == D && B != A && D != C) e3 = D;
            uint32_t *o = dst + (size_t)(2 * y) * W + 2 * x;
            o[0] = e0;
            o[1] = e1;
            o[W] = e2;
            o[W + 1] = e3;
        }
    }
}

uint32_t *upscale_pixels(const uint32_t *src, int w, int h, int factor)
{
    if (factor != 2 && factor != 4) return NULL;
    uint32_t *a = malloc((size_t)w * h * 16);
    scale2x(src, w, h, a);
    if (factor == 2) return a;
    uint32_t *b = malloc((size_t)w * h * 64);
    scale2x(a, w * 2, h * 2, b);
    free(a);
    return b;
}

int upscale_factor(void)
{
    static int f = -1;
    if (f < 0) {
        const char *e = getenv("GTA2_UPSCALE");
        f = e ? atoi(e) : 2;
        if (f != 1 && f != 2 && f != 4) f = 2;
    }
    return f;
}
