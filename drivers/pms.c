/*
 * drivers/pms.c - ESP32-S3 Permission Management System (PMS / MPU) Driver
 *
 * Enforces hardware memory isolation between World 0 (Kernel) and World 1 (User Tasks)
 * by programming ESP32-S3 Sensitive split-line and IRAM0/DRAM0 PMS registers.
 */

#include <radix/types.h>
#include <radix/pms.h>
#include <radix/sched.h>
#include <radix/kprintf.h>
#include <esp32s3_soc.h>

#ifndef RADIX_HOST_TEST
/* Linker boundary symbols from linker/esp32s3_ram.ld */
extern uint8_t _iram_start[];
extern uint8_t _iram_end[];
extern uint8_t _dram_start[];
extern uint8_t _dram_end[];
extern uint8_t _heap_start[];
extern uint8_t _heap_end[];
#endif

/* Global PMS regions table */
static pms_region_t s_pms_regions[PMS_MAX_REGIONS];

/* Shadow mirror of programmed ESP32-S3 PMS registers */
static pms_hw_regs_t s_pms_hw_shadow = {0};

/* Telemetry and fault tracking stats */
world_pms_stats_t g_pms_stats = {0};

static uint32_t pms_encode_iram0_perm(uint32_t perm) {
    uint32_t hw = 0;
    if (perm & PMS_PERM_R) hw |= SENSITIVE_IRAM0_PMS_SRAM_R;
    if (perm & PMS_PERM_W) hw |= SENSITIVE_IRAM0_PMS_SRAM_W;
    if (perm & PMS_PERM_X) hw |= SENSITIVE_IRAM0_PMS_SRAM_F;
    return hw;
}

static uint32_t pms_encode_dram0_perm(uint32_t perm) {
    uint32_t hw = 0;
    if (perm & PMS_PERM_R) hw |= SENSITIVE_DRAM0_PMS_SRAM_R;
    if (perm & PMS_PERM_W) hw |= SENSITIVE_DRAM0_PMS_SRAM_W;
    return hw;
}

static uint32_t pms_encode_split_line(uintptr_t addr, bool is_iram) {
    static const uint32_t iram_limits[7] = {
        0x4037FFFFu, 0x4038FFFFu, 0x4039FFFFu, 0x403AFFFFu,
        0x403BFFFFu, 0x403CFFFFu, 0x403DFFFFu
    };
    static const uint32_t dram_limits[7] = {
        0x3FC8FFFFu, 0x3FC9FFFFu, 0x3FCAFFFFu, 0x3FCBFFFFu,
        0x3FCCFFFFu, 0x3FCDFFFFu, 0x3FCEFFFFu
    };

    const uint32_t *limits = is_iram ? iram_limits : dram_limits;
    uintptr_t aligned_addr = addr & ~(uintptr_t)0xFFu;
    uint32_t addr32 = (uint32_t)aligned_addr;

    uint32_t regval = 0;
    bool found_eq = false;
    for (uint32_t i = 0; i < 7u; i++) {
        uint32_t cat_val;
        if (found_eq) {
            cat_val = SENSITIVE_SPLIT_LINE_CAT_ABOVE;
        } else if (addr32 <= limits[i]) {
            cat_val = SENSITIVE_SPLIT_LINE_CAT_EQUAL;
            found_eq = true;
        } else {
            cat_val = SENSITIVE_SPLIT_LINE_CAT_BELOW;
        }
        regval |= (cat_val & 0x3u) << (i * 2u);
    }

    uint32_t splitaddr = (addr32 >> 8u) & SENSITIVE_SPLIT_LINE_SPLITADDR_MASK;
    regval |= (splitaddr << SENSITIVE_SPLIT_LINE_SPLITADDR_SHIFT);
    return regval;
}

