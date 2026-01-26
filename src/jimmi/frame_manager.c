#include "frame_manager.h"
#include "replay_manager.h"
#include "api/callbacks.h"


/*
 * The frame manager keeps track of the canonical emulation "frame",
 * triggering on every VI interrupt. All replay, netplay and rollback
 * systems depend on this frame index.
 */

static uint64_t frame_index = 0;
static uint64_t last_seen_frame_index;

void frame_manager_init(void)
{
    frame_index = 0;
    last_seen_frame_index = 0;
}


void frame_manager_on_vi_interrupt(void)
{
    frame_index++;
    if (frame_index != last_seen_frame_index)
    {
        last_seen_frame_index = frame_index;
    }
    else 
    {
        DebugMessage(M64MSG_WARNING, "Frame Manager: Detected repeated frame index %llu", frame_index);
    }

    // if ((frame_index % 60) == 0)
    // {
    //     DebugMessage(M64MSG_INFO, "Frame Manager: Reached frame index %llu", frame_index);
    // }
}


uint64_t frame_manager_get_frame_index(void)
{
    return frame_index;
}
