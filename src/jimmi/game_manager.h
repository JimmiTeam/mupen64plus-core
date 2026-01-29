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

enum {
    REMIX_SCREEN_CSS = 269027079,
    REMIX_SCREEN_SSS = 353371911,
    REMIX_SCREEN_MATCH = 370476807,
};

enum
{
    GAME_IS_REMIX,
    GAME_IS_VANILLA,
};

int game_manager_get_is_remix(uint32_t crc1, uint32_t crc2);
int game_manager_get_game_status();
int game_manager_get_stage_id();
int game_manager_get_game();
int game_manager_get_current_screen();
int game_manager_get_last_screen();

#endif /* M64P_JIMMI_GAME_MANAGER_H */