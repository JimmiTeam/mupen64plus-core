#include "playback_manager.h"
#include "api/callbacks.h"
#include "main/main.h"
#include "api/config.h"
#include <string.h>


static int playback_enabled;
static char* playback_path = NULL;
static FILE* playback_file = NULL;


void playback_manager_init(void)
{
    char playback_path_buffer[512] = {0};
    
    playback_enabled = ConfigGetParamBool(g_CoreConfig, "Playback");
    
    if (playback_path != NULL)
    {
        free(playback_path);
        playback_path = NULL;
    }
    
    if (playback_enabled)
    {
        if (ConfigGetParameter(g_CoreConfig, "PlaybackPath", M64TYPE_STRING, playback_path_buffer, sizeof(playback_path_buffer)) == M64ERR_SUCCESS)
        {
            DebugMessage(M64MSG_INFO, "Playing from playback path: %s", playback_path_buffer);
            if (playback_path_buffer[0] != '\0')
            {
                playback_path = strdup(playback_path_buffer);
            }
        }
        playback_file = playback_manager_open();
        if (playback_file != NULL)
        {
            DebugMessage(M64MSG_INFO, "Playback Manager: Reading inputs from %s", playback_path);
        }
    }
}


FILE* playback_manager_open(void)
{
    if (!playback_enabled || playback_path == NULL)
    {
        return NULL;
    }

    char full_playback_path[1024];
    snprintf(full_playback_path, sizeof(full_playback_path), "%s\\inputs.bin", playback_path);
    FILE *file = fopen(full_playback_path, "rb");
    if (file == NULL)
    {
        DebugMessage(M64MSG_ERROR, "Playback Manager: Failed to open playback file at path %s", playback_path);
    }
    return file;
}


void playback_manager_close(void)
{
    if (playback_file != NULL)
    {
        fclose(playback_file);
        playback_file = NULL;
    }
}


int playback_manager_read_input(PlaybackInputRecord* out_record)
{
    if (playback_file == NULL || out_record == NULL)
    {
        return 0;
    }
    
    // Read record format: controller_index (4) | frame_index (8) | raw_input (4) = 16 bytes
    if (fread(&out_record->controller_index, sizeof(uint32_t), 1, playback_file) != 1)
    {
        // EOF or read error - normal condition when reaching end of file
        return 0;
    }
    
    if (fread(&out_record->frame_index, sizeof(uint64_t), 1, playback_file) != 1)
    {
        DebugMessage(M64MSG_ERROR, "Playback Manager: Failed to read frame_index");
        return 0;
    }
    
    if (fread(&out_record->raw_input, sizeof(uint32_t), 1, playback_file) != 1)
    {
        DebugMessage(M64MSG_ERROR, "Playback Manager: Failed to read raw_input");
        return 0;
    }
    
    // Validate controller_index
    if (out_record->controller_index < 0 || out_record->controller_index >= 4)
    {
        DebugMessage(M64MSG_WARNING, "Playback Manager: Invalid controller_index %d in playback file", out_record->controller_index);
        return 0;
    }
    
    return 1;
}


int playback_manager_is_enabled(void)
{
    return playback_enabled;
}


char* playback_manager_get_path(void)
{
    return playback_path;
}


FILE* playback_manager_get_file(void)
{
    return playback_file;
}
