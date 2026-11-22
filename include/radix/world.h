#ifndef RADIX_WORLD_H
#define RADIX_WORLD_H

#include <radix/types.h>

/*
 * Execution Domains (ESP32-S3 World Controller)
 */
typedef enum {
    RADIX_WORLD_0_KERNEL = 0,
    RADIX_WORLD_1_USER   = 1,
} radix_world_t;

typedef radix_world_t world_id_t;

#define WORLD_0 RADIX_WORLD_0_KERNEL
#define WORLD_1 RADIX_WORLD_1_USER

/* World Controller Interface */
void world_init(void);
radix_world_t world_get_current(void);
void world_set_current(radix_world_t world);
void world_switch_to_user(uintptr_t entry_or_return_pc);
void world_enter_kernel_from_trap(void);
radix_world_t world_capture_and_enter_kernel(void);
radix_world_t world_get_hardware_world(void);

#endif /* RADIX_WORLD_H */
