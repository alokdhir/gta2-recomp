#pragma once
#include <stdint.h>

/* Current state of every key, indexed by DirectInput scan code (DIK_*): 0x80 = down. */
extern uint8_t g_key_down[256];

/* Map a macOS virtual key code; returns 0 if the key is not mapped. */
int keymap_from_mac(unsigned mac_keycode, uint8_t *vk, uint8_t *dik);
