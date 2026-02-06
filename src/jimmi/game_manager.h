#include <stdint.h>
#ifndef M64P_JIMMI_GAME_MANAGER_H
#define M64P_JIMMI_GAME_MANAGER_H

typedef struct {
    char name[20];
    uint32_t crc1;
    uint32_t crc2;
} RemixMeta;

enum {
    REMIX_STATUS_WAIT = 0,             // Entered CSS after game start
    REMIX_STATUS_ONGOING = 65536,      // VS match in progress, players are receiving inputs
    REMIX_STATUS_PAUSED = 131072,      // Game paused during VS match
    REMIX_STATUS_UNPAUSED = 196608,    // Game is coming out of a pause during a VS match (players also receiving inputs?)
    REMIX_STATUS_MATCHEND = 458752,   // Match finished, stays in this state through next CSS until SSS?
    REMIX_STATUS_RESET = 16777216,     // Initial game state on boot or reset
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
int game_manager_get_game_status(void);
int game_manager_get_stage_id(void);
int game_manager_get_game(void);
int game_manager_get_current_screen(void);
int game_manager_get_last_screen(void);
void game_manager_disable_css_back_button(void);
int game_manager_is_css_back_button_disabled(void);

#endif /* M64P_JIMMI_GAME_MANAGER_H */