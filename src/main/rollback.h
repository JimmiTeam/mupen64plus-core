
#ifndef __ROLLBACK_H__
#define __ROLLBACK_H__

#include <stddef.h>
#include <stdint.h>

struct device;

#define ROLLBACK_RING_SIZE 5

#define ROLLBACK_STATE_SIZE (16788288 + 1024 + 4 + 4096)

struct rollback_slot {
    unsigned char *data;
    unsigned int frame;
    int valid;
};

struct rollback_ringbuf {
    struct rollback_slot slots[ROLLBACK_RING_SIZE];
    unsigned int head;
    int count;
};

extern struct rollback_ringbuf g_rollback;

int  rollback_init(void);

void rollback_deinit(void);

void rollback_save(const struct device *dev, unsigned int frame);

int  rollback_load(struct device *dev, unsigned int frames_back);

int  rollback_count(void);

#endif /* __ROLLBACK_H__ */
