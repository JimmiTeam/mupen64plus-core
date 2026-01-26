#include "replay_manager.h"
#include "api/callbacks.h"
#include "main/main.h"
#include "api/config.h"
#include <string.h>


static int replays_enabled;
static char* replay_path = NULL;
static FILE * replay_file = NULL;

void replay_manager_init(void)
{
    char replay_path_buffer[512] = {0};
    
    replays_enabled = ConfigGetParamBool(g_CoreConfig, "Record");
    
    if (replay_path != NULL)
    {
        free(replay_path);
        replay_path = NULL;
    }
    
    if (replays_enabled)
    {
        if (ConfigGetParameter(g_CoreConfig, "RecordPath", M64TYPE_STRING, replay_path_buffer, sizeof(replay_path_buffer)) == M64ERR_SUCCESS)
        {
            if (replay_path_buffer[0] != '\0')
            {
                replay_path = strdup(replay_path_buffer);
            }
        }
        replay_file = replay_manager_open();
    }
}

FILE * replay_manager_open()
{
    if (!replays_enabled || replay_path == NULL)
    {
        return NULL;
    }
    
    FILE *file = fopen(replay_path, "ab");
    if (file == NULL)
    {
        DebugMessage(M64MSG_ERROR, "Replay Manager: Failed to open replay file at path %s", replay_path);
    }
    return file;
}


void replay_manager_close(void)
{
    // Nothing to do here for now
}


int replay_manager_write_input(FILE * file, int controller_index, uint64_t frame_index, uint32_t raw_input)
{
    if (file == NULL)
    {
        return 0;
    }
    
    // Validate controller_index
    if (controller_index < 0 || controller_index >= 4)
    {
        DebugMessage(M64MSG_WARNING, "Replay Manager: Invalid controller_index %d", controller_index);
        return 0;
    }
    
    // Write frame data in format: controller_index | frame_index | raw_input
    if (fwrite(&controller_index, sizeof(int), 1, file) != 1)
    {
        DebugMessage(M64MSG_ERROR, "Replay Manager: Failed to write controller_index");
        return 0;
    }
    
    if (fwrite(&frame_index, sizeof(uint64_t), 1, file) != 1)
    {
        DebugMessage(M64MSG_ERROR, "Replay Manager: Failed to write frame_index");
        return 0;
    }
    
    if (fwrite(&raw_input, sizeof(uint32_t), 1, file) != 1)
    {
        DebugMessage(M64MSG_ERROR, "Replay Manager: Failed to write raw_input");
        return 0;
    }
    
    // Flush periodically to prevent data loss (every 60 frames)
    static uint64_t last_flush_frame = 0;
    if (frame_index - last_flush_frame >= 60)
    {
        fflush(file);
        last_flush_frame = frame_index;
    }
    
    return 1;
}


int replay_manager_is_enabled(void)
{
    return replays_enabled;
}

char* replay_manager_get_path(void)
{
    return replay_path;
}

FILE* replay_manager_get_file(void)
{
    return replay_file;
}
