#include <stdint.h>
#ifndef M64P_JIMMI_FRAME_MANAGER_H
#define M64P_JIMMI_FRAME_MANAGER_H

void frame_manager_init(void);
void frame_manager_on_vi_interrupt(void);

uint64_t frame_manager_get_frame_index(void);

#endif /* M64P_JIMMI_FRAME_MANAGER_H */