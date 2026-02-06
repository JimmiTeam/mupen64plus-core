/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - rdram.c                                                 *
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

#include "rdram.h"

#include "api/m64p_types.h"
#include "api/callbacks.h"
#include "device/device.h"
#include "device/memory/memory.h"
#include "device/r4300/r4300_core.h"
#include "device/rcp/ri/ri_controller.h"

#include <string.h>

#define RDRAM_BCAST_ADDRESS_MASK UINT32_C(0x00080000)
#define RDRAM_MODE_CE_MASK UINT32_C(0x80000000)

/* Jimmi constants for player tags */
#define MENU_ITEM_SYMBOL_PTR_OFFSET    0x00
#define MENU_ITEM_VALUE_TYPE_OFFSET    0x04
#define MENU_ITEM_STRING_TABLE_OFFSET  0x14
#define MENU_ITEM_VALUE_ARRAY_OFFSET   0x1C
#define MENU_ITEM_MIN_SIZE             0x20

/* XXX: deduce # of RDRAM modules from its total size
 * Assume only 2Mo RDRAM modules.
 * Proper way of doing it would be to declare in init_rdram
 * what kind of modules we insert and deduce dram_size from
 * that configuration.
 */
static size_t get_modules_count(const struct rdram* rdram)
{
    return (rdram->dram_size) / 0x200000;
}

static uint8_t cc_value(uint32_t mode_reg)
{
    return ((mode_reg & 0x00000040) >>  6)
        |  ((mode_reg & 0x00004000) >> 13)
        |  ((mode_reg & 0x00400000) >> 20)
        |  ((mode_reg & 0x00000080) >>  4)
        |  ((mode_reg & 0x00008000) >> 11)
        |  ((mode_reg & 0x00800000) >> 18);
}


static osal_inline uint16_t idfield_value(uint32_t device_id)
{
    return ((((device_id >> 26) & 0x3f) <<  0)
          | (((device_id >> 23) & 0x01) <<  6)
          | (((device_id >>  8) & 0xff) <<  7)
          | (((device_id >>  7) & 0x01) << 15));
}

static osal_inline uint8_t swapfield_value(uint32_t address_select)
{
    return ((((address_select >> 25) & 0x7f) << 0)
          | (((address_select >> 15) & 0x01) << 7));
}

static size_t get_module(const struct rdram* rdram, uint32_t address)
{
    size_t module;
    size_t modules = get_modules_count(rdram);
    uint16_t id_field;

    for (module = 0; module < modules; ++module) {
        id_field = ri_address_to_id_field(ri_address(address), swapfield_value(rdram->regs[module][RDRAM_ADDR_SELECT_REG]));
        if (id_field == idfield_value(rdram->regs[module][RDRAM_DEVICE_ID_REG])) {
            return module;
        }
    }

    /* can happen during memory detection because
     * it probes potentialy non present RDRAM */
    return RDRAM_MAX_MODULES_COUNT;
}

static void read_rdram_dram_corrupted(void* opaque, uint32_t address, uint32_t* value)
{
    struct rdram* rdram = (struct rdram*)opaque;
    uint32_t addr = rdram_dram_address(address);
    size_t module;

    module = get_module(rdram, address);
    if (module == RDRAM_MAX_MODULES_COUNT) {
        *value = 0;
        return;
    }

    /* corrupt read value if CC value is not calibrated */
    uint32_t mode = rdram->regs[module][RDRAM_MODE_REG] ^ UINT32_C(0xc0c0c0c0);
    if ((mode & RDRAM_MODE_CE_MASK) && (cc_value(mode) == 0)) {
        *value = 0;
        return;
    }

    if (address < rdram->dram_size) {
        *value = rdram->dram[addr];
    } else {
        *value = 0;
    }
}

static void map_corrupt_rdram(struct rdram* rdram, int corrupt)
{
    struct mem_mapping mapping;

    mapping.begin = MM_RDRAM_DRAM;
    mapping.end = MM_RDRAM_DRAM + 0x3efffff;
    mapping.type = M64P_MEM_RDRAM;
    mapping.handler.opaque = rdram;
    mapping.handler.read32 = (corrupt)
        ? read_rdram_dram_corrupted
        : read_rdram_dram;
    mapping.handler.write32 = write_rdram_dram;

    apply_mem_mapping(rdram->r4300->mem, &mapping);
#ifndef NEW_DYNAREC
    rdram->r4300->recomp.fast_memory = (corrupt) ? 0 : 1;
    invalidate_r4300_cached_code(rdram->r4300, 0, 0);
#endif
}


void init_rdram(struct rdram* rdram,
                uint32_t* dram,
                size_t dram_size,
                struct r4300_core* r4300)
{
    rdram->dram = dram;
    rdram->dram_size = dram_size;
    rdram->r4300 = r4300;
    rdram->corrupted_handler = 0;
}

