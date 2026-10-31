/*
 * drivers/pms.c - ESP32-S3 Permission Management System (PMS / MPU) Driver
 *
 * Enforces hardware memory isolation between World 0 (Kernel) and World 1 (User Tasks).
 */

#include <radix/types.h>
#include <radix/pms.h>
#include <radix/sched.h>
#include <radix/kprintf.h>

#ifndef RADIX_HOST_TEST
#include <esp32s3_soc.h>
#endif

/* Global PMS regions table */
static pms_region_t s_pms_regions[PMS_MAX_REGIONS];

/* Telemetry and fault tracking stats */
world_pms_stats_t g_pms_stats = {0};

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
    /* Enable violation monitors in ESP32-S3 Sensitive peripheral */
    REG_WRITE(SENSITIVE_INTERNAL_SRAM_IRAM0_PMS_MONITOR_0_REG, 1);
    REG_WRITE(SENSITIVE_INTERNAL_SRAM_DRAM0_PMS_MONITOR_0_REG, 1);
    REG_WRITE(SENSITIVE_INTERNAL_SRAM_DRAM0_PMS_MONITOR_1_REG, 1);
#endif

    /* Configure default kernel isolation regions */
    /* Region 0: Kernel IRAM (0x40378000, 96 KB) -> World 0: RX, World 1: NONE */
    pms_configure_region(0, 0x40378000, 0x18000, PMS_PERM_RX, PMS_PERM_NONE);

    /* Region 1: Kernel DRAM (0x3FC88000, 64 KB) -> World 0: RW, World 1: NONE */
    pms_configure_region(1, 0x3FC88000, 0x10000, PMS_PERM_RW, PMS_PERM_NONE);
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

#ifndef RADIX_HOST_TEST
    REG_SET_BITS(SENSITIVE_INTERNAL_SRAM_USAGE_0_REG, BIT(region_id));
#endif

    return true;
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

    for (uint32_t i = 0; i < PMS_MAX_REGIONS; i++) {
        pms_region_t *r = &s_pms_regions[i];
        if (!r->active) {
            continue;
        }

        uintptr_t r_end = r->base_addr + r->size;
        if (r_end < r->base_addr) {
            continue;
        }

        /* Check if requested buffer [addr, end_addr) is fully contained in this region */
        if (addr >= r->base_addr && end_addr <= r_end) {
            uint32_t perms = (world == RADIX_WORLD_0_KERNEL) ? r->world0_perm : r->world1_perm;
            if ((perms & req_perm) == req_perm) {
                return true;
            }
            return false;
        }
    }

    return false;
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

    if (curr) {
        g_pms_stats.last_killed_task_id = curr->task_id;
        task_exit();
    }
}

const world_pms_stats_t *pms_get_stats(void) {
    return &g_pms_stats;
}
