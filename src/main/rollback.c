#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define M64P_CORE_PROTOTYPES 1
#include "api/callbacks.h"
#include "api/m64p_types.h"
#include "backends/api/storage_backend.h"
#include "device/cart/flashram.h"
#include "device/controllers/paks/rumblepak.h"
#include "device/device.h"
#include "device/pif/pif.h"
#include "device/r4300/cp1.h"
#include "device/r4300/r4300_core.h"
#include "device/r4300/interrupt.h"
#include "device/rcp/rdp/fb.h"
#include "main/main.h"
#include "main/rom.h"
#include "plugin/plugin.h"
#include "rollback.h"
#include "util.h"

/* Buffer management macros for serialization */

#define GETARRAY(buff, type, count) \
    (to_little_endian_buffer(buff, sizeof(type), count), \
     buff += (count) * sizeof(type), \
     (type *)(buff - (count) * sizeof(type)))
#define COPYARRAY(dst, buff, type, count) \
    memcpy(dst, GETARRAY(buff, type, count), sizeof(type) * (count))
#define GETDATA(buff, type) *GETARRAY(buff, type, 1)
#define PUTARRAY(src, buff, type, count) \
    memcpy(buff, src, sizeof(type) * (count)); \
    to_little_endian_buffer(buff, sizeof(type), count); \
    buff += (count) * sizeof(type);
#define PUTDATA(buff, type, value) \
    do { type x = value; PUTARRAY(&x, buff, type, 1); } while (0)

struct rollback_ringbuf g_rollback;

int rollback_init(void)
{
    unsigned int i;
    memset(&g_rollback, 0, sizeof(g_rollback));

    for (i = 0; i < ROLLBACK_RING_SIZE; ++i)
    {
        g_rollback.slots[i].data = (unsigned char *)malloc(ROLLBACK_STATE_SIZE);
        if (g_rollback.slots[i].data == NULL)
        {
            DebugMessage(M64MSG_ERROR,
                         "Rollback: failed to allocate slot %u (%u bytes)",
                         i, (unsigned)ROLLBACK_STATE_SIZE);
            rollback_deinit();
            return -1;
        }
        g_rollback.slots[i].valid = 0;
        g_rollback.slots[i].frame = 0;
    }

    g_rollback.head  = 0;
    g_rollback.count = 0;

    DebugMessage(M64MSG_INFO,
                 "Rollback: initialised ring buffer (%u slots, ~%u MB total)",
                 ROLLBACK_RING_SIZE,
                 (unsigned)((ROLLBACK_RING_SIZE * (size_t)ROLLBACK_STATE_SIZE) / (1024 * 1024)));
    return 0;
}

void rollback_deinit(void)
{
    unsigned int i;
    for (i = 0; i < ROLLBACK_RING_SIZE; ++i)
    {
        free(g_rollback.slots[i].data);
        g_rollback.slots[i].data  = NULL;
        g_rollback.slots[i].valid = 0;
    }
    g_rollback.head  = 0;
    g_rollback.count = 0;
}

