/* Native-side API of the window layer (user32.m), for video/renderer modules. */
#pragma once
#include <stdint.h>

void user32_init(void);
/* CAMetalLayer* (as void*) backing the window; hwnd 0 = the first window. */
void *user32_metal_layer(uint32_t hwnd);
void user32_set_client_size(uint32_t hwnd, int width, int height);

/* Process pending window, keyboard and controller events (for modal loops
 * such as the intro movie). */
void user32_pump(void);
/* While on, key presses (keyboard or controller) are counted by
 * user32_presses() instead of reaching the game. */
void user32_swallow_keys(int on);
unsigned user32_presses(void);

/* Press/release a key as if typed (macOS key code); used by the gamepad mapper. */
void user32_inject_key(unsigned mac_keycode, int down);

/* Window title shows which renderer is active. */
void user32_update_title(void);

/* Developer actions (menu "Debug", Cmd-1/2/3, controller View + LB/RB/Y). */
void user32_action_toggle_renderer(void); /* original <-> 3D */
void user32_action_snapshot(void);        /* A/B screenshots + map log of the next frame */
void user32_action_replay(void);          /* save the last few seconds */
void user32_action_memsnap(void);         /* save a memory snapshot */