void pms_sync_hardware(void) {
    uintptr_t iram_end = s_pms_regions[PMS_REGION_KERNEL_IRAM].active
        ? (s_pms_regions[PMS_REGION_KERNEL_IRAM].base_addr + s_pms_regions[PMS_REGION_KERNEL_IRAM].size)
        : (uintptr_t)ESP32S3_IRAM_START;

    uintptr_t dram_base = s_pms_regions[PMS_REGION_KERNEL_DRAM].active
        ? s_pms_regions[PMS_REGION_KERNEL_DRAM].base_addr
        : (uintptr_t)ESP32S3_DRAM_START;
    uintptr_t dram_end = s_pms_regions[PMS_REGION_KERNEL_DRAM].active
        ? (s_pms_regions[PMS_REGION_KERNEL_DRAM].base_addr + s_pms_regions[PMS_REGION_KERNEL_DRAM].size)
        : (uintptr_t)ESP32S3_DRAM_START;

    uintptr_t task_split_start = dram_end;
    uintptr_t task_split_end = dram_end;
    if (s_pms_regions[PMS_REGION_TASK_STACK].active) {
        task_split_start = s_pms_regions[PMS_REGION_TASK_STACK].base_addr & ~(uintptr_t)0xFFu;
        task_split_end = (s_pms_regions[PMS_REGION_TASK_STACK].base_addr +
                          s_pms_regions[PMS_REGION_TASK_STACK].size + 0xFFu) & ~(uintptr_t)0xFFu;
    }

    s_pms_hw_shadow.split_line_ctrl0 = 0u; /* Keep unlocked for dynamic task switch updates */
    s_pms_hw_shadow.split_line_main_id = pms_encode_split_line(dram_base, false);
    s_pms_hw_shadow.split_line_iram_0 = pms_encode_split_line(iram_end, true);
    s_pms_hw_shadow.split_line_iram_1 = pms_encode_split_line(iram_end, true);
    s_pms_hw_shadow.split_line_dram_0 = pms_encode_split_line(task_split_start, false);
    s_pms_hw_shadow.split_line_dram_1 = pms_encode_split_line(task_split_end, false);

    uint32_t iram_w0_p0 = s_pms_regions[PMS_REGION_KERNEL_IRAM].active
        ? pms_encode_iram0_perm(s_pms_regions[PMS_REGION_KERNEL_IRAM].world0_perm)
        : pms_encode_iram0_perm(PMS_PERM_RX);
    uint32_t iram_w1_p0 = s_pms_regions[PMS_REGION_KERNEL_IRAM].active
        ? pms_encode_iram0_perm(s_pms_regions[PMS_REGION_KERNEL_IRAM].world1_perm)
        : 0u;
    uint32_t iram_rx = pms_encode_iram0_perm(PMS_PERM_RX);

    s_pms_hw_shadow.iram0_pms_ctrl0 = 0u;
    s_pms_hw_shadow.iram0_pms_world0 =
        (iram_w0_p0 << SENSITIVE_IRAM0_PMS_AREA_SHIFT(0)) |
        (iram_w0_p0 << SENSITIVE_IRAM0_PMS_AREA_SHIFT(1)) |
        (iram_w0_p0 << SENSITIVE_IRAM0_PMS_AREA_SHIFT(2)) |
        (iram_w0_p0 << SENSITIVE_IRAM0_PMS_AREA_SHIFT(3)) |
        (iram_rx   << SENSITIVE_IRAM0_PMS_CACHE0_SHIFT) |
        (iram_rx   << SENSITIVE_IRAM0_PMS_CACHE1_SHIFT) |
        (iram_rx   << SENSITIVE_IRAM0_PMS_ROM_SHIFT);

    s_pms_hw_shadow.iram0_pms_world1 =
        (iram_w1_p0 << SENSITIVE_IRAM0_PMS_AREA_SHIFT(0));

    uint32_t dram_w0_p0 = s_pms_regions[PMS_REGION_KERNEL_DRAM].active
        ? pms_encode_dram0_perm(s_pms_regions[PMS_REGION_KERNEL_DRAM].world0_perm)
        : pms_encode_dram0_perm(PMS_PERM_RW);
    uint32_t dram_w1_p0 = s_pms_regions[PMS_REGION_KERNEL_DRAM].active
        ? pms_encode_dram0_perm(s_pms_regions[PMS_REGION_KERNEL_DRAM].world1_perm)
        : 0u;

    uint32_t dram_w0_p1 = s_pms_regions[PMS_REGION_TASK_STACK].active
        ? pms_encode_dram0_perm(s_pms_regions[PMS_REGION_TASK_STACK].world0_perm)
        : pms_encode_dram0_perm(PMS_PERM_RW);
    uint32_t dram_w1_p1 = s_pms_regions[PMS_REGION_TASK_STACK].active
        ? pms_encode_dram0_perm(s_pms_regions[PMS_REGION_TASK_STACK].world1_perm)
        : 0u;

    uint32_t dram_w0_p2 = s_pms_regions[PMS_REGION_TASK_EXTRA].active
        ? pms_encode_dram0_perm(s_pms_regions[PMS_REGION_TASK_EXTRA].world0_perm)
        : pms_encode_dram0_perm(PMS_PERM_RW);
    uint32_t dram_w1_p2 = s_pms_regions[PMS_REGION_TASK_EXTRA].active
        ? pms_encode_dram0_perm(s_pms_regions[PMS_REGION_TASK_EXTRA].world1_perm)
        : 0u;

    uint32_t dram_rw = pms_encode_dram0_perm(PMS_PERM_RW);
    uint32_t dram_r  = pms_encode_dram0_perm(PMS_PERM_R);

    s_pms_hw_shadow.dram0_pms_ctrl0 = 0u;
    s_pms_hw_shadow.dram0_pms_world01 =
        (dram_w0_p0 << SENSITIVE_DRAM0_PMS_W0_AREA_SHIFT(0)) |
        (dram_w0_p1 << SENSITIVE_DRAM0_PMS_W0_AREA_SHIFT(1)) |
        (dram_w0_p2 << SENSITIVE_DRAM0_PMS_W0_AREA_SHIFT(2)) |
        (dram_rw    << SENSITIVE_DRAM0_PMS_W0_AREA_SHIFT(3)) |
        (dram_rw    << SENSITIVE_DRAM0_PMS_W0_CACHE0_SHIFT) |
        (dram_rw    << SENSITIVE_DRAM0_PMS_W0_CACHE1_SHIFT) |
        (dram_w1_p0 << SENSITIVE_DRAM0_PMS_W1_AREA_SHIFT(0)) |
        (dram_w1_p1 << SENSITIVE_DRAM0_PMS_W1_AREA_SHIFT(1)) |
        (dram_w1_p2 << SENSITIVE_DRAM0_PMS_W1_AREA_SHIFT(2)) |
        (dram_r     << SENSITIVE_DRAM0_PMS_ROM_W0_SHIFT);

    s_pms_hw_shadow.iram0_monitor_ctrl1 = SENSITIVE_PMS_MONITOR_VIOLATE_EN;
    s_pms_hw_shadow.dram0_monitor_ctrl1 = SENSITIVE_PMS_MONITOR_VIOLATE_EN;

#ifndef RADIX_HOST_TEST
    REG_WRITE(SENSITIVE_CORE_X_IRAM0_DRAM0_DMA_SPLIT_LINE_CONSTRAIN_1_REG, s_pms_hw_shadow.split_line_main_id);
    REG_WRITE(SENSITIVE_CORE_X_IRAM0_DRAM0_DMA_SPLIT_LINE_CONSTRAIN_2_REG, s_pms_hw_shadow.split_line_iram_0);
    REG_WRITE(SENSITIVE_CORE_X_IRAM0_DRAM0_DMA_SPLIT_LINE_CONSTRAIN_3_REG, s_pms_hw_shadow.split_line_iram_1);
    REG_WRITE(SENSITIVE_CORE_X_IRAM0_DRAM0_DMA_SPLIT_LINE_CONSTRAIN_4_REG, s_pms_hw_shadow.split_line_dram_0);
    REG_WRITE(SENSITIVE_CORE_X_IRAM0_DRAM0_DMA_SPLIT_LINE_CONSTRAIN_5_REG, s_pms_hw_shadow.split_line_dram_1);
    REG_WRITE(SENSITIVE_CORE_X_IRAM0_PMS_CONSTRAIN_1_REG, s_pms_hw_shadow.iram0_pms_world1);
    REG_WRITE(SENSITIVE_CORE_X_IRAM0_PMS_CONSTRAIN_2_REG, s_pms_hw_shadow.iram0_pms_world0);
    REG_WRITE(SENSITIVE_CORE_X_DRAM0_PMS_CONSTRAIN_1_REG, s_pms_hw_shadow.dram0_pms_world01);
#endif
}

