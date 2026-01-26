#include <stdint.h>
#include <stdio.h>
#ifndef M64P_JIMMI_REPLAY_MANAGER_H
#define M64P_JIMMI_REPLAY_MANAGER_H


void replay_manager_init(void);
int replay_manager_is_enabled(void);
char* replay_manager_get_path(void);
FILE* replay_manager_get_file(void);
FILE * replay_manager_open();
int replay_manager_write_input(FILE * file, int controller_index, uint64_t frame_index, uint32_t raw_input);
void replay_manager_close(void);

#endif /* M64P_JIMMI_REPLAY_MANAGER_H */
