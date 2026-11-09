/* The game's lights for the current frame (gbh_ResetLights / gbh_AddLight /
 * gbh_SetAmbient), shared between the original-style renderer and the 3D one. */
#pragma once

typedef struct {
    float x, y, z;      /* world: tiles, tiles, levels */
    float radius;       /* tiles */
    float radius2;
    float intensity;    /* 0..1 */
    float r, g, b;      /* 0..1 */
} GameLight;

/* Ambient level, 0..255; 255 means full daylight (the game skips lighting). */
float render_ambient(void);
int render_lights(const GameLight **out);
