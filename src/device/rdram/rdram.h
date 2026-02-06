/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - rdram.h                                                 *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2014 Bobby Smiles                                       *
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

#ifndef M64P_DEVICE_RCP_RI_RDRAM_H
#define M64P_DEVICE_RCP_RI_RDRAM_H

#include <stddef.h>
#include <stdint.h>

#include "osal/preproc.h"

struct r4300_core;

enum rdram_registers
{
    RDRAM_CONFIG_REG,
    RDRAM_DEVICE_ID_REG,
    RDRAM_DELAY_REG,
    RDRAM_MODE_REG,
    RDRAM_REF_INTERVAL_REG,
    RDRAM_REF_ROW_REG,
    RDRAM_RAS_INTERVAL_REG,
    RDRAM_MIN_INTERVAL_REG,
    RDRAM_ADDR_SELECT_REG,
    RDRAM_DEVICE_MANUF_REG,
    RDRAM_REGS_COUNT
};

/* IPL3 rdram initialization accepts up to 8 RDRAM modules */
enum { RDRAM_MAX_MODULES_COUNT = 8 };

struct rdram
{
    uint32_t regs[RDRAM_MAX_MODULES_COUNT][RDRAM_REGS_COUNT];

    uint32_t* dram;
    size_t dram_size;

    uint8_t corrupted_handler;

    struct r4300_core* r4300;
};

static osal_inline uint32_t rdram_reg(uint32_t address)
{
    return (address & 0x3ff) >> 2;
}

static osal_inline uint32_t rdram_dram_address(uint32_t address)
{
    return (address & 0xffffff) >> 2;
}

void init_rdram(struct rdram* rdram,
                uint32_t* dram,
                size_t dram_size,
                struct r4300_core* r4300);

void poweron_rdram(struct rdram* rdram);

void read_rdram_regs(void* opaque, uint32_t address, uint32_t* value);
void write_rdram_regs(void* opaque, uint32_t address, uint32_t value, uint32_t mask);

void read_rdram_dram(void* opaque, uint32_t address, uint32_t* value);
void write_rdram_dram(void* opaque, uint32_t address, uint32_t value, uint32_t mask);

/* Jimmi */

static osal_inline uint32_t viraddr_to_physaddr(uint32_t viraddr)
{
    return viraddr & 0x1FFFFFFF;
}

static osal_inline const uint8_t* rdram_bytes(const struct rdram* r)
{
    return (const uint8_t*)r->dram;
}

static osal_inline size_t rdram_bytes_size(const struct rdram* r)
{
    return r->dram_size;
}

static osal_inline uint16_t read_u16_be(const uint8_t* p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static osal_inline uint32_t read_u32_be(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |
           ((uint32_t)p[3] <<  0);
}

static osal_inline uint32_t physaddr_to_kseg0(uint32_t p)
{
    return 0x80000000u | (p & 0x1FFFFFFFu);
}

/* Result of locating a menu item symbol in RDRAM */
struct rdram_symbol_result
{
    uint32_t struct_vaddr;       /* KSEG0 address of the menu item struct */
    uint32_t string_table_vaddr; /* KSEG0 address of the string table */
    uint32_t value_array_vaddr;  /* KSEG0 address of the per-port value array */
    uint16_t value_type;         /* type field from the menu item */
};

/* Maximum length of a player tag name string (including NUL terminator).
 * The string table is an array of pointers; each points to a buffer of
 * at least this many bytes (e.g. Toggles.entry_player_tags_N + 0x28). */
enum { PLAYER_TAG_MAX_LEN = 16 };

/* Locate a menu item struct in RDRAM whose label matches |symbol|.
 * Returns 1 and fills |out| on success, 0 on failure. */
int rdram_locate_symbol(const struct rdram* r, const uint8_t* symbol,
                        struct rdram_symbol_result* out);

#endif
