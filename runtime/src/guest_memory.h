/* The guest's 32-bit address space, mapped at g_xbox_mem_offset on the host. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Fixed guest layout */
#define GUEST_TIB_VA      0x00090000u  /* thread information block (fs:) */
#define GUEST_STACK_LO    0x00100000u
#define GUEST_STACK_HI    0x00300000u  /* 2 MB main-thread stack */
#define GUEST_IMAGE_BASE  0x00400000u
#define GUEST_HEAP_LO     0x10000000u
#define GUEST_HEAP_HI     0xE0000000u

typedef struct {
    uint32_t entry;
    uint32_t image_size;
    uint32_t code_lo, code_hi;
} GuestImage;

void guest_memory_init(void);
void guest_load_pe(const char *path, GuestImage *out);
void guest_tib_init(void);

/* Guest heap (HeapAlloc/VirtualAlloc/malloc all land here). 0 on failure. */
uint32_t guest_alloc(uint32_t size, int zero);
void guest_free(uint32_t va);
uint32_t guest_realloc(uint32_t va, uint32_t size);
uint32_t guest_alloc_size(uint32_t va);

/* Copy a host string into fresh guest memory; returns its VA. */
uint32_t guest_strdup(const char *s);
