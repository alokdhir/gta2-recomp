/* Native-side API of the window layer (user32.m), for video/renderer modules. */
#pragma once
#include <stdint.h>

void user32_init(void);
/* CAMetalLayer* (as void*) backing the window; hwnd 0 = the first window. */
void *user32_metal_layer(uint32_t hwnd);
void user32_set_client_size(uint32_t hwnd, int width, int height);

/* Press/release a key as if typed (macOS key code); used by the gamepad mapper. */
void user32_inject_key(unsigned mac_keycode, int down);

/* Window title shows which renderer is active. */
void user32_update_title(void);

/* Developer actions (menu "Debug", Cmd-1/2/3, controller View + LB/RB/Y). */
void user32_action_toggle_renderer(void); /* original <-> 3D */
void user32_action_snapshot(void);        /* A/B screenshots + map log of the next frame */
void user32_action_replay(void);          /* save the last few seconds */