void rollback_save(const struct device *dev, unsigned int frame)
{
    struct rollback_slot *slot;
    char *curr;
    int i;
    unsigned char outbuf[4];
    char queue[1024];

    const uint32_t *cp0_regs = r4300_cp0_regs((struct cp0 *)&dev->r4300.cp0);

    slot = &g_rollback.slots[g_rollback.head];
    if (slot->data == NULL)
        return;

    save_eventqueue_infos(&dev->r4300.cp0, queue);

    curr = (char *)slot->data;
    memset(curr, 0, ROLLBACK_STATE_SIZE);

    {
        static const char magic[8] = "M64+SAVE";
        static const int version   = 0x00010900;

        PUTARRAY(magic, curr, unsigned char, 8);
        outbuf[0] = (version >> 24) & 0xff;
        outbuf[1] = (version >> 16) & 0xff;
        outbuf[2] = (version >>  8) & 0xff;
        outbuf[3] = (version >>  0) & 0xff;
        PUTARRAY(outbuf, curr, unsigned char, 4);
        PUTARRAY(ROM_SETTINGS.MD5, curr, char, 32);
    }

    PUTDATA(curr, uint32_t, dev->rdram.regs[0][RDRAM_CONFIG_REG]);
    PUTDATA(curr, uint32_t, dev->rdram.regs[0][RDRAM_DEVICE_ID_REG]);
    PUTDATA(curr, uint32_t, dev->rdram.regs[0][RDRAM_DELAY_REG]);
    PUTDATA(curr, uint32_t, dev->rdram.regs[0][RDRAM_MODE_REG]);
    PUTDATA(curr, uint32_t, dev->rdram.regs[0][RDRAM_REF_INTERVAL_REG]);
    PUTDATA(curr, uint32_t, dev->rdram.regs[0][RDRAM_REF_ROW_REG]);
    PUTDATA(curr, uint32_t, dev->rdram.regs[0][RDRAM_RAS_INTERVAL_REG]);
    PUTDATA(curr, uint32_t, dev->rdram.regs[0][RDRAM_MIN_INTERVAL_REG]);
    PUTDATA(curr, uint32_t, dev->rdram.regs[0][RDRAM_ADDR_SELECT_REG]);
    PUTDATA(curr, uint32_t, dev->rdram.regs[0][RDRAM_DEVICE_MANUF_REG]);

    /* MI */
    PUTDATA(curr, uint32_t, 0);
    PUTDATA(curr, uint32_t, dev->mi.regs[MI_INIT_MODE_REG]);
    PUTDATA(curr, uint8_t,  dev->mi.regs[MI_INIT_MODE_REG] & 0x7F);
    PUTDATA(curr, uint8_t, (dev->mi.regs[MI_INIT_MODE_REG] & 0x80) != 0);
    PUTDATA(curr, uint8_t, (dev->mi.regs[MI_INIT_MODE_REG] & 0x100) != 0);
    PUTDATA(curr, uint8_t, (dev->mi.regs[MI_INIT_MODE_REG] & 0x200) != 0);
    PUTDATA(curr, uint32_t, dev->mi.regs[MI_VERSION_REG]);
    PUTDATA(curr, uint32_t, dev->mi.regs[MI_INTR_REG]);
    PUTDATA(curr, uint32_t, dev->mi.regs[MI_INTR_MASK_REG]);
    PUTDATA(curr, uint32_t, 0); /* Padding */
    PUTDATA(curr, uint8_t, (dev->mi.regs[MI_INTR_MASK_REG] & 0x1) != 0);
    PUTDATA(curr, uint8_t, (dev->mi.regs[MI_INTR_MASK_REG] & 0x2) != 0);
    PUTDATA(curr, uint8_t, (dev->mi.regs[MI_INTR_MASK_REG] & 0x4) != 0);
    PUTDATA(curr, uint8_t, (dev->mi.regs[MI_INTR_MASK_REG] & 0x8) != 0);
    PUTDATA(curr, uint8_t, (dev->mi.regs[MI_INTR_MASK_REG] & 0x10) != 0);
    PUTDATA(curr, uint8_t, (dev->mi.regs[MI_INTR_MASK_REG] & 0x20) != 0);
    PUTDATA(curr, uint16_t, 0); /* Padding */

    /* PI */
    PUTDATA(curr, uint32_t, dev->pi.regs[PI_DRAM_ADDR_REG]);
    PUTDATA(curr, uint32_t, dev->pi.regs[PI_CART_ADDR_REG]);
    PUTDATA(curr, uint32_t, dev->pi.regs[PI_RD_LEN_REG]);
    PUTDATA(curr, uint32_t, dev->pi.regs[PI_WR_LEN_REG]);
    PUTDATA(curr, uint32_t, dev->pi.regs[PI_STATUS_REG]);
    PUTDATA(curr, uint32_t, dev->pi.regs[PI_BSD_DOM1_LAT_REG]);
    PUTDATA(curr, uint32_t, dev->pi.regs[PI_BSD_DOM1_PWD_REG]);
    PUTDATA(curr, uint32_t, dev->pi.regs[PI_BSD_DOM1_PGS_REG]);
    PUTDATA(curr, uint32_t, dev->pi.regs[PI_BSD_DOM1_RLS_REG]);
    PUTDATA(curr, uint32_t, dev->pi.regs[PI_BSD_DOM2_LAT_REG]);
    PUTDATA(curr, uint32_t, dev->pi.regs[PI_BSD_DOM2_PWD_REG]);
    PUTDATA(curr, uint32_t, dev->pi.regs[PI_BSD_DOM2_PGS_REG]);
    PUTDATA(curr, uint32_t, dev->pi.regs[PI_BSD_DOM2_RLS_REG]);

    /* SP */
    PUTDATA(curr, uint32_t, dev->sp.regs[SP_MEM_ADDR_REG]);
    PUTDATA(curr, uint32_t, dev->sp.regs[SP_DRAM_ADDR_REG]);
    PUTDATA(curr, uint32_t, dev->sp.regs[SP_RD_LEN_REG]);
    PUTDATA(curr, uint32_t, dev->sp.regs[SP_WR_LEN_REG]);
    PUTDATA(curr, uint32_t, 0); /* Padding */
    PUTDATA(curr, uint32_t, dev->sp.regs[SP_STATUS_REG]);
    PUTDATA(curr, uint8_t, (dev->sp.regs[SP_STATUS_REG] & 0x1) != 0);
    PUTDATA(curr, uint8_t, (dev->sp.regs[SP_STATUS_REG] & 0x2) != 0);
    PUTDATA(curr, uint8_t, (dev->sp.regs[SP_STATUS_REG] & 0x4) != 0);
    PUTDATA(curr, uint8_t, (dev->sp.regs[SP_STATUS_REG] & 0x8) != 0);
    PUTDATA(curr, uint8_t, (dev->sp.regs[SP_STATUS_REG] & 0x10) != 0);
    PUTDATA(curr, uint8_t, (dev->sp.regs[SP_STATUS_REG] & 0x20) != 0);
    PUTDATA(curr, uint8_t, (dev->sp.regs[SP_STATUS_REG] & 0x40) != 0);
    PUTDATA(curr, uint8_t, (dev->sp.regs[SP_STATUS_REG] & 0x80) != 0);
    PUTDATA(curr, uint8_t, (dev->sp.regs[SP_STATUS_REG] & 0x100) != 0);
    PUTDATA(curr, uint8_t, (dev->sp.regs[SP_STATUS_REG] & 0x200) != 0);
    PUTDATA(curr, uint8_t, (dev->sp.regs[SP_STATUS_REG] & 0x400) != 0);
    PUTDATA(curr, uint8_t, (dev->sp.regs[SP_STATUS_REG] & 0x800) != 0);
    PUTDATA(curr, uint8_t, (dev->sp.regs[SP_STATUS_REG] & 0x1000) != 0);
    PUTDATA(curr, uint8_t, (dev->sp.regs[SP_STATUS_REG] & 0x2000) != 0);
    PUTDATA(curr, uint8_t, (dev->sp.regs[SP_STATUS_REG] & 0x4000) != 0);
    PUTDATA(curr, uint8_t, 0);
    PUTDATA(curr, uint32_t, dev->sp.regs[SP_DMA_FULL_REG]);
    PUTDATA(curr, uint32_t, dev->sp.regs[SP_DMA_BUSY_REG]);
    PUTDATA(curr, uint32_t, dev->sp.regs[SP_SEMAPHORE_REG]);

    PUTDATA(curr, uint32_t, dev->sp.regs2[SP_PC_REG]);
    PUTDATA(curr, uint32_t, dev->sp.regs2[SP_IBIST_REG]);

    /* SI */
    PUTDATA(curr, uint32_t, dev->si.regs[SI_DRAM_ADDR_REG]);
    PUTDATA(curr, uint32_t, dev->si.regs[SI_PIF_ADDR_RD64B_REG]);
    PUTDATA(curr, uint32_t, dev->si.regs[SI_PIF_ADDR_WR64B_REG]);
    PUTDATA(curr, uint32_t, dev->si.regs[SI_STATUS_REG]);

    /* VI */
    PUTDATA(curr, uint32_t, dev->vi.regs[VI_STATUS_REG]);
    PUTDATA(curr, uint32_t, dev->vi.regs[VI_ORIGIN_REG]);
    PUTDATA(curr, uint32_t, dev->vi.regs[VI_WIDTH_REG]);
    PUTDATA(curr, uint32_t, dev->vi.regs[VI_V_INTR_REG]);
    PUTDATA(curr, uint32_t, dev->vi.regs[VI_CURRENT_REG]);
    PUTDATA(curr, uint32_t, dev->vi.regs[VI_BURST_REG]);
    PUTDATA(curr, uint32_t, dev->vi.regs[VI_V_SYNC_REG]);
    PUTDATA(curr, uint32_t, dev->vi.regs[VI_H_SYNC_REG]);
    PUTDATA(curr, uint32_t, dev->vi.regs[VI_LEAP_REG]);
    PUTDATA(curr, uint32_t, dev->vi.regs[VI_H_START_REG]);
    PUTDATA(curr, uint32_t, dev->vi.regs[VI_V_START_REG]);
    PUTDATA(curr, uint32_t, dev->vi.regs[VI_V_BURST_REG]);
    PUTDATA(curr, uint32_t, dev->vi.regs[VI_X_SCALE_REG]);
    PUTDATA(curr, uint32_t, dev->vi.regs[VI_Y_SCALE_REG]);
    PUTDATA(curr, uint32_t, dev->vi.delay);

    /* RI */
    PUTDATA(curr, uint32_t, dev->ri.regs[RI_MODE_REG]);
    PUTDATA(curr, uint32_t, dev->ri.regs[RI_CONFIG_REG]);
    PUTDATA(curr, uint32_t, dev->ri.regs[RI_CURRENT_LOAD_REG]);
    PUTDATA(curr, uint32_t, dev->ri.regs[RI_SELECT_REG]);
    PUTDATA(curr, uint32_t, dev->ri.regs[RI_REFRESH_REG]);
    PUTDATA(curr, uint32_t, dev->ri.regs[RI_LATENCY_REG]);
    PUTDATA(curr, uint32_t, dev->ri.regs[RI_ERROR_REG]);
    PUTDATA(curr, uint32_t, dev->ri.regs[RI_WERROR_REG]);

    /* AI */
    PUTDATA(curr, uint32_t, dev->ai.regs[AI_DRAM_ADDR_REG]);
    PUTDATA(curr, uint32_t, dev->ai.regs[AI_LEN_REG]);
    PUTDATA(curr, uint32_t, dev->ai.regs[AI_CONTROL_REG]);
    PUTDATA(curr, uint32_t, dev->ai.regs[AI_STATUS_REG]);
    PUTDATA(curr, uint32_t, dev->ai.regs[AI_DACRATE_REG]);
    PUTDATA(curr, uint32_t, dev->ai.regs[AI_BITRATE_REG]);
    PUTDATA(curr, uint32_t, dev->ai.fifo[1].duration);
    PUTDATA(curr, uint32_t, dev->ai.fifo[1].length);
    PUTDATA(curr, uint32_t, dev->ai.fifo[0].duration);
    PUTDATA(curr, uint32_t, dev->ai.fifo[0].length);

    /* DPC */
    PUTDATA(curr, uint32_t, dev->dp.dpc_regs[DPC_START_REG]);
    PUTDATA(curr, uint32_t, dev->dp.dpc_regs[DPC_END_REG]);
    PUTDATA(curr, uint32_t, dev->dp.dpc_regs[DPC_CURRENT_REG]);
    PUTDATA(curr, uint32_t, 0);
    PUTDATA(curr, uint32_t, dev->dp.dpc_regs[DPC_STATUS_REG]);
    PUTDATA(curr, uint8_t, (dev->dp.dpc_regs[DPC_STATUS_REG] & 0x1) != 0);
    PUTDATA(curr, uint8_t, (dev->dp.dpc_regs[DPC_STATUS_REG] & 0x2) != 0);
    PUTDATA(curr, uint8_t, (dev->dp.dpc_regs[DPC_STATUS_REG] & 0x4) != 0);
    PUTDATA(curr, uint8_t, (dev->dp.dpc_regs[DPC_STATUS_REG] & 0x8) != 0);
    PUTDATA(curr, uint8_t, (dev->dp.dpc_regs[DPC_STATUS_REG] & 0x10) != 0);
    PUTDATA(curr, uint8_t, (dev->dp.dpc_regs[DPC_STATUS_REG] & 0x20) != 0);
    PUTDATA(curr, uint8_t, (dev->dp.dpc_regs[DPC_STATUS_REG] & 0x40) != 0);
    PUTDATA(curr, uint8_t, (dev->dp.dpc_regs[DPC_STATUS_REG] & 0x80) != 0);
    PUTDATA(curr, uint8_t, (dev->dp.dpc_regs[DPC_STATUS_REG] & 0x100) != 0);
    PUTDATA(curr, uint8_t, (dev->dp.dpc_regs[DPC_STATUS_REG] & 0x200) != 0);
    PUTDATA(curr, uint8_t, (dev->dp.dpc_regs[DPC_STATUS_REG] & 0x400) != 0);
    PUTDATA(curr, uint8_t, 0);
    PUTDATA(curr, uint32_t, dev->dp.dpc_regs[DPC_CLOCK_REG]);
    PUTDATA(curr, uint32_t, dev->dp.dpc_regs[DPC_BUFBUSY_REG]);
    PUTDATA(curr, uint32_t, dev->dp.dpc_regs[DPC_PIPEBUSY_REG]);
    PUTDATA(curr, uint32_t, dev->dp.dpc_regs[DPC_TMEM_REG]);

    /* DPS */
    PUTDATA(curr, uint32_t, dev->dp.dps_regs[DPS_TBIST_REG]);
    PUTDATA(curr, uint32_t, dev->dp.dps_regs[DPS_TEST_MODE_REG]);
    PUTDATA(curr, uint32_t, dev->dp.dps_regs[DPS_BUFTEST_ADDR_REG]);
    PUTDATA(curr, uint32_t, dev->dp.dps_regs[DPS_BUFTEST_DATA_REG]);

    /* Large arrays */
    PUTARRAY(dev->rdram.dram, curr, uint32_t, RDRAM_MAX_SIZE / 4);
    PUTARRAY(dev->sp.mem, curr, uint32_t, SP_MEM_SIZE / 4);
    PUTARRAY(dev->pif.ram, curr, uint8_t, PIF_RAM_SIZE);

    PUTDATA(curr, int32_t, dev->cart.use_flashram);
    curr += 4 + 8 + 4 + 4; /* flashram state placeholder */

    PUTARRAY(dev->r4300.cp0.tlb.LUT_r, curr, uint32_t, 0x100000);
    PUTARRAY(dev->r4300.cp0.tlb.LUT_w, curr, uint32_t, 0x100000);

    /* R4300 core state */
    PUTDATA(curr, uint32_t, *r4300_llbit((struct r4300_core *)&dev->r4300));
    PUTARRAY(r4300_regs((struct r4300_core *)&dev->r4300), curr, int64_t, 32);
    PUTARRAY(cp0_regs, curr, uint32_t, CP0_REGS_COUNT);
    PUTDATA(curr, int64_t, *r4300_mult_lo((struct r4300_core *)&dev->r4300));
    PUTDATA(curr, int64_t, *r4300_mult_hi((struct r4300_core *)&dev->r4300));

    {
        const cp1_reg *cp1_regs = r4300_cp1_regs((struct cp1 *)&dev->r4300.cp1);
        PUTARRAY(&cp1_regs->dword, curr, int64_t, 32);
    }

    PUTDATA(curr, uint32_t, *r4300_cp1_fcr0((struct cp1 *)&dev->r4300.cp1));
    PUTDATA(curr, uint32_t, *r4300_cp1_fcr31((struct cp1 *)&dev->r4300.cp1));

    /* TLB entries */
    for (i = 0; i < 32; i++)
    {
        PUTDATA(curr, int16_t,  dev->r4300.cp0.tlb.entries[i].mask);
        PUTDATA(curr, int16_t,  0);
        PUTDATA(curr, uint32_t, dev->r4300.cp0.tlb.entries[i].vpn2);
        PUTDATA(curr, char,     dev->r4300.cp0.tlb.entries[i].g);
        PUTDATA(curr, unsigned char, dev->r4300.cp0.tlb.entries[i].asid);
        PUTDATA(curr, int16_t,  0);
        PUTDATA(curr, uint32_t, dev->r4300.cp0.tlb.entries[i].pfn_even);
        PUTDATA(curr, char,     dev->r4300.cp0.tlb.entries[i].c_even);
        PUTDATA(curr, char,     dev->r4300.cp0.tlb.entries[i].d_even);
        PUTDATA(curr, char,     dev->r4300.cp0.tlb.entries[i].v_even);
        PUTDATA(curr, char,     0);
        PUTDATA(curr, uint32_t, dev->r4300.cp0.tlb.entries[i].pfn_odd);
        PUTDATA(curr, char,     dev->r4300.cp0.tlb.entries[i].c_odd);
        PUTDATA(curr, char,     dev->r4300.cp0.tlb.entries[i].d_odd);
        PUTDATA(curr, char,     dev->r4300.cp0.tlb.entries[i].v_odd);
        PUTDATA(curr, char,     dev->r4300.cp0.tlb.entries[i].r);

        PUTDATA(curr, uint32_t, dev->r4300.cp0.tlb.entries[i].start_even);
        PUTDATA(curr, uint32_t, dev->r4300.cp0.tlb.entries[i].end_even);
        PUTDATA(curr, uint32_t, dev->r4300.cp0.tlb.entries[i].phys_even);
        PUTDATA(curr, uint32_t, dev->r4300.cp0.tlb.entries[i].start_odd);
        PUTDATA(curr, uint32_t, dev->r4300.cp0.tlb.entries[i].end_odd);
        PUTDATA(curr, uint32_t, dev->r4300.cp0.tlb.entries[i].phys_odd);
    }

    PUTDATA(curr, uint32_t, *r4300_pc((struct r4300_core *)&dev->r4300));
    PUTDATA(curr, uint32_t, *r4300_cp0_next_interrupt((struct cp0 *)&dev->r4300.cp0));
    PUTDATA(curr, uint32_t, 0); /* was next_vi */
    PUTDATA(curr, uint32_t, dev->vi.field);

    /* Event queue */
    to_little_endian_buffer(queue, 4, sizeof(queue) / 4);
    PUTARRAY(queue, curr, char, sizeof(queue));

    /* using_tlb flag */
#ifdef NEW_DYNAREC
    PUTDATA(curr, uint32_t, using_tlb);
#else
    PUTDATA(curr, uint32_t, 0);
#endif

    /* Extra state (v1.2+ data_0001_0200 region) */
    PUTDATA(curr, uint32_t, dev->ai.last_read);
    PUTDATA(curr, uint32_t, dev->ai.delayed_carry);
    PUTDATA(curr, uint32_t, dev->cart.cart_rom.last_write);
    PUTDATA(curr, uint32_t, 0); /* was rom_written */
    PUTDATA(curr, uint32_t, 0); /* was rsp_task_locked */

    PUTDATA(curr, uint16_t, dev->cart.af_rtc.control);
    PUTDATA(curr, uint16_t, 0); /* padding */
    PUTDATA(curr, int64_t,  dev->cart.af_rtc.now);
    PUTDATA(curr, int64_t,  dev->cart.af_rtc.last_update_rtc);

    for (i = 0; i < GAME_CONTROLLERS_COUNT; ++i)
        PUTDATA(curr, uint8_t, dev->controllers[i].status);

    for (i = 0; i < GAME_CONTROLLERS_COUNT; ++i)
        PUTDATA(curr, uint8_t, dev->rumblepaks[i].state);


    for (i = 0; i < PIF_CHANNELS_COUNT; ++i)
    {
        PUTDATA(curr, int8_t, (dev->pif.channels[i].tx == NULL)
                ? (int8_t)-1
                : (int8_t)(dev->pif.channels[i].tx - dev->pif.ram));
    }

    PUTDATA(curr, uint8_t, dev->si.dma_dir);
    PUTDATA(curr, uint8_t, dev->dp.do_on_unfreeze);
    PUTDATA(curr, uint32_t, dev->vi.count_per_scanline);

    /* Extra RDRAM modules */
    for (i = 1; i < RDRAM_MAX_MODULES_COUNT; ++i)
    {
        PUTDATA(curr, uint32_t, dev->rdram.regs[i][RDRAM_CONFIG_REG]);
        PUTDATA(curr, uint32_t, dev->rdram.regs[i][RDRAM_DEVICE_ID_REG]);
        PUTDATA(curr, uint32_t, dev->rdram.regs[i][RDRAM_DELAY_REG]);
        PUTDATA(curr, uint32_t, dev->rdram.regs[i][RDRAM_MODE_REG]);
        PUTDATA(curr, uint32_t, dev->rdram.regs[i][RDRAM_REF_INTERVAL_REG]);
        PUTDATA(curr, uint32_t, dev->rdram.regs[i][RDRAM_REF_ROW_REG]);
        PUTDATA(curr, uint32_t, dev->rdram.regs[i][RDRAM_RAS_INTERVAL_REG]);
        PUTDATA(curr, uint32_t, dev->rdram.regs[i][RDRAM_MIN_INTERVAL_REG]);
        PUTDATA(curr, uint32_t, dev->rdram.regs[i][RDRAM_ADDR_SELECT_REG]);
        PUTDATA(curr, uint32_t, dev->rdram.regs[i][RDRAM_DEVICE_MANUF_REG]);
    }


    /* NEW_DYNAREC extra */
#ifdef NEW_DYNAREC
    PUTDATA(curr, uint32_t, stop_after_jal);
#else
    PUTDATA(curr, uint32_t, 0);
#endif

    /* SP DMA FIFO */
    PUTDATA(curr, uint32_t, dev->sp.fifo[0].dir);
    PUTDATA(curr, uint32_t, dev->sp.fifo[0].length);
    PUTDATA(curr, uint32_t, dev->sp.fifo[0].memaddr);
    PUTDATA(curr, uint32_t, dev->sp.fifo[0].dramaddr);
    PUTDATA(curr, uint32_t, dev->sp.fifo[1].dir);
    PUTDATA(curr, uint32_t, dev->sp.fifo[1].length);
    PUTDATA(curr, uint32_t, dev->sp.fifo[1].memaddr);
    PUTDATA(curr, uint32_t, dev->sp.fifo[1].dramaddr);

    PUTARRAY(dev->cart.flashram.page_buf, curr, uint8_t, 128);
    PUTARRAY(dev->cart.flashram.silicon_id, curr, uint32_t, 2);
    PUTDATA(curr, uint32_t, dev->cart.flashram.status);
    PUTDATA(curr, uint16_t, dev->cart.flashram.erase_page);
    PUTDATA(curr, uint16_t, dev->cart.flashram.mode);

    PUTDATA(curr, uint64_t, *r4300_cp0_latch((struct cp0 *)&dev->r4300.cp0));
    PUTDATA(curr, uint64_t, *r4300_cp2_latch((struct cp2 *)&dev->r4300.cp2));

    /* Commit the slot */
    slot->frame = frame;
    slot->valid = 1;

    /* Advance ring buffer head */
    g_rollback.head = (g_rollback.head + 1) % ROLLBACK_RING_SIZE;
    if (g_rollback.count < ROLLBACK_RING_SIZE)
        g_rollback.count++;
}