void poweron_rdram(struct rdram* rdram)
{
    size_t module;
    size_t modules = get_modules_count(rdram);
    memset(rdram->regs, 0, RDRAM_MAX_MODULES_COUNT*RDRAM_REGS_COUNT*sizeof(uint32_t));
    memset(rdram->dram, 0, rdram->dram_size);

    DebugMessage(M64MSG_INFO, "Initializing %u RDRAM modules for a total of %u MB",
        (uint32_t) modules, (uint32_t) rdram->dram_size / (1024*1024));

    for (module = 0; module < modules; ++module) {
        rdram->regs[module][RDRAM_CONFIG_REG] = UINT32_C(0xb5190010);
        rdram->regs[module][RDRAM_DEVICE_ID_REG] = UINT32_C(0x00000000);
        rdram->regs[module][RDRAM_DELAY_REG] = UINT32_C(0x230b0223);
        rdram->regs[module][RDRAM_MODE_REG] = UINT32_C(0xc4c0c0c0);
        rdram->regs[module][RDRAM_REF_ROW_REG] = UINT32_C(0x00000000);
        rdram->regs[module][RDRAM_MIN_INTERVAL_REG] = UINT32_C(0x0040c0e0);
        rdram->regs[module][RDRAM_ADDR_SELECT_REG] = UINT32_C(0x00000000);
        rdram->regs[module][RDRAM_DEVICE_MANUF_REG] = UINT32_C(0x00000500);
    }
}


void read_rdram_regs(void* opaque, uint32_t address, uint32_t* value)
{
    struct rdram* rdram = (struct rdram*)opaque;
    uint32_t reg = rdram_reg(address);
    size_t module;

    if (address & RDRAM_BCAST_ADDRESS_MASK) {
        DebugMessage(M64MSG_WARNING, "Reading from broadcast address is unsupported %08x", address);
        return;
    }

    module = get_module(rdram, address);
    if (module == RDRAM_MAX_MODULES_COUNT) {
        *value = 0;
        return;
    }

    if (reg < RDRAM_REGS_COUNT) {
        *value = rdram->regs[module][reg];
    } else {
        *value = 0;
    }

    /* some bits are inverted when read */
    if (reg == RDRAM_MODE_REG) {
        *value ^= UINT32_C(0xc0c0c0c0);
    }
}

void write_rdram_regs(void* opaque, uint32_t address, uint32_t value, uint32_t mask)
{
    struct rdram* rdram = (struct rdram*)opaque;
    uint32_t reg = rdram_reg(address);
    uint32_t mode;
    uint8_t corrupted_handler = 0;
    size_t module;
    size_t modules = get_modules_count(rdram);

    if (reg >= RDRAM_REGS_COUNT) {
        return;
    }

    if (address & RDRAM_BCAST_ADDRESS_MASK) {
        for (module = 0; module < modules; ++module) {
            masked_write(&rdram->regs[module][reg], value, mask);
        }
    }
    else {
        module = get_module(rdram, address);
        if (module != RDRAM_MAX_MODULES_COUNT) {
            masked_write(&rdram->regs[module][reg], value, mask);
        }
    }

    /* toggle corrupt handler based on CC value for all modules,
     * only check values when writing to the mode register */
    if (reg == RDRAM_MODE_REG) {
        for (module = 0; module < modules; ++module) {
            mode = rdram->regs[module][RDRAM_MODE_REG] ^ UINT32_C(0xc0c0c0c0);
            corrupted_handler |= ((mode & RDRAM_MODE_CE_MASK) && (cc_value(mode) == 0));
        }
        if (rdram->corrupted_handler != corrupted_handler) {
            map_corrupt_rdram(rdram, corrupted_handler);
            rdram->corrupted_handler = corrupted_handler;
        }
    }
}


void read_rdram_dram(void* opaque, uint32_t address, uint32_t* value)
{
    struct rdram* rdram = (struct rdram*)opaque;
    uint32_t addr = rdram_dram_address(address);

    if (address < rdram->dram_size)
    {
        *value = rdram->dram[addr];
    }
    else
    {
        *value = 0;
    }
}

void write_rdram_dram(void* opaque, uint32_t address, uint32_t value, uint32_t mask)
{
    struct rdram* rdram = (struct rdram*)opaque;
    uint32_t addr = rdram_dram_address(address);

    if (address < rdram->dram_size)
    {
        masked_write(&rdram->dram[addr], value, mask);
    }
}

/* Jimmi */
static int find_bytes(const uint8_t* mem, size_t mem_len,
                      const uint8_t* target,   size_t target_len,
                      size_t* out_offset)
{
    if (!mem || !target || target_len == 0 || mem_len < target_len)
        return 0;

    for (size_t i = 0; i + target_len <= mem_len; i++)
    {
        if (mem[i] == target[0] &&
            memcmp(mem + i, target, target_len) == 0)
        {
            if (out_offset)
                *out_offset = i;
            return 1;
        }
    }
    return 0;
}