const pms_hw_regs_t *pms_get_hw_regs(void) {
    return &s_pms_hw_shadow;
}

void pms_init(void) {
    for (uint32_t i = 0; i < PMS_MAX_REGIONS; i++) {
        s_pms_regions[i].region_id = i;
        s_pms_regions[i].base_addr = 0;
        s_pms_regions[i].size = 0;
        s_pms_regions[i].world0_perm = PMS_PERM_NONE;
        s_pms_regions[i].world1_perm = PMS_PERM_NONE;
        s_pms_regions[i].active = false;
    }

    g_pms_stats.world0_to_world1_switches = 0;
    g_pms_stats.world1_to_world0_traps = 0;
    g_pms_stats.pms_violations = 0;
    g_pms_stats.last_fault_addr = 0;
    g_pms_stats.last_fault_exccause = 0;
    g_pms_stats.last_killed_task_id = 0;

#ifndef RADIX_HOST_TEST
    /* Clear any latched PMS monitor interrupts and enable IRAM0/DRAM0 monitors */
    REG_WRITE(SENSITIVE_CORE_0_IRAM0_PMS_MONITOR_1_REG, SENSITIVE_PMS_MONITOR_VIOLATE_CLR);
    REG_WRITE(SENSITIVE_CORE_0_IRAM0_PMS_MONITOR_1_REG, SENSITIVE_PMS_MONITOR_VIOLATE_EN);
    REG_WRITE(SENSITIVE_CORE_0_DRAM0_PMS_MONITOR_1_REG, SENSITIVE_PMS_MONITOR_VIOLATE_CLR);
    REG_WRITE(SENSITIVE_CORE_0_DRAM0_PMS_MONITOR_1_REG, SENSITIVE_PMS_MONITOR_VIOLATE_EN);
    REG_WRITE(SENSITIVE_CORE_1_DRAM0_PMS_MONITOR_1_REG, SENSITIVE_PMS_MONITOR_VIOLATE_EN);

    uintptr_t iram_base = (uintptr_t)_iram_start;
    size_t iram_size = (size_t)((uintptr_t)_iram_end - (uintptr_t)_iram_start);
    if (iram_size == 0) {
        iram_size = 0x18000u;
    }

    uintptr_t dram_base = (uintptr_t)_dram_start;
    size_t dram_size = (size_t)((uintptr_t)_dram_end - (uintptr_t)_dram_start);
    if (dram_size == 0) {
        dram_size = 0x10000u;
    }

    /* Region 0: Kernel IRAM (_iram_start.._iram_end) -> World 0: RX, World 1: NONE */
    pms_configure_region(PMS_REGION_KERNEL_IRAM, iram_base, iram_size, PMS_PERM_RX, PMS_PERM_NONE);

    /* Region 1: Kernel DRAM (_dram_start.._dram_end) -> World 0: RW, World 1: NONE */
    pms_configure_region(PMS_REGION_KERNEL_DRAM, dram_base, dram_size, PMS_PERM_RW, PMS_PERM_NONE);
#else
    /* Host simulation boundaries consistent with ESP32-S3 memory map */
    pms_configure_region(PMS_REGION_KERNEL_IRAM, 0x40378000u, 0x18000u, PMS_PERM_RX, PMS_PERM_NONE);
    pms_configure_region(PMS_REGION_KERNEL_DRAM, 0x3FC88000u, 0x10000u, PMS_PERM_RW, PMS_PERM_NONE);
#endif

    pms_sync_hardware();
}

