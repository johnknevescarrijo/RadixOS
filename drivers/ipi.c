#include <radix/types.h>
#include <radix/amp.h>
#include <radix/atomic.h>

#ifdef RADIX_HOST_TEST

static uint32_t s_host_cpu_intr[RADIX_NUM_CORES] = {0, 0};

uint32_t ipi_get_hardware_intr_reg(uint32_t core_id) {
    if (core_id < RADIX_NUM_CORES) {
        return s_host_cpu_intr[core_id];
    }
    return 0;
}

void ipi_init(void) {
    s_host_cpu_intr[0] = 0;
    s_host_cpu_intr[1] = 0;
}

static inline void ipi_hw_trigger(uint32_t target_core) {
    if (target_core < RADIX_NUM_CORES) {
        s_host_cpu_intr[target_core] = 1;
    }
}

static inline void ipi_hw_clear(uint32_t current_core) {
    if (current_core < RADIX_NUM_CORES) {
        s_host_cpu_intr[current_core] = 0;
    }
}

#else

#include <esp32s3_soc.h>

void ipi_init(void) {
    REG_WRITE(SYSTEM_CPU_INTR_FROM_CPU_0_REG, 0);
    REG_WRITE(SYSTEM_CPU_INTR_FROM_CPU_1_REG, 0);
}

static inline void ipi_hw_trigger(uint32_t target_core) {
    if (target_core == RADIX_CORE_0) {
        REG_WRITE(SYSTEM_CPU_INTR_FROM_CPU_0_REG, SYSTEM_CPU_INTR_FROM_CPU_0);
    } else {
        REG_WRITE(SYSTEM_CPU_INTR_FROM_CPU_1_REG, SYSTEM_CPU_INTR_FROM_CPU_1);
    }
}

static inline void ipi_hw_clear(uint32_t current_core) {
    if (current_core == RADIX_CORE_0) {
        REG_WRITE(SYSTEM_CPU_INTR_FROM_CPU_0_REG, 0);
    } else {
        REG_WRITE(SYSTEM_CPU_INTR_FROM_CPU_1_REG, 0);
    }
}

#endif

bool ipi_send(uint32_t target_core, uint32_t reason) {
    if (target_core >= RADIX_NUM_CORES || reason == 0) {
        return false;
    }

    amp_shared_state_t *st = amp_get_state();
    if (!st) {
        return false;
    }

    /* Lock-free atomic bitwise OR into target core pending_ipi register */
    volatile uint32_t *target_pending = &st->pending_ipi[target_core];
    uint32_t expected;
    uint32_t desired;
    do {
        expected = *target_pending;
        desired = expected | reason;
    } while (!arch_atomic_cas(target_pending, expected, desired));

    /* Increment TX counter for current core */
    uint32_t current_core = arch_get_core_id();
    if (current_core < RADIX_NUM_CORES) {
        arch_atomic_fetch_add(&st->ipi_tx_count[current_core], 1);
    }

    arch_memory_barrier();

    /* Trigger hardware IPI interrupt */
    ipi_hw_trigger(target_core);

    return true;
}

uint32_t ipi_handle_irq(void) {
    uint32_t core_id = arch_get_core_id();
    if (core_id >= RADIX_NUM_CORES) {
        return 0;
    }

    /* Clear hardware interrupt trigger */
    ipi_hw_clear(core_id);

    amp_shared_state_t *st = amp_get_state();
    if (!st) {
        return 0;
    }

    /* Atomically fetch and clear pending IPI reasons */
    uint32_t reasons = arch_atomic_swap(&st->pending_ipi[core_id], 0);
    if (reasons == 0) {
        return 0;
    }

    /* Increment RX counter for this core */
    arch_atomic_fetch_add(&st->ipi_rx_count[core_id], 1);

    /* 1. Reschedule request */
    if (reasons & IPI_REASON_RESCHEDULE) {
        arch_atomic_fetch_add(&st->resched_requests[core_id], 1);
    }

    /* 2. Remote function call invocation */
    if (reasons & IPI_REASON_CALL_FUNC) {
        amp_call_slot_t *slot = &st->call_mailbox[core_id];
        if (slot->fn != NULL) {
            slot->fn(slot->arg);
            arch_memory_barrier();
            slot->done = 1;
            arch_memory_barrier();
        }
    }

    /* 3. IPC notify & Ping reasons returned to caller */
    return reasons;
}