static int find_u32_be_all(const uint8_t* mem, size_t mem_len,
                           uint32_t target,
                           size_t* out_offsets, size_t out_cap,
                           size_t* out_count)
{
    size_t count = 0;
    if (!mem || mem_len < 4)
        return 0;

    for (size_t off = 0; off + 4 <= mem_len; off += 4)
    {
        if (read_u32_be(mem + off) == target)
        {
            if (count < out_cap)
                out_offsets[count] = off;
            count++;
        }
    }

    if (out_count)
        *out_count = count;
    return count != 0;
}

int rdram_locate_symbol(const struct rdram* r, const uint8_t* symbol,
                        struct rdram_symbol_result* out)
{
    if (!r || !symbol || !out)
    {
        DebugMessage(M64MSG_WARNING, "rdram_locate_symbol: Invalid parameters");
        return 0;
    }

    memset(out, 0, sizeof(*out));

    size_t symbol_paddr = 0;
    const uint8_t* rdram_data = rdram_bytes(r);
    size_t rdram_size = rdram_bytes_size(r);

    size_t symbol_len = 0;
    while (symbol[symbol_len] != '\0' && symbol_len < 256)
        symbol_len++;

    if (symbol_len == 0 || symbol_len >= 256)
    {
        DebugMessage(M64MSG_WARNING, "Invalid symbol length: %zu", symbol_len);
        return 0;
    }

    if (!find_bytes(rdram_data, rdram_size, symbol, symbol_len, &symbol_paddr))
    {
        DebugMessage(M64MSG_WARNING, "Symbol not found in RDRAM: %.32s", symbol);
        return 0;
    }

    DebugMessage(M64MSG_STATUS, "Found symbol at physical offset 0x%X", (uint32_t)symbol_paddr);

    uint32_t symbol_addrs[3] = {
        0x80000000u | (uint32_t)symbol_paddr,
        0xA0000000u | (uint32_t)symbol_paddr,
        (uint32_t)symbol_paddr
    };

    size_t hits[64];
    size_t hit_count = 0;

    for (int addr_idx = 0; addr_idx < 3; addr_idx++)
    {
        hit_count = 0;
        if (find_u32_be_all(rdram_data, rdram_size, symbol_addrs[addr_idx],
                           hits, 64, &hit_count) && hit_count > 0)
        {
            DebugMessage(M64MSG_INFO, "Found %zu references to symbol (format: 0x%08X)",
                        hit_count, symbol_addrs[addr_idx]);
            break;
        }
    }

    if (hit_count == 0)
    {
        DebugMessage(M64MSG_WARNING, "No references to symbol found in RDRAM");
        return 0;
    }

    for (size_t i = 0; i < hit_count; i++)
    {
        size_t struct_offset = hits[i];

        if (struct_offset + MENU_ITEM_MIN_SIZE > rdram_size)
        {
            DebugMessage(M64MSG_WARNING, "Potential structure at 0x%X exceeds RDRAM bounds",
                        (uint32_t)struct_offset);
            continue;
        }

        uint32_t symbol_ptr = read_u32_be(rdram_data + struct_offset + MENU_ITEM_SYMBOL_PTR_OFFSET);
        uint16_t value_type = read_u16_be(rdram_data + struct_offset + MENU_ITEM_VALUE_TYPE_OFFSET);
        uint32_t string_table_ptr = read_u32_be(rdram_data + struct_offset + MENU_ITEM_STRING_TABLE_OFFSET);
        uint32_t value_array_ptr = read_u32_be(rdram_data + struct_offset + MENU_ITEM_VALUE_ARRAY_OFFSET);

        uint32_t seg = symbol_ptr >> 29;
        if (seg != 4 && seg != 5) /* 4 = KSEG0 (0x8xxx), 5 = KSEG1 (0xAxxx) */
        {
            DebugMessage(M64MSG_STATUS, "Skipping hit at 0x%X (pointer 0x%08X not in KSEG0/KSEG1)",
                        (uint32_t)struct_offset, symbol_ptr);
            continue;
        }

        /* Validate string_table_ptr and value_array_ptr are in KSEG0/KSEG1 */
        uint32_t st_seg = string_table_ptr >> 29;
        uint32_t va_seg = value_array_ptr >> 29;
        if ((st_seg != 4 && st_seg != 5) || (va_seg != 4 && va_seg != 5))
        {
            DebugMessage(M64MSG_STATUS, "Skipping hit at 0x%X (strtab/varr not in KSEG0/KSEG1)",
                        (uint32_t)struct_offset);
            continue;
        }

        DebugMessage(M64MSG_INFO,
            "Menu item found at 0x%08X | symbol=0x%08X | type=0x%04X | strtab=0x%08X | varr=0x%08X",
            physaddr_to_kseg0((uint32_t)struct_offset), symbol_ptr, value_type,
            string_table_ptr, value_array_ptr);

        /* Return the first valid hit */
        out->struct_vaddr    = physaddr_to_kseg0((uint32_t)struct_offset);
        out->string_table_vaddr = string_table_ptr;
        out->value_array_vaddr  = value_array_ptr;
        out->value_type      = value_type;
        return 1;
    }

    DebugMessage(M64MSG_WARNING, "No valid menu item structure found for symbol");
    return 0;
}
