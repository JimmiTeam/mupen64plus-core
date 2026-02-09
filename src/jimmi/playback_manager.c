#include "playback_manager.h"
#include "input_manager.h"
#include "api/callbacks.h"
#include "main/main.h"
#include "api/config.h"
#include <string.h>


static int playback_enabled;
static char* playback_path = NULL;
static FILE* playback_file = NULL;

struct playback_frame
{
    uint64_t frame;
    uint32_t inputs[4];
    uint8_t present_mask;
};

static struct playback_frame* playback_frames = NULL;
static size_t playback_frame_count = 0;
static size_t playback_frame_cap = 0;
static size_t playback_last_index = 0;

static void playback_manager_clear_index(void)
{
    free(playback_frames);
    playback_frames = NULL;
    playback_frame_count = 0;
    playback_frame_cap = 0;
    playback_last_index = 0;
}

static int playback_manager_ensure_capacity(size_t needed)
{
    if (needed <= playback_frame_cap)
        return 1;

    size_t new_cap = playback_frame_cap == 0 ? 1024 : playback_frame_cap * 2;
    while (new_cap < needed)
        new_cap *= 2;

    struct playback_frame* new_buf = realloc(playback_frames, new_cap * sizeof(*new_buf));
    if (new_buf == NULL)
        return 0;

    playback_frames = new_buf;
    playback_frame_cap = new_cap;
    return 1;
}


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
            DebugMessage(M64MSG_INFO, "Playback Manager: Playing from playback path: %s", playback_path_buffer);
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
    snprintf(full_playback_path, sizeof(full_playback_path), "%s/inputs.bin", playback_path);
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

    playback_manager_clear_index();
}


int playback_manager_read_input(PlaybackInputRecord* out_record)
{
    if (playback_file == NULL || out_record == NULL)
    {
        return 0;
    }
    
    // Read record format: controller_index (4) | frame_index (8) | raw_input (4) = 16 bytes
    if (fread(&out_record->controller_index, sizeof(int), 1, playback_file) != 1)
    {
        // EOF or read error
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

static int playback_manager_build_index(FILE *file)
{
    PlaybackInputRecord record;

    playback_manager_clear_index();

    while (1)
    {
        if (fread(&record.controller_index, sizeof(int), 1, file) != 1)
            break;
        if (fread(&record.frame_index, sizeof(uint64_t), 1, file) != 1)
            break;
        if (fread(&record.raw_input, sizeof(uint32_t), 1, file) != 1)
            break;

        if (record.controller_index < 0 || record.controller_index >= 4)
        {
            DebugMessage(M64MSG_WARNING, "Playback Manager: Invalid controller_index %d in playback file", record.controller_index);
            continue;
        }

        if (playback_frame_count == 0 || record.frame_index > playback_frames[playback_frame_count - 1].frame)
        {
            if (!playback_manager_ensure_capacity(playback_frame_count + 1))
            {
                DebugMessage(M64MSG_ERROR, "Playback Manager: Failed to allocate frame index");
                return 0;
            }

            struct playback_frame *slot = &playback_frames[playback_frame_count++];
            slot->frame = record.frame_index;
            memset(slot->inputs, 0, sizeof(slot->inputs));
            slot->present_mask = 0;
        }
        else if (record.frame_index < playback_frames[playback_frame_count - 1].frame)
        {
            DebugMessage(M64MSG_WARNING, "Playback Manager: Non-monotonic frame %llu in playback file", (unsigned long long)record.frame_index);
            continue;
        }

        struct playback_frame *slot = &playback_frames[playback_frame_count - 1];
        if (slot->frame != record.frame_index)
            continue;

        slot->inputs[record.controller_index] = record.raw_input;
        slot->present_mask |= (uint8_t)(1u << record.controller_index);
    }

    playback_last_index = 0;
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

int playback_manager_read_frame(uint64_t f)
{
    if (!playback_enabled)
        return 0;

    if (playback_frames == NULL && playback_file != NULL)
    {
        if (!playback_manager_build_index(playback_file))
            return 0;
        fclose(playback_file);
        playback_file = NULL;
    }

    if (playback_frames == NULL || playback_frame_count == 0)
        return 0;

    size_t left = 0;
    size_t right = playback_frame_count;
    size_t idx = playback_last_index;

    if (idx < playback_frame_count && playback_frames[idx].frame == f)
    {
        // fast path
    }
    else
    {
        while (left < right)
        {
            size_t mid = left + (right - left) / 2;
            uint64_t mid_frame = playback_frames[mid].frame;
            if (mid_frame < f)
                left = mid + 1;
            else
                right = mid;
        }

        idx = left;
        if (idx >= playback_frame_count || playback_frames[idx].frame != f)
            return 0;
    }

    playback_last_index = idx;
    const struct playback_frame *slot = &playback_frames[idx];
    int record_count = 0;

    for (int i = 0; i < 4; ++i)
    {
        uint32_t value = (slot->present_mask & (1u << i)) ? slot->inputs[i] : 0;
        uint32_t filtered_input = value & ~0x0010u;
        input_manager_record_raw(i, f, filtered_input, 1);
        if (slot->present_mask & (1u << i))
            record_count++;
    }

    if ((f % 60) == 0 && record_count > 0)
    {
        DebugMessage(M64MSG_INFO, "Playback Manager: Replayed frame %llu with %d port inputs", f, record_count);
    }

    return record_count;
}
