/* The local player's state, read straight from the game's objects. Offsets
 * found by snapshotting memory (GTA2_MEMSNAP, Debug > Save Memory Snapshot):
 *
 *   [0x5EB4FC]       game object
 *   game   + 0x38    local player
 *   player + 0x2C4   the player's ped
 *   ped    + 0x168   the ped's sprite
 *   ped    + 0x16C   the car the ped is in (0 on foot)
 *   sprite + 0x40    rotation: 0..1439 quarter degrees, 720 = north (up the
 *                    screen), increasing anticlockwise
 */
#pragma once
#include <stdint.h>

/* The player's facing, writable; NULL when there's no valid player. */
int16_t *player_facing(void);

/* 1 when the player is on foot (not in a car), 0 otherwise or unknown. */
int player_on_foot(void);
