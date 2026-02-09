#include <stdint.h>
#include <stdio.h>
#ifndef M64P_JIMMI_REPLAY_MANAGER_H
#define M64P_JIMMI_REPLAY_MANAGER_H


void replay_manager_init(void);
int replay_manager_is_enabled(void);
char* replay_manager_get_path(void);
char* replay_manager_generate_path(char* folder);
int replay_manager_set_path(char* path);
FILE* replay_manager_get_file(void);
void replay_manager_open(char* folder);
void replay_manager_close(void);
int replay_manager_write_input(FILE * file, int controller_index, uint64_t frame_index, uint32_t raw_input);
void replay_manager_commit_frames(uint64_t current_frame);

// Write all 4 controller records for a frame in order
int replay_manager_write_frame(FILE * file, uint64_t frame_index, const uint32_t raw_inputs[4]);

#endif /* M64P_JIMMI_REPLAY_MANAGER_H */
