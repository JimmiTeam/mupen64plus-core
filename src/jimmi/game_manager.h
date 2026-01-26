#include <stdint.h>
#ifndef M64P_JIMMI_GAME_MANAGER_H
#define M64P_JIMMI_GAME_MANAGER_H

typedef struct {
    char name[20];
    uint32_t crc1;
    uint32_t crc2;
} RemixMeta;

enum {
    REMIX_WAIT = 0,             // Entered CSS after game start
    REMIX_ONGOING = 65536,      // Match in progress
    REMIX_PAUSED = 131072,      // Game paused
    REMIX_UNPAUSED = 196608,    // Game unpaused
    REMIX_MATCH_END = 458752,   // Match ended, stays like this during next CSS until SSS?
    REMIX_RESET = 16777216,     // Initial game state
};

int game_manager_get_is_remix(uint32_t crc1, uint32_t crc2);
int game_manager_get_game_status();

#endif /* M64P_JIMMI_GAME_MANAGER_H */