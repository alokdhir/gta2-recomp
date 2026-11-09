/* Native-side API of the window layer (user32.m), for video/renderer modules. */
#pragma once
#include <stdint.h>

void user32_init(void);
/* CAMetalLayer* (as void*) backing the window; hwnd 0 = the first window. */
void *user32_metal_layer(uint32_t hwnd);
void user32_set_client_size(uint32_t hwnd, int width, int height);

/* Press/release a key as if typed (macOS key code); used by the gamepad mapper. */
void user32_inject_key(unsigned mac_keycode, int down);
