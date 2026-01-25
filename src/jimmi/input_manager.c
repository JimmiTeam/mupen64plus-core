#include "input_manager.h"
#include "api/callbacks.h"
#include <string.h>

static JimmiControllerState ports[4];
static uint32_t raw_ports[4];
static uint8_t has_ports[4];
static uint64_t latched_frame_index = 0;


void input_manager_init(void)
{
    memset(ports, 0, sizeof(ports));
    latched_frame_index = 0;
}


uint64_t input_manager_get_latched_frame_index(void)
{
    return latched_frame_index;
}


const JimmiControllerState* input_manager_get_controller_state(int port_index)
{
    if (port_index < 0 || port_index >= 4)
    {
        return NULL;
    }
    return &ports[port_index];
}


int input_manager_has_input(unsigned int port_index)
{
    if (port_index >= 4)
        return 0;
    return has_ports[port_index] != 0;
}


uint32_t input_manager_get_raw(unsigned int port_index)
{
    if (port_index >= 4)
        return 0;
    return raw_ports[port_index];
}


void input_manager_latch_for_frame(uint64_t frame_index)
{
    latched_frame_index = frame_index;

    // if ((frame_index % 60) == 0)
    // {
    //     DebugMessage(M64MSG_INFO,
    //         "Input Manager: Latched input for frame=%llu p1 buttons=%04x stick=(%d,%d)",
    //         frame_index,
    //         ports[0].buttons,
    //         ports[0].stick_x,
    //         ports[0].stick_y
    //     );
    // }
}


void input_manager_record_raw(unsigned int port_index, uint64_t frame_index, uint32_t packed_input)
{
    if (port_index >= 4)
        return;

    if (frame_index != latched_frame_index)
    {
        DebugMessage(M64MSG_WARNING,
            "[InputManager] record_raw for frame=%llu but latched_frame=%llu (port=%u)",
            (unsigned long long)frame_index,
            (unsigned long long)latched_frame_index,
            port_index);
    }

    raw_ports[port_index] = packed_input;
    ports[port_index] = decode_input(packed_input);
    has_ports[port_index] = 1;

    
    if ((frame_index % 60) == 0)
    {
        JimmiControllerState p1 = ports[0];
        DebugMessage(M64MSG_INFO,
        "[P1] f=%llu buttons=%04x stick=(%d,%d) raw=%08x",
        (unsigned long long)frame_index,
        p1.buttons, (int)p1.stick_x, (int)p1.stick_y,
        raw_ports[0]);
    }
}
