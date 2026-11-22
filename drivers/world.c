/*
 * drivers/world.c - ESP32-S3 World Controller Driver
 *
 * Implements hardware execution domain transitions between World 0 (Kernel/Supervisor)
 * and World 1 (User/Restricted).
 */

#include <radix/types.h>
#include <radix/world.h>
#include <radix/pms.h>
#include <radix/amp.h>

#ifndef RADIX_HOST_TEST
#include <esp32s3_soc.h>
extern void _user_exception_vector(void);
#else
extern uint32_t arch_get_core_id(void) __attribute__((weak));
#endif

extern world_pms_stats_t g_pms_stats;

/* Per-core current execution world */
static radix_world_t s_current_world[2] = { RADIX_WORLD_0_KERNEL, RADIX_WORLD_0_KERNEL };

static uint32_t world_safe_core_id(void) {
#ifdef RADIX_HOST_TEST
    uint32_t core_id = arch_get_core_id ? arch_get_core_id() : 0u;
#else
    uint32_t core_id = arch_get_core_id();
#endif
    return (core_id > 1u) ? 0u : core_id;
}

void world_init(void) {
    uint32_t core_id = world_safe_core_id();
    s_current_world[core_id] = RADIX_WORLD_0_KERNEL;

#ifndef RADIX_HOST_TEST
    if (core_id == 0u) {
        REG_WRITE(WCL_CORE_0_ENTRY_1_ADDR_REG, (uint32_t)(uintptr_t)_user_exception_vector);
        REG_WRITE(WCL_CORE_0_WORLD_CANCEL_REG, 1u);
    } else {
        REG_WRITE(WCL_CORE_1_ENTRY_1_ADDR_REG, (uint32_t)(uintptr_t)_user_exception_vector);
        REG_WRITE(WCL_CORE_1_WORLD_CANCEL_REG, 1u);
    }
#endif
}

radix_world_t world_get_current(void) {
    uint32_t core_id = world_safe_core_id();
    return s_current_world[core_id];
}

radix_world_t world_get_hardware_world(void) {
    uint32_t core_id = world_safe_core_id();
#ifndef RADIX_HOST_TEST
    uint32_t hw_reg = (core_id == 0u)
        ? REG_READ(WCL_CORE_0_WORLD_DRAM0_PIF_REG)
        : REG_READ(WCL_CORE_1_WORLD_DRAM0_PIF_REG);
    if ((hw_reg & WCL_WORLD_BUS_MASK) == WCL_WORLD_PREPARE_WORLD_1) {
        return RADIX_WORLD_1_USER;
    }
    if ((hw_reg & WCL_WORLD_BUS_MASK) == WCL_WORLD_PREPARE_WORLD_0) {
        return RADIX_WORLD_0_KERNEL;
    }
#endif
    return s_current_world[core_id];
}

void world_set_current(radix_world_t world) {
    uint32_t core_id = world_safe_core_id();
    s_current_world[core_id] = world;

#ifndef RADIX_HOST_TEST
    if (world == RADIX_WORLD_0_KERNEL) {
        if (core_id == 0u) {
            REG_WRITE(WCL_CORE_0_WORLD_CANCEL_REG, 1u);
        } else {
            REG_WRITE(WCL_CORE_1_WORLD_CANCEL_REG, 1u);
        }
    }
#endif
}

void world_switch_to_user(uintptr_t entry_or_return_pc) {
    uint32_t core_id = world_safe_core_id();

#ifndef RADIX_HOST_TEST
    if (core_id == 0u) {
        REG_WRITE(WCL_CORE_0_WORLD_TRIGGER_ADDR_REG, (uint32_t)entry_or_return_pc);
        REG_WRITE(WCL_CORE_0_WORLD_PREPARE_REG, WCL_WORLD_PREPARE_WORLD_1);
        REG_WRITE(WCL_CORE_0_WORLD_UPDATE_REG, 1u);
    } else {
        REG_WRITE(WCL_CORE_1_WORLD_TRIGGER_ADDR_REG, (uint32_t)entry_or_return_pc);
        REG_WRITE(WCL_CORE_1_WORLD_PREPARE_REG, WCL_WORLD_PREPARE_WORLD_1);
        REG_WRITE(WCL_CORE_1_WORLD_UPDATE_REG, 1u);
    }
#else
    (void)entry_or_return_pc;
#endif

    s_current_world[core_id] = RADIX_WORLD_1_USER;
    g_pms_stats.world0_to_world1_switches++;
}

void world_enter_kernel_from_trap(void) {
    uint32_t core_id = world_safe_core_id();
    radix_world_t prev = s_current_world[core_id];

    s_current_world[core_id] = RADIX_WORLD_0_KERNEL;

#ifndef RADIX_HOST_TEST
    if (core_id == 0u) {
        REG_WRITE(WCL_CORE_0_WORLD_CANCEL_REG, 1u);
    } else {
        REG_WRITE(WCL_CORE_1_WORLD_CANCEL_REG, 1u);
    }
#endif

    if (prev == RADIX_WORLD_1_USER) {
        g_pms_stats.world1_to_world0_traps++;
    }
}

radix_world_t world_capture_and_enter_kernel(void) {
    uint32_t core_id = world_safe_core_id();
    radix_world_t prev = s_current_world[core_id];

#ifndef RADIX_HOST_TEST
    uint32_t hw_reg = (core_id == 0u)
        ? REG_READ(WCL_CORE_0_WORLD_DRAM0_PIF_REG)
        : REG_READ(WCL_CORE_1_WORLD_DRAM0_PIF_REG);
    if ((hw_reg & WCL_WORLD_BUS_MASK) == WCL_WORLD_PREPARE_WORLD_1) {
        prev = RADIX_WORLD_1_USER;
    }
#endif

    s_current_world[core_id] = prev;
    world_enter_kernel_from_trap();
    return prev;
}
