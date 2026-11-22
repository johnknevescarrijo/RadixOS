#ifndef RADIX_PMS_H
#define RADIX_PMS_H

#include <radix/types.h>
#include <radix/world.h>

/* Forward declaration */
struct tcb;

/*
 * PMS Memory Protection Permissions
 */
#define PMS_PERM_NONE 0
#define PMS_PERM_R    (1u << 0)
#define PMS_PERM_W    (1u << 1)
#define PMS_PERM_X    (1u << 2)
#define PMS_PERM_RW   (PMS_PERM_R | PMS_PERM_W)
#define PMS_PERM_RX   (PMS_PERM_R | PMS_PERM_X)
#define PMS_PERM_RWX  (PMS_PERM_R | PMS_PERM_W | PMS_PERM_X)

#define PMS_MAX_REGIONS 8u

/* Reserved PMS Region Indices */
#define PMS_REGION_KERNEL_IRAM  0u
#define PMS_REGION_KERNEL_DRAM  1u
#define PMS_REGION_TASK_STACK   2u
#define PMS_REGION_TASK_EXTRA   5u

/*
 * Region descriptor for Permission Management System
 */
typedef struct {
    uint32_t region_id;
    uintptr_t base_addr;
    size_t size;
    uint32_t world0_perm;
    uint32_t world1_perm;
    bool active;
} pms_region_t;

/*
 * Snapshot of programmed ESP32-S3 PMS hardware registers (and host shadow)
 */
typedef struct {
    uint32_t split_line_ctrl0;
    uint32_t split_line_main_id;
    uint32_t split_line_iram_0;
    uint32_t split_line_iram_1;
    uint32_t split_line_dram_0;
    uint32_t split_line_dram_1;
    uint32_t iram0_pms_ctrl0;
    uint32_t iram0_pms_world1;
    uint32_t iram0_pms_world0;
    uint32_t dram0_pms_ctrl0;
    uint32_t dram0_pms_world01;
    uint32_t iram0_monitor_ctrl1;
    uint32_t dram0_monitor_ctrl1;
} pms_hw_regs_t;

/*
 * Telemetry and statistics for World Controller and PMS
 */
typedef struct {
    uint32_t world0_to_world1_switches;
    uint32_t world1_to_world0_traps;
    uint32_t pms_violations;
    uintptr_t last_fault_addr;
    uint32_t last_fault_exccause;
    uint32_t last_killed_task_id;
} world_pms_stats_t;

/* PMS MPU Interface */
void pms_init(void);
bool pms_configure_region(uint32_t region_id, uintptr_t base_addr, size_t size, uint32_t world0_perm, uint32_t world1_perm);
void pms_clear_region(uint32_t region_id);
bool pms_check_access(radix_world_t world, uintptr_t addr, size_t len, uint32_t req_perm);
void pms_apply_task_region(uintptr_t stack_base, size_t stack_size, uintptr_t extra_base, size_t extra_size);
void pms_sync_hardware(void);
const pms_hw_regs_t *pms_get_hw_regs(void);
void pms_handle_violation_exception(uint32_t exccause, uintptr_t excvaddr);
const world_pms_stats_t *pms_get_stats(void);

#endif /* RADIX_PMS_H */
