/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - vi_controller.c                                         *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2014 Bobby Smiles                                       *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include "vi_controller.h"

#include <string.h>

#include "api/m64p_types.h"
#include "device/memory/memory.h"
#include "device/r4300/r4300_core.h"
#include "device/rcp/mi/mi_controller.h"
#include "main/main.h"
#include "main/savestates.h"
#include "plugin/plugin.h"
#include "backends/plugins_compat/plugins_compat.h"
#include "jimmi/frame_manager.h"
#include "jimmi/input_manager.h"
#include "jimmi/replay_manager.h"
#include "jimmi/playback_manager.h"
#include "jimmi/game_manager.h"


unsigned int vi_clock_from_tv_standard(m64p_system_type tv_standard)
{
    switch(tv_standard)
    {
    case SYSTEM_PAL:
        return 49656530;
    case SYSTEM_MPAL:
        return 48628316;
    case SYSTEM_NTSC:
    default:
        return 48681812;
    }
}

unsigned int vi_expected_refresh_rate_from_tv_standard(m64p_system_type tv_standard)
{
    switch (tv_standard)
    {
    case SYSTEM_PAL:
        return 50;
    case SYSTEM_NTSC:
    case SYSTEM_MPAL:
    default:
        return 60;
    }
}

void set_vi_vertical_interrupt(struct vi_controller* vi)
{
    if (!get_event(&vi->mi->r4300->cp0.q, VI_INT) && (vi->regs[VI_V_INTR_REG] < vi->regs[VI_V_SYNC_REG]))
    {
        cp0_update_count(vi->mi->r4300);
        add_interrupt_event(&vi->mi->r4300->cp0, VI_INT, vi->delay);
    }
}

void init_vi(struct vi_controller* vi, unsigned int clock, unsigned int expected_refresh_rate,
             struct mi_controller* mi, struct rdp_core* dp)
{
    vi->clock = clock;
    vi->expected_refresh_rate = expected_refresh_rate;
    vi->mi = mi;
    vi->dp = dp;
}

void poweron_vi(struct vi_controller* vi)
{
    memset(vi->regs, 0, VI_REGS_COUNT*sizeof(uint32_t));
    vi->field = 0;
    vi->delay = 0;
    vi->count_per_scanline = 0;
}

void read_vi_regs(void* opaque, uint32_t address, uint32_t* value)
{
    struct vi_controller* vi = (struct vi_controller*)opaque;
    uint32_t reg = vi_reg(address);
    const uint32_t* cp0_regs = r4300_cp0_regs(&vi->mi->r4300->cp0);

    if (reg == VI_CURRENT_REG)
    {
        uint32_t* next_vi = get_event(&vi->mi->r4300->cp0.q, VI_INT);
        if (next_vi != NULL) {
            cp0_update_count(vi->mi->r4300);
            vi->regs[VI_CURRENT_REG] = (vi->delay - (*next_vi - cp0_regs[CP0_COUNT_REG])) / vi->count_per_scanline;

            /* wrap around VI_CURRENT_REG if needed */
            if (vi->regs[VI_CURRENT_REG] >= vi->regs[VI_V_SYNC_REG])
                vi->regs[VI_CURRENT_REG] -= vi->regs[VI_V_SYNC_REG];
        }

        /* update current field */
        vi->regs[VI_CURRENT_REG] = (vi->regs[VI_CURRENT_REG] & (~1)) | vi->field;
    }

    if (reg < VI_REGS_COUNT)
    {
        *value = vi->regs[reg];
    }
}

void write_vi_regs(void* opaque, uint32_t address, uint32_t value, uint32_t mask)
{
    struct vi_controller* vi = (struct vi_controller*)opaque;
    uint32_t reg = vi_reg(address);

    switch(reg)
    {
    case VI_STATUS_REG:
        if ((vi->regs[VI_STATUS_REG] & mask) != (value & mask))
        {
            masked_write(&vi->regs[VI_STATUS_REG], value, mask);
            gfx.viStatusChanged();
        }
        return;

    case VI_WIDTH_REG:
        if ((vi->regs[VI_WIDTH_REG] & mask) != (value & mask))
        {
            masked_write(&vi->regs[VI_WIDTH_REG], value, mask);
            gfx.viWidthChanged();
        }
        return;

    case VI_CURRENT_REG:
        clear_rcp_interrupt(vi->mi, MI_INTR_VI);
        return;

    case VI_V_SYNC_REG:
        if ((vi->regs[VI_V_SYNC_REG] & mask) != (value & mask))
        {
            masked_write(&vi->regs[VI_V_SYNC_REG], value, mask);
            vi->count_per_scanline = (vi->clock / vi->expected_refresh_rate) / (vi->regs[VI_V_SYNC_REG] + 1);
            vi->delay = (vi->regs[VI_V_SYNC_REG] + 1) * vi->count_per_scanline;
            set_vi_vertical_interrupt(vi);
        }
        return;

    case VI_V_INTR_REG:
        masked_write(&vi->regs[VI_V_INTR_REG], value, mask);
        set_vi_vertical_interrupt(vi);
        return;
    }

    if (reg < VI_REGS_COUNT)
    {
        masked_write(&vi->regs[reg], value, mask);
    }
}

