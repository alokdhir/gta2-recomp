#pragma once
#include <stdint.h>
/* Debug: trap the next store to the 8 bytes at guest_va (see watch.c). */
void watch_arm(uint32_t guest_va);
void watch_rearm(void); /* once per frame */
