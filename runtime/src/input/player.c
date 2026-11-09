#include "input/player.h"

#include "guest_memory.h"
#include "recomp_types.h"

static int heap_ptr(uint32_t p, uint32_t span) { return p >= GUEST_HEAP_LO && p + span <= guest_heap_top(); }

static uint32_t player_ped(void)
{
    uint32_t game = MEM32(0x5EB4FCu);
    if (!heap_ptr(game, 0x40)) return 0;
    uint32_t player = MEM32(game + 0x38u);
    if (!heap_ptr(player, 0x2C8)) return 0;
    uint32_t ped = MEM32(player + 0x2C4u);
    return heap_ptr(ped, 0x170) ? ped : 0;
}

int16_t *player_facing(void)
{
    uint32_t ped = player_ped();
    if (!ped) return NULL;
    uint32_t spr = MEM32(ped + 0x168u);
    if (!heap_ptr(spr, 0x44)) return NULL;
    int16_t *a = (int16_t *)XBOX_PTR(spr + 0x40u);
    return *a >= 0 && *a < 1440 ? a : NULL;
}

int player_on_foot(void)
{
    uint32_t ped = player_ped();
    return ped && MEM32(ped + 0x16Cu) == 0;
}