int rollback_load(struct device *dev, unsigned int frames_back)
{
    struct rollback_slot *slot;
    unsigned char *curr;
    int i;
    uint32_t FCR31;
    char queue[1024];
    unsigned int idx;

    uint32_t *cp0_regs = r4300_cp0_regs(&dev->r4300.cp0);

    if (frames_back == 0 || frames_back > (unsigned int)g_rollback.count)
        return 0;

    idx = (g_rollback.head + ROLLBACK_RING_SIZE - frames_back) % ROLLBACK_RING_SIZE;
    slot = &g_rollback.slots[idx];

    if (!slot->valid || slot->data == NULL)
        return 0;

    curr = slot->data;

    curr += 44;

    dev->rdram.regs[0][RDRAM_CONFIG_REG]       = GETDATA(curr, uint32_t);
    dev->rdram.regs[0][RDRAM_DEVICE_ID_REG]    = GETDATA(curr, uint32_t);
    dev->rdram.regs[0][RDRAM_DELAY_REG]        = GETDATA(curr, uint32_t);
    dev->rdram.regs[0][RDRAM_MODE_REG]         = GETDATA(curr, uint32_t);
    dev->rdram.regs[0][RDRAM_REF_INTERVAL_REG] = GETDATA(curr, uint32_t);
    dev->rdram.regs[0][RDRAM_REF_ROW_REG]      = GETDATA(curr, uint32_t);
    dev->rdram.regs[0][RDRAM_RAS_INTERVAL_REG] = GETDATA(curr, uint32_t);
    dev->rdram.regs[0][RDRAM_MIN_INTERVAL_REG] = GETDATA(curr, uint32_t);
    dev->rdram.regs[0][RDRAM_ADDR_SELECT_REG]  = GETDATA(curr, uint32_t);
    dev->rdram.regs[0][RDRAM_DEVICE_MANUF_REG] = GETDATA(curr, uint32_t);

    /* MI */
    curr += 4; /* Padding */
    dev->mi.regs[MI_INIT_MODE_REG] = GETDATA(curr, uint32_t);
    curr += 4; /* Duplicate MI init mode flags */
    dev->mi.regs[MI_VERSION_REG]   = GETDATA(curr, uint32_t);
    dev->mi.regs[MI_INTR_REG]      = GETDATA(curr, uint32_t);
    dev->mi.regs[MI_INTR_MASK_REG] = GETDATA(curr, uint32_t);
    curr += 4; /* Padding */
    curr += 8; /* Duplicated MI intr flags + padding */

    /* PI */
    dev->pi.regs[PI_DRAM_ADDR_REG]    = GETDATA(curr, uint32_t);
    dev->pi.regs[PI_CART_ADDR_REG]    = GETDATA(curr, uint32_t);
    dev->pi.regs[PI_RD_LEN_REG]      = GETDATA(curr, uint32_t);
    dev->pi.regs[PI_WR_LEN_REG]      = GETDATA(curr, uint32_t);
    dev->pi.regs[PI_STATUS_REG]      = GETDATA(curr, uint32_t);
    dev->pi.regs[PI_BSD_DOM1_LAT_REG] = GETDATA(curr, uint32_t);
    dev->pi.regs[PI_BSD_DOM1_PWD_REG] = GETDATA(curr, uint32_t);
    dev->pi.regs[PI_BSD_DOM1_PGS_REG] = GETDATA(curr, uint32_t);
    dev->pi.regs[PI_BSD_DOM1_RLS_REG] = GETDATA(curr, uint32_t);
    dev->pi.regs[PI_BSD_DOM2_LAT_REG] = GETDATA(curr, uint32_t);
    dev->pi.regs[PI_BSD_DOM2_PWD_REG] = GETDATA(curr, uint32_t);
    dev->pi.regs[PI_BSD_DOM2_PGS_REG] = GETDATA(curr, uint32_t);
    dev->pi.regs[PI_BSD_DOM2_RLS_REG] = GETDATA(curr, uint32_t);

    /* SP */
    dev->sp.regs[SP_MEM_ADDR_REG]  = GETDATA(curr, uint32_t);
    dev->sp.regs[SP_DRAM_ADDR_REG] = GETDATA(curr, uint32_t);
    dev->sp.regs[SP_RD_LEN_REG]    = GETDATA(curr, uint32_t);
    dev->sp.regs[SP_WR_LEN_REG]    = GETDATA(curr, uint32_t);
    curr += 4; /* Padding */
    dev->sp.regs[SP_STATUS_REG]    = GETDATA(curr, uint32_t);
    curr += 16; /* Duplicated SP flags + padding */
    dev->sp.regs[SP_DMA_FULL_REG]  = GETDATA(curr, uint32_t);
    dev->sp.regs[SP_DMA_BUSY_REG]  = GETDATA(curr, uint32_t);
    dev->sp.regs[SP_SEMAPHORE_REG] = GETDATA(curr, uint32_t);

    dev->sp.regs2[SP_PC_REG]    = GETDATA(curr, uint32_t);
    dev->sp.regs2[SP_IBIST_REG] = GETDATA(curr, uint32_t);

    /* SI */
    dev->si.regs[SI_DRAM_ADDR_REG]      = GETDATA(curr, uint32_t);
    dev->si.regs[SI_PIF_ADDR_RD64B_REG] = GETDATA(curr, uint32_t);
    dev->si.regs[SI_PIF_ADDR_WR64B_REG] = GETDATA(curr, uint32_t);
    dev->si.regs[SI_STATUS_REG]         = GETDATA(curr, uint32_t);

    /* VI */
    dev->vi.regs[VI_STATUS_REG]  = GETDATA(curr, uint32_t);
    dev->vi.regs[VI_ORIGIN_REG]  = GETDATA(curr, uint32_t);
    dev->vi.regs[VI_WIDTH_REG]   = GETDATA(curr, uint32_t);
    dev->vi.regs[VI_V_INTR_REG]  = GETDATA(curr, uint32_t);
    dev->vi.regs[VI_CURRENT_REG] = GETDATA(curr, uint32_t);
    dev->vi.regs[VI_BURST_REG]   = GETDATA(curr, uint32_t);
    dev->vi.regs[VI_V_SYNC_REG]  = GETDATA(curr, uint32_t);
    dev->vi.regs[VI_H_SYNC_REG]  = GETDATA(curr, uint32_t);
    dev->vi.regs[VI_LEAP_REG]    = GETDATA(curr, uint32_t);
    dev->vi.regs[VI_H_START_REG] = GETDATA(curr, uint32_t);
    dev->vi.regs[VI_V_START_REG] = GETDATA(curr, uint32_t);
    dev->vi.regs[VI_V_BURST_REG] = GETDATA(curr, uint32_t);
    dev->vi.regs[VI_X_SCALE_REG] = GETDATA(curr, uint32_t);
    dev->vi.regs[VI_Y_SCALE_REG] = GETDATA(curr, uint32_t);
    dev->vi.delay                = GETDATA(curr, uint32_t);
    gfx.viStatusChanged();
    gfx.viWidthChanged();

    /* RI */
    dev->ri.regs[RI_MODE_REG]         = GETDATA(curr, uint32_t);
    dev->ri.regs[RI_CONFIG_REG]       = GETDATA(curr, uint32_t);
    dev->ri.regs[RI_CURRENT_LOAD_REG] = GETDATA(curr, uint32_t);
    dev->ri.regs[RI_SELECT_REG]       = GETDATA(curr, uint32_t);
    dev->ri.regs[RI_REFRESH_REG]      = GETDATA(curr, uint32_t);
    dev->ri.regs[RI_LATENCY_REG]      = GETDATA(curr, uint32_t);
    dev->ri.regs[RI_ERROR_REG]        = GETDATA(curr, uint32_t);
    dev->ri.regs[RI_WERROR_REG]       = GETDATA(curr, uint32_t);

    /* AI */
    dev->ai.regs[AI_DRAM_ADDR_REG] = GETDATA(curr, uint32_t);
    dev->ai.regs[AI_LEN_REG]       = GETDATA(curr, uint32_t);
    dev->ai.regs[AI_CONTROL_REG]   = GETDATA(curr, uint32_t);
    dev->ai.regs[AI_STATUS_REG]    = GETDATA(curr, uint32_t);
    dev->ai.regs[AI_DACRATE_REG]   = GETDATA(curr, uint32_t);
    dev->ai.regs[AI_BITRATE_REG]   = GETDATA(curr, uint32_t);
    dev->ai.fifo[1].duration       = GETDATA(curr, uint32_t);
    dev->ai.fifo[1].length         = GETDATA(curr, uint32_t);
    dev->ai.fifo[0].duration       = GETDATA(curr, uint32_t);
    dev->ai.fifo[0].length         = GETDATA(curr, uint32_t);
    /* best-effort address init */
    dev->ai.fifo[0].address = dev->ai.regs[AI_DRAM_ADDR_REG];
    dev->ai.fifo[1].address = dev->ai.regs[AI_DRAM_ADDR_REG];
    dev->ai.samples_format_changed = 1;

    /* DPC */
    dev->dp.dpc_regs[DPC_START_REG]    = GETDATA(curr, uint32_t);
    dev->dp.dpc_regs[DPC_END_REG]      = GETDATA(curr, uint32_t);
    dev->dp.dpc_regs[DPC_CURRENT_REG]  = GETDATA(curr, uint32_t);
    curr += 4; /* Padding */
    dev->dp.dpc_regs[DPC_STATUS_REG]   = GETDATA(curr, uint32_t);
    curr += 12; /* Duplicated DPC flags + padding */
    dev->dp.dpc_regs[DPC_CLOCK_REG]    = GETDATA(curr, uint32_t);
    dev->dp.dpc_regs[DPC_BUFBUSY_REG]  = GETDATA(curr, uint32_t);
    dev->dp.dpc_regs[DPC_PIPEBUSY_REG] = GETDATA(curr, uint32_t);
    dev->dp.dpc_regs[DPC_TMEM_REG]     = GETDATA(curr, uint32_t);

    /* DPS */
    dev->dp.dps_regs[DPS_TBIST_REG]        = GETDATA(curr, uint32_t);
    dev->dp.dps_regs[DPS_TEST_MODE_REG]    = GETDATA(curr, uint32_t);
    dev->dp.dps_regs[DPS_BUFTEST_ADDR_REG] = GETDATA(curr, uint32_t);
    dev->dp.dps_regs[DPS_BUFTEST_DATA_REG] = GETDATA(curr, uint32_t);

    /* Large arrays */
    COPYARRAY(dev->rdram.dram, curr, uint32_t, RDRAM_MAX_SIZE / 4);
    COPYARRAY(dev->sp.mem, curr, uint32_t, SP_MEM_SIZE / 4);
    COPYARRAY(dev->pif.ram, curr, uint8_t, PIF_RAM_SIZE);

    dev->cart.use_flashram = GETDATA(curr, int32_t);
    curr += 4 + 8 + 4 + 4; /* flashram state placeholder */
    poweron_flashram(&dev->cart.flashram);

    COPYARRAY(dev->r4300.cp0.tlb.LUT_r, curr, uint32_t, 0x100000);
    COPYARRAY(dev->r4300.cp0.tlb.LUT_w, curr, uint32_t, 0x100000);

    /* R4300 core state */
    *r4300_llbit(&dev->r4300) = GETDATA(curr, uint32_t);
    COPYARRAY(r4300_regs(&dev->r4300), curr, int64_t, 32);
    COPYARRAY(cp0_regs, curr, uint32_t, CP0_REGS_COUNT);
    *r4300_mult_lo(&dev->r4300) = GETDATA(curr, int64_t);
    *r4300_mult_hi(&dev->r4300) = GETDATA(curr, int64_t);

    {
        cp1_reg *cp1_regs = r4300_cp1_regs(&dev->r4300.cp1);
        COPYARRAY(&cp1_regs->dword, curr, int64_t, 32);
    }

    *r4300_cp1_fcr0(&dev->r4300.cp1)  = GETDATA(curr, uint32_t);
    FCR31 = GETDATA(curr, uint32_t);
    *r4300_cp1_fcr31(&dev->r4300.cp1) = FCR31;
    set_fpr_pointers(&dev->r4300.cp1, cp0_regs[CP0_STATUS_REG]);
    update_x86_rounding_mode(&dev->r4300.cp1);

    /* TLB entries */
    for (i = 0; i < 32; i++)
    {
        dev->r4300.cp0.tlb.entries[i].mask     = GETDATA(curr, int16_t);
        curr += 2;
        dev->r4300.cp0.tlb.entries[i].vpn2     = GETDATA(curr, uint32_t);
        dev->r4300.cp0.tlb.entries[i].g        = GETDATA(curr, char);
        dev->r4300.cp0.tlb.entries[i].asid     = GETDATA(curr, unsigned char);
        curr += 2;
        dev->r4300.cp0.tlb.entries[i].pfn_even = GETDATA(curr, uint32_t);
        dev->r4300.cp0.tlb.entries[i].c_even   = GETDATA(curr, char);
        dev->r4300.cp0.tlb.entries[i].d_even   = GETDATA(curr, char);
        dev->r4300.cp0.tlb.entries[i].v_even   = GETDATA(curr, char);
        curr++;
        dev->r4300.cp0.tlb.entries[i].pfn_odd  = GETDATA(curr, uint32_t);
        dev->r4300.cp0.tlb.entries[i].c_odd    = GETDATA(curr, char);
        dev->r4300.cp0.tlb.entries[i].d_odd    = GETDATA(curr, char);
        dev->r4300.cp0.tlb.entries[i].v_odd    = GETDATA(curr, char);
        dev->r4300.cp0.tlb.entries[i].r        = GETDATA(curr, char);

        dev->r4300.cp0.tlb.entries[i].start_even = GETDATA(curr, uint32_t);
        dev->r4300.cp0.tlb.entries[i].end_even   = GETDATA(curr, uint32_t);
        dev->r4300.cp0.tlb.entries[i].phys_even  = GETDATA(curr, uint32_t);
        dev->r4300.cp0.tlb.entries[i].start_odd  = GETDATA(curr, uint32_t);
        dev->r4300.cp0.tlb.entries[i].end_odd    = GETDATA(curr, uint32_t);
        dev->r4300.cp0.tlb.entries[i].phys_odd   = GETDATA(curr, uint32_t);
    }

    savestates_load_set_pc(&dev->r4300, GETDATA(curr, uint32_t));

    *r4300_cp0_next_interrupt(&dev->r4300.cp0) = GETDATA(curr, uint32_t);
    curr += 4; /* was next_vi */
    dev->vi.field = GETDATA(curr, uint32_t);

    /* Event queue */
    to_little_endian_buffer(queue, 4, 256);
    COPYARRAY(queue, curr, char, sizeof(queue));
    to_little_endian_buffer(queue, 4, 256);
    load_eventqueue_infos(&dev->r4300.cp0, queue);

#ifdef NEW_DYNAREC
    using_tlb = GETDATA(curr, uint32_t);
#else
    curr += sizeof(uint32_t);
#endif

    dev->ai.last_read       = GETDATA(curr, uint32_t);
    dev->ai.delayed_carry   = GETDATA(curr, uint32_t);
    dev->cart.cart_rom.last_write = GETDATA(curr, uint32_t);
    curr += 4; /* was rom_written */
    curr += 4; /* was rsp_task_locked */

    dev->cart.af_rtc.control        = GETDATA(curr, uint16_t);
    curr += 2; /* padding */
    dev->cart.af_rtc.now            = (time_t)GETDATA(curr, int64_t);
    dev->cart.af_rtc.last_update_rtc = (time_t)GETDATA(curr, int64_t);

    for (i = 0; i < GAME_CONTROLLERS_COUNT; ++i)
        dev->controllers[i].status = GETDATA(curr, uint8_t);

    for (i = 0; i < GAME_CONTROLLERS_COUNT; ++i)
        dev->rumblepaks[i].state = GETDATA(curr, uint8_t);


    /* PIF channels */
    for (i = 0; i < PIF_CHANNELS_COUNT; ++i)
    {
        int offset = GETDATA(curr, int8_t);
        if (offset >= 0)
            setup_pif_channel(&dev->pif.channels[i], dev->pif.ram + offset);
        else
            disable_pif_channel(&dev->pif.channels[i]);
    }

    dev->si.dma_dir          = GETDATA(curr, uint8_t);
    dev->dp.do_on_unfreeze   = GETDATA(curr, uint8_t);
    dev->vi.count_per_scanline = GETDATA(curr, uint32_t);

    /* Extra RDRAM modules */
    for (i = 1; i < RDRAM_MAX_MODULES_COUNT; ++i)
    {
        dev->rdram.regs[i][RDRAM_CONFIG_REG]       = GETDATA(curr, uint32_t);
        dev->rdram.regs[i][RDRAM_DEVICE_ID_REG]    = GETDATA(curr, uint32_t);
        dev->rdram.regs[i][RDRAM_DELAY_REG]        = GETDATA(curr, uint32_t);
        dev->rdram.regs[i][RDRAM_MODE_REG]         = GETDATA(curr, uint32_t);
        dev->rdram.regs[i][RDRAM_REF_INTERVAL_REG] = GETDATA(curr, uint32_t);
        dev->rdram.regs[i][RDRAM_REF_ROW_REG]      = GETDATA(curr, uint32_t);
        dev->rdram.regs[i][RDRAM_RAS_INTERVAL_REG] = GETDATA(curr, uint32_t);
        dev->rdram.regs[i][RDRAM_MIN_INTERVAL_REG] = GETDATA(curr, uint32_t);
        dev->rdram.regs[i][RDRAM_ADDR_SELECT_REG]  = GETDATA(curr, uint32_t);
        dev->rdram.regs[i][RDRAM_DEVICE_MANUF_REG] = GETDATA(curr, uint32_t);
    }


    /* NEW_DYNAREC extra */
#ifdef NEW_DYNAREC
    stop_after_jal = GETDATA(curr, uint32_t);
#else
    curr += sizeof(uint32_t);
#endif

    /* SP DMA FIFO */
    dev->sp.fifo[0].dir      = GETDATA(curr, uint32_t);
    dev->sp.fifo[0].length   = GETDATA(curr, uint32_t);
    dev->sp.fifo[0].memaddr  = GETDATA(curr, uint32_t);
    dev->sp.fifo[0].dramaddr = GETDATA(curr, uint32_t);
    dev->sp.fifo[1].dir      = GETDATA(curr, uint32_t);
    dev->sp.fifo[1].length   = GETDATA(curr, uint32_t);
    dev->sp.fifo[1].memaddr  = GETDATA(curr, uint32_t);
    dev->sp.fifo[1].dramaddr = GETDATA(curr, uint32_t);

    /* Flashram state (v1.8+) */
    COPYARRAY(dev->cart.flashram.page_buf, curr, uint8_t, 128);
    COPYARRAY(dev->cart.flashram.silicon_id, curr, uint32_t, 2);
    dev->cart.flashram.status     = GETDATA(curr, uint32_t);
    dev->cart.flashram.erase_page = GETDATA(curr, uint16_t);
    dev->cart.flashram.mode       = GETDATA(curr, uint16_t);

    /* cp0 and cp2 latch (v1.9) */
    *r4300_cp0_latch(&dev->r4300.cp0) = GETDATA(curr, uint64_t);
    *r4300_cp2_latch(&dev->r4300.cp2) = GETDATA(curr, uint64_t);

    /* Zilmar-Spec plugin expects a call with control_id = -1 */
    if (input.controllerCommand)
        input.controllerCommand(-1, NULL);

    /* Reset fb state */
    poweron_fb(&dev->dp.fb);

    dev->sp.rsp_task_locked = 0;
    dev->r4300.cp0.interrupt_unsafe_state = 0;

    *r4300_cp0_last_addr(&dev->r4300.cp0) = *r4300_pc(&dev->r4300);

    DebugMessage(M64MSG_INFO, "Rollback: restored state from frame %u (slot %u, %u back)",
                 slot->frame, idx, frames_back);

    return 1;
}

int rollback_count(void)
{
    return g_rollback.count;
}
