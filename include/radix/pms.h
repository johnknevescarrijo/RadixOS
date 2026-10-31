#ifndef RADIX_PMS_H
#define RADIX_PMS_H

#include <radix/types.h>

/*
 * Execution Domains (World Controller)
 */
typedef enum {
    RADIX_WORLD_0_KERNEL = 0,
    RADIX_WORLD_1_USER   = 1,
} radix_world_t;

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

#define PMS_MAX_REGIONS 8

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

/* World Controller Interface */
void world_init(void);
radix_world_t world_get_current(void);
void world_switch_to_user(uintptr_t entry_or_return_pc);
void world_enter_kernel_from_trap(void);

/* PMS MPU Interface */
void pms_init(void);
bool pms_configure_region(uint32_t region_id, uintptr_t base_addr, size_t size, uint32_t world0_perm, uint32_t world1_perm);
bool pms_check_access(radix_world_t world, uintptr_t addr, size_t len, uint32_t req_perm);
void pms_handle_violation_exception(uint32_t exccause, uintptr_t excvaddr);
const world_pms_stats_t *pms_get_stats(void);

#endif /* RADIX_PMS_H */
