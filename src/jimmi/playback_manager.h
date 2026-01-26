#include <stdint.h>
#include <stdio.h>
#ifndef M64P_JIMMI_PLAYBACK_MANAGER_H
#define M64P_JIMMI_PLAYBACK_MANAGER_H

typedef struct {
    int controller_index;
    uint64_t frame_index;
    uint32_t raw_input;
} PlaybackInputRecord;

void playback_manager_init(void);
int playback_manager_is_enabled(void);
char* playback_manager_get_path(void);
FILE* playback_manager_get_file(void);
FILE* playback_manager_open(void);

/* Read the next input record from the playback file
 * Returns 1 if successfully read a record, 0 if EOF or error */
int playback_manager_read_input(PlaybackInputRecord* out_record);

void playback_manager_close(void);

#endif /* M64P_JIMMI_PLAYBACK_MANAGER_H */
