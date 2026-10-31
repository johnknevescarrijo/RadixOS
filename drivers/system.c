#include <radix/types.h>
#include <radix/amp.h>
#include <radix/atomic.h>

#ifdef RADIX_HOST_TEST

static uint32_t s_host_core_id = 0;
static uint32_t s_host_core1_boot_addr = 0;
static uint32_t s_host_core1_ctrl0 = 0x01; /* SYSTEM_CONTROL_CORE_1_RUNSTALL initially */

void host_set_current_core_id(uint32_t core_id) {
    s_host_core_id = core_id;
}

uint32_t arch_get_core_id(void) {
    return s_host_core_id;
}

uint32_t system_get_core1_boot_addr(void) {
    return s_host_core1_boot_addr;
}

uint32_t system_get_core1_ctrl0_reg(void) {
    return s_host_core1_ctrl0;
}

void system_boot_core1(void (*entry)(void)) {
    s_host_core1_boot_addr = (uint32_t)(uintptr_t)entry;

    /* Enable clock gate */
    s_host_core1_ctrl0 |= (1UL << 1); /* SYSTEM_CONTROL_CORE_1_CLKGATE_EN */

    /* Pulse reset line */
    s_host_core1_ctrl0 |= (1UL << 2); /* SYSTEM_CONTROL_CORE_1_RESETING */
    arch_memory_barrier();
    s_host_core1_ctrl0 &= ~(1UL << 2);

    /* Release run stall */
    s_host_core1_ctrl0 &= ~(1UL << 0); /* SYSTEM_CONTROL_CORE_1_RUNSTALL */
    arch_memory_barrier();

    amp_shared_state_t *st = amp_get_state();
    if (st) {
        st->core_state[RADIX_CORE_1] = CORE_STATE_BOOTING;
    }
}

void system_halt_core1(void) {
    /* Stall Core 1 and gate clock */
    s_host_core1_ctrl0 |= (1UL << 0);  /* SYSTEM_CONTROL_CORE_1_RUNSTALL */
    s_host_core1_ctrl0 &= ~(1UL << 1); /* SYSTEM_CONTROL_CORE_1_CLKGATE_EN */
    arch_memory_barrier();

    amp_shared_state_t *st = amp_get_state();
    if (st) {
        st->core_state[RADIX_CORE_1] = CORE_STATE_HALTED;
    }
}

#else

#include <esp32s3_soc.h>

uint32_t arch_get_core_id(void) {
    uint32_t prid;
    __asm__ volatile ("rsr %0, 235" : "=r"(prid));
    return (prid >> 13) & 1u;
}

void system_boot_core1(void (*entry)(void)) {
    /* Set boot address in hardware system control */
    REG_WRITE(SYSTEM_CORE_1_BOOT_ADDR_REG, (uint32_t)(uintptr_t)entry);

    /* Enable clock gate for Core 1 */
    REG_SET_BITS(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_CLKGATE_EN);

    /* Assert reset pulse */
    REG_SET_BITS(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_RESETING);
    arch_memory_barrier();

    /* Release reset pulse */
    REG_CLR_BITS(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_RESETING);

    /* Clear run-stall to begin execution */
    REG_CLR_BITS(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_RUNSTALL);
    arch_memory_barrier();

    amp_shared_state_t *st = amp_get_state();
    if (st) {
        st->core_state[RADIX_CORE_1] = CORE_STATE_BOOTING;
    }
}

void system_halt_core1(void) {
    /* Stall Core 1 and disable clock gate */
    REG_SET_BITS(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_RUNSTALL);
    REG_CLR_BITS(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_CLKGATE_EN);
    arch_memory_barrier();

    amp_shared_state_t *st = amp_get_state();
    if (st) {
        st->core_state[RADIX_CORE_1] = CORE_STATE_HALTED;
    }
}

#endif
