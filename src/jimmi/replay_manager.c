#include "replay_manager.h"
#include "game_manager.h"
#include "api/callbacks.h"
#include "main/main.h"
#include "api/config.h"
#include <string.h>
#include <time.h>
#include <stdio.h>
#include "osal/files.h"


static int replays_enabled;
static char* replay_path = NULL;
static FILE * replay_file = NULL;

void replay_manager_init(void)
{
    char replay_path_buffer[512] = {0};
    
    replays_enabled = ConfigGetParamBool(g_CoreConfig, "Replays");
    
    if (replay_path != NULL)
    {
        free(replay_path);
        replay_path = NULL;
    }

    replay_manager_set_path(replay_manager_generate_path());
    
    // if (replays_enabled)
    // {
    //     if (ConfigGetParameter(g_CoreConfig, "RecordPath", M64TYPE_STRING, replay_path_buffer, sizeof(replay_path_buffer)) == M64ERR_SUCCESS)
    //     {
    //         if (replay_path_buffer[0] != '\0')
    //         {
    //             replay_path = strdup(replay_path_buffer);
    //         }
    //     }
    //     replay_file = replay_manager_open();
    // }
}

void replay_manager_open()
{
    char input_path[1024];
    snprintf(input_path, sizeof(input_path), "%s/inputs.bin", replay_path);

    FILE *file = fopen(input_path, "wb");
    if (file == NULL)
    {
        DebugMessage(M64MSG_ERROR, "Replay Manager: Failed to open replay file at path %s", input_path);
    }
    replay_file = file;
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
    
    // Filter out Start button (0x0010) to allow pausing without affecting replay
    uint32_t filtered_input = raw_input & ~0x0010u;
    
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
    
    if (fwrite(&filtered_input, sizeof(uint32_t), 1, file) != 1)
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

int replay_manager_set_path(char* path)
{
    if (replay_path != NULL)
    {
        free(replay_path);
        replay_path = NULL;
    }
    
    if (path != NULL)
    {
        replay_path = strdup(path);
        if (replay_path == NULL)
        {
            DebugMessage(M64MSG_ERROR, "Replay Manager: Failed to set replay path");
            return 0;
        }
    }
    
    return 1;
}

char* replay_manager_generate_path()
{
    char folder[64];
    time_t now = time(0);
    struct tm tmv;
    localtime_s(&tmv, &now);
    if (g_GameType == GAME_IS_REMIX)
    {
        strftime(folder, sizeof(folder), "./replays/remix/%Y-%m-%dT%H.%M.%S", &tmv);
    }
    else if (g_GameType == GAME_IS_VANILLA)
    {
        strftime(folder, sizeof(folder), "./replays/vanilla/%Y-%m-%dT%H.%M.%S", &tmv);
    }
    else
    {
        DebugMessage(M64MSG_ERROR, "Replay Manager: Unknown game type %d, cannot generate replay path", g_GameType);
        return NULL;
    }
    int dir_result = osal_mkdirp(folder, 0755);
    if (dir_result != 0)
    {
        DebugMessage(M64MSG_ERROR, "Replay Manager: Failed to create replay directory at path %s", folder);
        return NULL;
    }
    // replay_manager_set_path(folder);
    return replay_path;
}

FILE* replay_manager_get_file(void)
{
    return replay_file;
}