bool pms_configure_region(uint32_t region_id, uintptr_t base_addr, size_t size, uint32_t world0_perm, uint32_t world1_perm) {
    if (region_id >= PMS_MAX_REGIONS || size == 0) {
        return false;
    }

    s_pms_regions[region_id].region_id = region_id;
    s_pms_regions[region_id].base_addr = base_addr;
    s_pms_regions[region_id].size = size;
    s_pms_regions[region_id].world0_perm = world0_perm;
    s_pms_regions[region_id].world1_perm = world1_perm;
    s_pms_regions[region_id].active = true;

    pms_sync_hardware();
    return true;
}

void pms_clear_region(uint32_t region_id) {
    if (region_id >= PMS_MAX_REGIONS) {
        return;
    }
    s_pms_regions[region_id].base_addr = 0;
    s_pms_regions[region_id].size = 0;
    s_pms_regions[region_id].world0_perm = PMS_PERM_NONE;
    s_pms_regions[region_id].world1_perm = PMS_PERM_NONE;
    s_pms_regions[region_id].active = false;

    pms_sync_hardware();
}

void pms_apply_task_region(uintptr_t stack_base, size_t stack_size, uintptr_t extra_base, size_t extra_size) {
    if (stack_base != 0u && stack_size > 0u) {
        s_pms_regions[PMS_REGION_TASK_STACK].region_id = PMS_REGION_TASK_STACK;
        s_pms_regions[PMS_REGION_TASK_STACK].base_addr = stack_base;
        s_pms_regions[PMS_REGION_TASK_STACK].size = stack_size;
        s_pms_regions[PMS_REGION_TASK_STACK].world0_perm = PMS_PERM_RW;
        s_pms_regions[PMS_REGION_TASK_STACK].world1_perm = PMS_PERM_RW;
        s_pms_regions[PMS_REGION_TASK_STACK].active = true;
    } else {
        s_pms_regions[PMS_REGION_TASK_STACK].active = false;
    }

    if (extra_base != 0u && extra_size > 0u) {
        s_pms_regions[PMS_REGION_TASK_EXTRA].region_id = PMS_REGION_TASK_EXTRA;
        s_pms_regions[PMS_REGION_TASK_EXTRA].base_addr = extra_base;
        s_pms_regions[PMS_REGION_TASK_EXTRA].size = extra_size;
        s_pms_regions[PMS_REGION_TASK_EXTRA].world0_perm = PMS_PERM_RW;
        s_pms_regions[PMS_REGION_TASK_EXTRA].world1_perm = PMS_PERM_RW;
        s_pms_regions[PMS_REGION_TASK_EXTRA].active = true;
    } else {
        s_pms_regions[PMS_REGION_TASK_EXTRA].active = false;
    }

    pms_sync_hardware();
}

