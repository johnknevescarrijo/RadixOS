/*
 * drivers/world.c - ESP32-S3 World Controller Driver
 *
 * Implements hardware execution domain transitions between World 0 (Kernel/Supervisor)
 * and World 1 (User/Restricted).
 */

#include <radix/types.h>
#include <radix/pms.h>
#include <radix/amp.h>

#ifndef RADIX_HOST_TEST
#include <esp32s3_soc.h>
#endif

extern world_pms_stats_t g_pms_stats;

/* Per-core current execution world */
static radix_world_t s_current_world[2] = { RADIX_WORLD_0_KERNEL, RADIX_WORLD_0_KERNEL };

void world_init(void) {
    uint32_t core_id = arch_get_core_id();
    if (core_id > 1) {
        core_id = 0;
    }
    s_current_world[core_id] = RADIX_WORLD_0_KERNEL;

#ifndef RADIX_HOST_TEST
    if (core_id == 0) {
        REG_WRITE(WCL_CORE_0_WORLD_CANCEL_REG, 1);
    }
#endif
}

radix_world_t world_get_current(void) {
    uint32_t core_id = arch_get_core_id();
    if (core_id > 1) {
        core_id = 0;
    }
    return s_current_world[core_id];
}

void world_switch_to_user(uintptr_t entry_or_return_pc) {
    uint32_t core_id = arch_get_core_id();
    if (core_id > 1) {
        core_id = 0;
    }

#ifndef RADIX_HOST_TEST
    if (core_id == 0) {
        REG_WRITE(WCL_CORE_0_WORLD_IRAM0_REG, (uint32_t)entry_or_return_pc);
        REG_WRITE(WCL_CORE_0_WORLD_PREPARE_REG, (1u << 1)); /* Target World 1 */
        REG_WRITE(WCL_CORE_0_WORLD_UPDATE_REG, 1);
    }
#else
    (void)entry_or_return_pc;
#endif

    s_current_world[core_id] = RADIX_WORLD_1_USER;
    g_pms_stats.world0_to_world1_switches++;
}

void world_enter_kernel_from_trap(void) {
    uint32_t core_id = arch_get_core_id();
    if (core_id > 1) {
        core_id = 0;
    }

    s_current_world[core_id] = RADIX_WORLD_0_KERNEL;

#ifndef RADIX_HOST_TEST
    if (core_id == 0) {
        REG_WRITE(WCL_CORE_0_WORLD_CANCEL_REG, 1);
    }
#endif

    g_pms_stats.world1_to_world0_traps++;
}
