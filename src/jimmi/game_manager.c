#include "game_manager.h"
#include "api/callbacks.h"
#include "device/device.h"
#include "device/rdram/rdram.h"
#include "main/main.h"
#include "main/rom.h"


const static RemixMeta REMIX_META =  {"Smash Remix", 3236924630, 1440317707};

static int g_GameType = GAME_IS_REMIX;
static int g_BackButtonDisabled = 0;

// TODO: Make a more reliable validation method (MD5 probably)
int game_manager_get_is_remix(uint32_t crc1, uint32_t crc2)
{
    if (crc1 == REMIX_META.crc1 && crc2 == REMIX_META.crc2)
    {
        g_GameType = GAME_IS_REMIX;
        return 1;
    }
    return 0;
}

// Just assume the game is remix for now
int game_manager_get_game() {
    return GAME_IS_REMIX;
}


int game_manager_get_game_status()
{
    struct rdram* rdram = &g_dev.rdram;
    uint32_t virtual_addr = 0x800A4D19;  // Game status address
    uint32_t physical_offset = virtual_addr & 0x3FFFFF;  // Convert to physical RDRAM offset
    uint32_t status = 0;

    // Validate address is within RDRAM bounds
    if (physical_offset >= rdram->dram_size) {
        DebugMessage(M64MSG_ERROR, "Game Manager: Address 0x%X out of RDRAM bounds", virtual_addr);
        return 0;
    }

    status = rdram->dram[physical_offset >> 2];

    return status;
}

int game_manager_get_stage_id()
{
    struct rdram* rdram = &g_dev.rdram;
    uint32_t virtual_addr = 0x800A4D09;  // Stage ID address
    uint32_t physical_offset = virtual_addr & 0x3FFFFF;  // Convert to physical RDRAM offset
    int stage_id = 0;

    // Validate address is within RDRAM bounds
    if (physical_offset >= rdram->dram_size) {
        DebugMessage(M64MSG_ERROR, "Game Manager: Address 0x%X out of RDRAM bounds", virtual_addr);
        return 0;
    }

    stage_id = rdram->dram[physical_offset >> 2];

    return stage_id;
}

int game_manager_get_current_screen()
{
    struct rdram* rdram = &g_dev.rdram;
    uint32_t virtual_addr = 0x800A4AD0;  // Current screen address
    uint32_t physical_offset = virtual_addr & 0x3FFFFF;  // Convert to physical RDRAM offset
    int current_screen = 0;

    // Validate address is within RDRAM bounds
    if (physical_offset >= rdram->dram_size) {
        DebugMessage(M64MSG_ERROR, "Game Manager: Address 0x%X out of RDRAM bounds", virtual_addr);
        return 0;
    }

    current_screen = rdram->dram[physical_offset >> 2];

    return current_screen;
}

int game_manager_get_last_screen()
{
    struct rdram* rdram = &g_dev.rdram;
    uint32_t virtual_addr = 0x800A4AD1;  // last screen address
    uint32_t physical_offset = virtual_addr & 0x3FFFFF;  // Convert to physical RDRAM offset
    int last_screen = 0;

    // Validate address is within RDRAM bounds
    if (physical_offset >= rdram->dram_size) {
        DebugMessage(M64MSG_ERROR, "Game Manager: Address 0x%X out of RDRAM bounds", virtual_addr);
        return 0;
    }

    last_screen = rdram->dram[physical_offset >> 2];

    return last_screen;
}

void game_manager_disable_css_back_button()
{
    struct rdram* rdram = &g_dev.rdram;
    uint32_t virtual_addr = 0x80138218;  // CSS function to check if back button was pressed
    uint32_t physical_offset = virtual_addr & 0x3FFFFF;  // Convert to physical RDRAM offset

    // Validate address is within RDRAM bounds
    if (physical_offset >= rdram->dram_size) {
        DebugMessage(M64MSG_ERROR, "Game Manager: Address 0x%X out of RDRAM bounds", virtual_addr);
        return 0;
    }

    uint32_t nop_return = 0x24000000;  // addiu v0, r0, 0
    rdram->dram[physical_offset >> 2] = nop_return;

    // Write: jr ra (jump to return address)
    uint32_t jr_ra = 0x03E00008;
    rdram->dram[(physical_offset >> 2) + 1] = jr_ra;

    g_BackButtonDisabled = 1;
    DebugMessage(M64MSG_INFO, "Game Manager: CSS back button disabled");
}

int game_manager_is_css_back_button_disabled()
{
    return g_BackButtonDisabled;
}
