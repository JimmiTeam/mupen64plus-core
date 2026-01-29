#include <stdint.h>
#ifndef M64P_JIMMI_INPUT_MANAGER_H
#define M64P_JIMMI_INPUT_MANAGER_H

typedef struct {
    uint16_t buttons;
    int8_t  stick_x;
    int8_t  stick_y;
} JimmiControllerState;


void input_manager_init(void);
void input_manager_latch_for_frame(uint64_t frame_index);
void input_manager_record_raw(int controller_index, uint64_t frame_index, uint32_t raw_input, int from_playback);
uint32_t input_manager_get_raw(int controller_index);
int input_manager_has_input(int controller_index);
int input_manager_is_from_playback(int controller_index);
const JimmiControllerState* input_manager_get_controller_state(int controller_index);
uint64_t input_manager_get_latched_frame_index(void);

static inline JimmiControllerState decode_input(uint32_t input)
{
    JimmiControllerState out;
    out.buttons = (uint16_t)(input & 0xFFFFu);
    out.stick_x = (int8_t)((input >> 16) & 0xFFu);
    out.stick_y = (int8_t)((input >> 24) & 0xFFu);

    return out;
}
static inline uint32_t encode_input(JimmiControllerState state)
{
    return ((uint32_t)(uint8_t)state.stick_y << 24) |
           ((uint32_t)(uint8_t)state.stick_x << 16) |
           (uint32_t)state.buttons;
}


#endif /* M64P_JIMMI_INPUT_MANAGER_H */
