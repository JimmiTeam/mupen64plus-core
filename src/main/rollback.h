// Adapted from:
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - savestates.h                                            *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2012 CasualJames                                        *
 *   Copyright (C) 2009 Olejl Tillin9                                      *
 *   Copyright (C) 2008 Richard42 Tillin9                                  *
 *   Copyright (C) 2002 Hacktarux                                          *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

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
