#include "game_manager.h"
#include "api/callbacks.h"
#include "device/device.h"
#include "device/rdram/rdram.h"
#include "main/main.h"
#include "main/rom.h"


const static RemixMeta REMIX_META =  {"Smash Remix", 3236924630, 1440317707};


int game_manager_get_is_remix(uint32_t crc1, uint32_t crc2)
{
    if (crc1 == REMIX_META.crc1 && crc2 == REMIX_META.crc2)
    {
        return 1;
    }
    return 0;
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

    // Read 32-bit value from RDRAM (shift by 2 for uint32_t array indexing)
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

    // Read 8-bit value from RDRAM by casting to uint8_t* for byte-level access
    stage_id = rdram->dram[physical_offset >> 2];

    return stage_id;
}