void pms_apply_task_isolation(const struct tcb *task) {
    if (!task) {
        s_pms_regions[PMS_REGION_TASK_STACK].active = false;
        s_pms_regions[PMS_REGION_TASK_EXTRA].active = false;
        pms_sync_hardware();
        return;
    }

    if (task->world == RADIX_WORLD_1_USER) {
        pms_apply_task_region(task->pms.stack_base, task->pms.stack_size,
                              task->pms.user_data_base, task->pms.user_data_size);
        return;
    }

    /* World 0 kernel task: protect its stack from World 1 access */
    if (task->pms.stack_base != 0u && task->pms.stack_size > 0u) {
        s_pms_regions[PMS_REGION_TASK_STACK].region_id = PMS_REGION_TASK_STACK;
        s_pms_regions[PMS_REGION_TASK_STACK].base_addr = task->pms.stack_base;
        s_pms_regions[PMS_REGION_TASK_STACK].size = task->pms.stack_size;
        s_pms_regions[PMS_REGION_TASK_STACK].world0_perm = PMS_PERM_RW;
        s_pms_regions[PMS_REGION_TASK_STACK].world1_perm = PMS_PERM_NONE;
        s_pms_regions[PMS_REGION_TASK_STACK].active = true;
    } else {
        s_pms_regions[PMS_REGION_TASK_STACK].active = false;
    }

    s_pms_regions[PMS_REGION_TASK_EXTRA].active = false;
    pms_sync_hardware();
}

bool pms_check_access(radix_world_t world, uintptr_t addr, size_t len, uint32_t req_perm) {
    if (len == 0 || req_perm == PMS_PERM_NONE) {
        return true;
    }

    /* Guard against integer wrap-around */
    if (addr + len < addr) {
        return false;
    }

    uintptr_t end_addr = addr + len;
    const pms_region_t *best = NULL;

    /*
     * Resolve against the most specific (smallest) active region containing [addr, end_addr).
     * Tie-break toward higher region_id so per-task/user regions override broad kernel catch-alls.
     */
    for (uint32_t i = 0; i < PMS_MAX_REGIONS; i++) {
        const pms_region_t *r = &s_pms_regions[i];
        if (!r->active) {
            continue;
        }

        uintptr_t r_end = r->base_addr + r->size;
        if (r_end < r->base_addr) {
            continue;
        }

        if (addr >= r->base_addr && end_addr <= r_end) {
            if (!best || r->size <= best->size) {
                best = r;
            }
        }
    }

    if (!best) {
        return false;
    }

    uint32_t perms = (world == RADIX_WORLD_0_KERNEL) ? best->world0_perm : best->world1_perm;
    return ((perms & req_perm) == req_perm);
}

void pms_handle_violation_exception(uint32_t exccause, uintptr_t excvaddr) {
    g_pms_stats.pms_violations++;
    g_pms_stats.last_fault_addr = excvaddr;
    g_pms_stats.last_fault_exccause = exccause;

    tcb_t *curr = sched_get_current_task();
    const char *name = curr ? curr->name : "none";
    uint32_t task_id = curr ? curr->task_id : 0;

    kprintf("[PMS-FAULT] Access violation (cause=%u, addr=0x%x) in task '%s' (id=%u) -> terminating task\n",
            (unsigned int)exccause, (unsigned int)excvaddr, name, (unsigned int)task_id);

    if (curr && curr->task_id != 0u) {
        g_pms_stats.last_killed_task_id = curr->task_id;
        task_exit();
    }
}

const world_pms_stats_t *pms_get_stats(void) {
    return &g_pms_stats;
}
