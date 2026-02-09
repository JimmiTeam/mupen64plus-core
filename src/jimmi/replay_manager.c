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

#define REPLAY_BUFFER_SIZE 64
#define REPLAY_COMMIT_DELAY 5

struct replay_frame_buffer
{
    uint64_t frame;
    uint32_t inputs[4];
    uint8_t present_mask;
    uint8_t valid;
};

static struct replay_frame_buffer replay_buffer[REPLAY_BUFFER_SIZE];
static uint64_t last_flush_frame = 0;

static void replay_manager_reset_buffer(void)
{
    memset(replay_buffer, 0, sizeof(replay_buffer));
    last_flush_frame = 0;
}

static int replay_manager_write_record(FILE *file, int controller_index, uint64_t frame_index, uint32_t raw_input)
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

    if (frame_index - last_flush_frame >= 60)
    {
        fflush(file);
        last_flush_frame = frame_index;
    }

    return 1;
}

static void replay_manager_commit_slot(struct replay_frame_buffer *slot)
{
    if (slot == NULL || !slot->valid || replay_file == NULL)
        return;

    for (int i = 0; i < 4; ++i)
    {
        uint32_t value = (slot->present_mask & (1u << i)) ? slot->inputs[i] : 0;
        replay_manager_write_record(replay_file, i, slot->frame, value);
    }

    slot->valid = 0;
    slot->present_mask = 0;
}

void replay_manager_init(void)
{
    replays_enabled = ConfigGetParamBool(g_CoreConfig, "Replays");
    
    if (replay_path != NULL)
    {
        free(replay_path);
        replay_path = NULL;
    }
    
    if (replays_enabled)
    {
        char replay_path_buffer[512] = {0};
        if (ConfigGetParameter(g_CoreConfig, "ReplaysPath", M64TYPE_STRING, replay_path_buffer, sizeof(replay_path_buffer)) == M64ERR_SUCCESS)
        {
            if (replay_path_buffer[0] != '\0')
            {
                replay_path = strdup(replay_path_buffer);
            }
        }
    }
}

void replay_manager_open(char* folder)
{
    char input_path[1024];
    snprintf(input_path, sizeof(input_path), "%s/inputs.bin", replay_manager_generate_path(folder));

    if (replay_file != NULL)
    {
        fclose(replay_file);
        replay_file = NULL;
    }

    FILE *file = fopen(input_path, "wb");
    if (file == NULL)
    {
        DebugMessage(M64MSG_ERROR, "Replay Manager: Failed to open replay file at path %s", input_path);
    }
    replay_file = file;

    replay_manager_reset_buffer();
}

void replay_manager_close(void)
{
    if (replay_file == NULL)
    {
        return;
    }

    replay_manager_commit_frames(UINT64_MAX);

    fclose(replay_file);
    replay_file = NULL;
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

    int free_idx = -1;
    int match_idx = -1;
    uint64_t oldest_frame = UINT64_MAX;
    int oldest_idx = -1;

    for (int i = 0; i < REPLAY_BUFFER_SIZE; ++i)
    {
        if (replay_buffer[i].valid)
        {
            if (replay_buffer[i].frame == frame_index)
            {
                match_idx = i;
                break;
            }
            if (replay_buffer[i].frame < oldest_frame)
            {
                oldest_frame = replay_buffer[i].frame;
                oldest_idx = i;
            }
        }
        else if (free_idx == -1)
        {
            free_idx = i;
        }
    }

    int idx = (match_idx >= 0) ? match_idx : free_idx;
    if (idx < 0 && oldest_idx >= 0)
    {
        replay_manager_commit_slot(&replay_buffer[oldest_idx]);
        idx = oldest_idx;
    }

    if (idx < 0)
    {
        return 0;
    }

    if (!replay_buffer[idx].valid || replay_buffer[idx].frame != frame_index)
    {
        replay_buffer[idx].frame = frame_index;
        replay_buffer[idx].valid = 1;
        replay_buffer[idx].present_mask = 0;
        memset(replay_buffer[idx].inputs, 0, sizeof(replay_buffer[idx].inputs));
    }

    replay_buffer[idx].inputs[controller_index] = filtered_input;
    replay_buffer[idx].present_mask |= (uint8_t)(1u << controller_index);

    return 1;
}

void replay_manager_commit_frames(uint64_t current_frame)
{
    if (replay_file == NULL)
    {
        return;
    }

    while (1)
    {
        uint64_t oldest_frame = UINT64_MAX;
        int oldest_idx = -1;

        for (int i = 0; i < REPLAY_BUFFER_SIZE; ++i)
        {
            if (!replay_buffer[i].valid)
                continue;

            if (replay_buffer[i].frame + REPLAY_COMMIT_DELAY > current_frame)
                continue;

            if (replay_buffer[i].frame < oldest_frame)
            {
                oldest_frame = replay_buffer[i].frame;
                oldest_idx = i;
            }
        }

        if (oldest_idx < 0)
            break;

        replay_manager_commit_slot(&replay_buffer[oldest_idx]);
    }
}

int replay_manager_write_frame(FILE * file, uint64_t frame_index, const uint32_t raw_inputs[4])
{
    if (file == NULL || raw_inputs == NULL)
    {
        return 0;
    }

    for (int i = 0; i < 4; ++i)
    {
        if (!replay_manager_write_input(file, i, frame_index, raw_inputs[i]))
        {
            return 0;
        }
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

char* replay_manager_generate_path(char* folder)
{
    char replay_folder[1024];
    snprintf(replay_folder, sizeof(replay_folder), "%s%s", replay_path, folder);
    int dir_result = osal_mkdirp(replay_folder, 0755);
    if (dir_result != 0)
    {
        DebugMessage(M64MSG_ERROR, "Replay Manager: Failed to create replay directory at path %s", replay_folder);
        return NULL;
    }
    return strdup(replay_folder);
}

FILE* replay_manager_get_file(void)
{
    return replay_file;
}
