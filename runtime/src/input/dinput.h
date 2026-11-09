#pragma once
#include <stdint.h>

/* Called by the window layer on every key transition (DIK scan code). */
void dinput_key_event(uint8_t dik, int down, uint32_t time_ms);