void vi_vertical_interrupt_event(void* opaque)
{
    struct vi_controller* vi = (struct vi_controller*)opaque;
    if (vi->dp->do_on_unfreeze & DELAY_DP_INT)
        vi->dp->do_on_unfreeze |= DELAY_UPDATESCREEN;
    else
        gfx.updateScreen();

    /* allow main module to do things on VI event */
    new_vi();

    /* toggle vi field if in interlaced mode */
    vi->field ^= (vi->regs[VI_STATUS_REG] >> 6) & 0x1;

    /* Jimmi frame logic */
    uint64_t old_f = frame_manager_get_frame_index();
    
    // Check game status BEFORE polling new frame to detect transitions
    int current_game_status = game_manager_get_game_status();
    int match_ongoing = current_game_status == REMIX_STATUS_ONGOING;
                        current_game_status == REMIX_STATUS_UNPAUSED; // TEST
    static int last_game_status = 0;
    int prev_was_wait = last_game_status == REMIX_STATUS_WAIT ||
                        last_game_status == REMIX_STATUS_PAUSED; // TEST
    
    int playback_enabled = playback_manager_is_enabled();
    int replays_enabled = replay_manager_is_enabled();
    
    // RECORDING: If we just transitioned from WAIT -> ONGOING, we must save the PREVIOUS frame (N-1)
    // which is currently in input_manager.
    if (replays_enabled && !playback_enabled && prev_was_wait && match_ongoing)
    {
         char* replay_path = replay_manager_get_path();
         if (replay_path != NULL)
         {
             FILE * replay_file = replay_manager_get_file();
             if (replay_file != NULL)
             {
                 // Write input for all 4 controller ports (Data is from N-1)
                 replay_manager_write_input(replay_file, 0, old_f, input_manager_get_raw(0));
                 replay_manager_write_input(replay_file, 1, old_f, input_manager_get_raw(1));
                 replay_manager_write_input(replay_file, 2, old_f, input_manager_get_raw(2));
                 replay_manager_write_input(replay_file, 3, old_f, input_manager_get_raw(3));
                 DebugMessage(M64MSG_INFO, "Replay Manager: Captured transition frame %llu", old_f);
             }
         }
    }
    
    // Update Frame Index (increments to NEW frame)
    frame_manager_on_vi_interrupt();
    const uint64_t f_new = frame_manager_get_frame_index();
    input_manager_latch_for_frame(f_new);
    
    // STARTUP REPLAY LOAD: 
    // If this is the very first frame of a replay session, load the save state now.
    // This ensures we are "running" (in the loop) before loading, preventing thread/context issues.
    if (f_new == 1 && playback_enabled)
    {
        char state_path[4096];
        snprintf(state_path, sizeof(state_path), "%s/state.st", playback_manager_get_path());
        
        // Use the async job system which is safe to call from here
        DebugMessage(M64MSG_INFO, "Queueing initial replay save state load: %s", state_path);
        savestates_set_job(savestates_job_load, savestates_type_m64p, state_path);
    }

    // PLAYBACK or RECORDING (Standard Frame)
    if (playback_enabled && match_ongoing)
    {
        playback_manager_read_frame(f_new);
    }
    else
    {
        /* Poll controllers for live input */
        input_plugin_poll_all_controllers_for_frame(f_new);
    }


    if (replays_enabled && !playback_enabled && match_ongoing)
    {
        if ((f_new % 60) == 0)
        {
            DebugMessage(M64MSG_INFO, "Replay Manager: Current stage id: %d", game_manager_get_stage_id());
        }

        char* replay_path = replay_manager_get_path();
        if (replay_path != NULL)
        {
            FILE * replay_file = replay_manager_get_file();
            if (replay_file != NULL)
            {
                // Write input for all 4 controller ports
                int write_failed = 0;
                write_failed |= !replay_manager_write_input(replay_file, 0, f_new, input_manager_get_raw(0));
                write_failed |= !replay_manager_write_input(replay_file, 1, f_new, input_manager_get_raw(1));
                write_failed |= !replay_manager_write_input(replay_file, 2, f_new, input_manager_get_raw(2));
                write_failed |= !replay_manager_write_input(replay_file, 3, f_new, input_manager_get_raw(3));
                
                if (write_failed)
                {
                    DebugMessage(M64MSG_WARNING, "Replay Manager: Failed to write input for frame %llu", f_new);
                }
            }
        }
    }
    
    last_game_status = current_game_status;


    /* schedule next vertical interrupt */
    uint32_t next_vi = *get_event(&vi->mi->r4300->cp0.q, VI_INT) + vi->delay;
    remove_interrupt_event(&vi->mi->r4300->cp0);
    add_interrupt_event_count(&vi->mi->r4300->cp0, VI_INT, next_vi);

    /* trigger interrupt */
    raise_rcp_interrupt(vi->mi, MI_INTR_VI);
}

